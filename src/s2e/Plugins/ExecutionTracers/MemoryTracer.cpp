///
/// Copyright (C) 2010-2016, Dependable Systems Laboratory, EPFL
/// Copyright (C) 2015-2016, Cyberhaven
/// All rights reserved.
///
/// Licensed under the Cyberhaven Research License Agreement.
///

#include <s2e/opcodes.h>

#include <s2e/ConfigFile.h>
#include <s2e/S2E.h>
#include <s2e/S2EExecutionState.h>
#include <s2e/S2EExecutor.h>
#include <s2e/Utils.h>

#include <llvm/Support/CommandLine.h>

#include <inttypes.h>
#include <iomanip>

#include <TraceEntries.pb.h>

#include "MemoryTracer.h"

extern llvm::cl::opt<bool> ConcolicMode;

namespace s2e {
namespace plugins {

S2E_DEFINE_PLUGIN(MemoryTracer, "Memory tracer plugin", "MemoryTracer", "ExecutionTracer");

MemoryTracer::MemoryTracer(S2E *s2e) : Plugin(s2e) {
}

void MemoryTracer::initialize() {
    m_tracer = s2e()->getPlugin<ExecutionTracer>();
    m_execDetector = s2e()->getPlugin<ModuleExecutionDetector>();

    // Retrict monitoring to configured modules only
    m_monitorModules = s2e()->getConfig()->getBool(getConfigKey() + ".monitorModules");
    if (m_monitorModules && !m_execDetector) {
        getWarningsStream() << "MemoryTracer: The monitorModules option requires ModuleExecutionDetector\n";
        exit(-1);
    }

    // Catch all accesses to the stack
    m_monitorStack = s2e()->getConfig()->getBool(getConfigKey() + ".monitorStack");

    // Catch accesses that are above the specified address
    m_catchAbove = s2e()->getConfig()->getInt(getConfigKey() + ".catchAccessesAbove");
    m_catchBelow = s2e()->getConfig()->getInt(getConfigKey() + ".catchAccessesBelow");

    // Whether or not to include host addresses in the trace.
    // This is useful for debugging, bug yields larger traces
    m_traceHostAddresses = s2e()->getConfig()->getBool(getConfigKey() + ".traceHostAddresses");

    // Check that the current state is actually allowed to write to
    // the object state. Can be useful to debug the engine.
    m_debugObjectStates = s2e()->getConfig()->getBool(getConfigKey() + ".debugObjectStates");

    // Start monitoring after the specified number of seconds
    bool hasTimeTrigger = false;
    m_timeTrigger = s2e()->getConfig()->getInt(getConfigKey() + ".timeTrigger", 0, &hasTimeTrigger);
    m_elapsedTics = 0;

    bool manualMode = s2e()->getConfig()->getBool(getConfigKey() + ".manualTrigger");

    m_monitorMemory = s2e()->getConfig()->getBool(getConfigKey() + ".monitorMemory");
    m_monitorPageFaults = s2e()->getConfig()->getBool(getConfigKey() + ".monitorPageFaults");
    m_monitorTlbMisses = s2e()->getConfig()->getBool(getConfigKey() + ".monitorTlbMisses");

    getDebugStream() << "MonitorMemory: " << m_monitorMemory << " PageFaults: " << m_monitorPageFaults
                     << " TlbMisses: " << m_monitorTlbMisses << '\n';

    if (hasTimeTrigger) {
        m_timerConnection = s2e()->getCorePlugin()->onTimer.connect(sigc::mem_fun(*this, &MemoryTracer::onTimer));
    } else if (manualMode) {
        s2e()->getCorePlugin()->onCustomInstruction.connect(sigc::mem_fun(*this, &MemoryTracer::onCustomInstruction));
    } else {
        enableTracing();
    }
}

bool MemoryTracer::decideTracing(S2EExecutionState *state) {
    if (m_catchAbove || m_catchBelow) {
        if (m_catchAbove && (m_catchAbove >= state->regs()->getPc())) {
            return false;
        }
        if (m_catchBelow && (m_catchBelow < state->regs()->getPc())) {
            return false;
        }
    }

    return true;
}

void MemoryTracer::traceConcreteDataMemoryAccess(S2EExecutionState *state, uint64_t address, uint64_t value,
                                                 uint8_t size, unsigned flags) {
    if (!decideTracing(state)) {
        return;
    }

    s2e_trace::PbTraceMemoryAccess item;
    item.set_pc(state->regs()->getPc());

    item.set_address(address);
    item.set_value(value);
    item.set_size(size);
    item.set_host_address(0);

    uint32_t traceFlags = 0;

    if (flags & MEM_TRACE_FLAG_WRITE) {
        traceFlags |= s2e_trace::PbTraceMemoryAccess::EXECTRACE_MEM_WRITE;
    }

    if (flags & MEM_TRACE_FLAG_IO) {
        traceFlags |= s2e_trace::PbTraceMemoryAccess::EXECTRACE_MEM_IO;
    }

    item.set_flags(s2e_trace::PbTraceMemoryAccess::Flags(traceFlags));

    m_tracer->writeData(state, item, s2e_trace::TRACE_MEMORY);
}

void MemoryTracer::traceSymbolicDataMemoryAccess(S2EExecutionState *state, klee::ref<klee::Expr> &address,
                                                 klee::ref<klee::Expr> &hostAddress, klee::ref<klee::Expr> &value,
                                                 unsigned flags) {
    if (!decideTracing(state)) {
        return;
    }

    bool isAddrCste = isa<klee::ConstantExpr>(address);
    bool isValCste = isa<klee::ConstantExpr>(value);
    bool isHostAddrCste = isa<klee::ConstantExpr>(hostAddress);

    uint32_t traceFlags = 0;

    s2e_trace::PbTraceMemoryAccess item;
    item.set_pc(state->regs()->getPc());

    uint64_t concreteAddress = 0xdeadbeef;
    uint64_t concreteValue = 0xdeadbeef;
    if (ConcolicMode) {
        klee::ref<klee::ConstantExpr> ce = dyn_cast<klee::ConstantExpr>(state->concolics->evaluate(address));
        concreteAddress = ce->getZExtValue();

        ce = dyn_cast<klee::ConstantExpr>(state->concolics->evaluate(value));
        concreteValue = ce->getZExtValue();
    }

    item.set_address(isAddrCste ? cast<klee::ConstantExpr>(address)->getZExtValue(64) : concreteAddress);
    item.set_value(isValCste ? cast<klee::ConstantExpr>(value)->getZExtValue(64) : concreteValue);
    item.set_size(klee::Expr::getMinBytesForWidth(value->getWidth()));

    if (flags & MEM_TRACE_FLAG_WRITE) {
        traceFlags |= s2e_trace::PbTraceMemoryAccess::EXECTRACE_MEM_WRITE;
    }

    if (flags & MEM_TRACE_FLAG_IO) {
        traceFlags |= s2e_trace::PbTraceMemoryAccess::EXECTRACE_MEM_IO;
    }

    item.set_host_address(isHostAddrCste ? cast<klee::ConstantExpr>(hostAddress)->getZExtValue(64) : 0xDEADBEEF);

    if (m_traceHostAddresses) {
        traceFlags |= s2e_trace::PbTraceMemoryAccess::EXECTRACE_MEM_HASHOSTADDR;
        traceFlags |= s2e_trace::PbTraceMemoryAccess::EXECTRACE_MEM_OBJECTSTATE;

        klee::ObjectPair op = state->addressSpace.findObject(item.host_address() & SE_RAM_OBJECT_MASK);
        if (op.first && op.second) {
            item.set_concrete_buffer((uint64_t) op.second->getConcreteStore());
            if ((flags & MEM_TRACE_FLAG_WRITE) && m_debugObjectStates) {
                assert(state->addressSpace.isOwnedByUs(op.second));
            }
        }
    }

    if (!isAddrCste) {
        traceFlags |= s2e_trace::PbTraceMemoryAccess::EXECTRACE_MEM_SYMBADDR;
    }

    if (!isValCste) {
        traceFlags |= s2e_trace::PbTraceMemoryAccess::EXECTRACE_MEM_SYMBVAL;
    }

    if (!isHostAddrCste) {
        traceFlags |= s2e_trace::PbTraceMemoryAccess::EXECTRACE_MEM_SYMBHOSTADDR;
    }

    item.set_flags(s2e_trace::PbTraceMemoryAccess::Flags(traceFlags));

    m_tracer->writeData(state, item, s2e_trace::TRACE_MEMORY);
}

bool MemoryTracer::forceDisconnect(S2EExecutionState *state) {
    // XXX: This is a hack.
    // Sometimes the onModuleTransition is not fired properly...
    if (m_execDetector && m_monitorModules && !m_execDetector->getCurrentDescriptor(state)) {
        disconnectMemoryTracing();
        return true;
    }
    return false;
}

void MemoryTracer::onAfterSymbolicDataMemoryAccess(S2EExecutionState *state, klee::ref<klee::Expr> address,
                                                   klee::ref<klee::Expr> hostAddress, klee::ref<klee::Expr> value,
                                                   unsigned flags) {
    if (forceDisconnect(state)) {
        return;
    }
    traceSymbolicDataMemoryAccess(state, address, hostAddress, value, flags);
}

void MemoryTracer::onConcreteDataMemoryAccess(S2EExecutionState *state, uint64_t address, uint64_t value, uint8_t size,
                                              unsigned flags) {
    if (forceDisconnect(state)) {
        return;
    }

    traceConcreteDataMemoryAccess(state, address, value, size, flags);
}

void MemoryTracer::onModuleTransition(S2EExecutionState *state, ModuleDescriptorConstPtr prevModule,
                                      ModuleDescriptorConstPtr nextModule) {
    if (nextModule && !m_symbolicMemoryMonitor.connected()) {
        connectMemoryTracing();
    } else {
        disconnectMemoryTracing();
    }
}

void MemoryTracer::onModuleTranslateBlockStart(ExecutionSignal *signal, S2EExecutionState *state,
                                               const ModuleDescriptor &module, TranslationBlock *tb, uint64_t pc) {
    signal->connect(sigc::mem_fun(*this, &MemoryTracer::onExecuteBlockStart));
}

void MemoryTracer::onExecuteBlockStart(S2EExecutionState *state, uint64_t pc) {
    connectMemoryTracing();
}

void MemoryTracer::onTlbMiss(S2EExecutionState *state, uint64_t addr, bool is_write) {
    s2e_trace::PbTraceSimpleMemoryAccess item;

    item.set_pc(state->regs()->getPc());
    item.set_address(addr);
    item.set_is_write(is_write);

    std::string data;
    if (!item.AppendToString(&data)) {
        return;
    }

    m_tracer->writeData(state, data.c_str(), data.size(), s2e_trace::TRACE_TLBMISS);
}

void MemoryTracer::onPageFault(S2EExecutionState *state, uint64_t addr, bool is_write) {
    s2e_trace::PbTraceSimpleMemoryAccess item;

    item.set_pc(state->regs()->getPc());
    item.set_address(addr);
    item.set_is_write(is_write);

    m_tracer->writeData(state, item, s2e_trace::TRACE_PAGEFAULT);
}

void MemoryTracer::enableTracing() {
    if (m_monitorMemory) {
        getInfoStream() << "MemoryTracer Plugin: Enabling memory tracing" << '\n';
        m_symbolicMemoryMonitor.disconnect();

        if (m_monitorModules) {
            m_execDetector->onModuleTransition.connect(sigc::mem_fun(*this, &MemoryTracer::onModuleTransition));
            m_execDetector->onModuleTranslateBlockStart.connect(
                sigc::mem_fun(*this, &MemoryTracer::onModuleTranslateBlockStart));
        } else {
            connectMemoryTracing();
        }
    }

    if (m_monitorPageFaults) {
        getInfoStream() << "MemoryTracer Plugin: Enabling page fault tracing" << '\n';
        m_pageFaultsMonitor.disconnect();
        m_pageFaultsMonitor =
            s2e()->getCorePlugin()->onPageFault.connect(sigc::mem_fun(*this, &MemoryTracer::onPageFault));
    }

    if (m_monitorTlbMisses) {
        getInfoStream() << "MemoryTracer Plugin: Enabling TLB miss tracing" << '\n';
        m_tlbMissesMonitor.disconnect();
        m_tlbMissesMonitor = s2e()->getCorePlugin()->onTlbMiss.connect(sigc::mem_fun(*this, &MemoryTracer::onTlbMiss));
    }
}

void MemoryTracer::connectMemoryTracing() {
    if (!m_symbolicMemoryMonitor.connected()) {
        m_symbolicMemoryMonitor = s2e()->getCorePlugin()->onAfterSymbolicDataMemoryAccess.connect(
            sigc::mem_fun(*this, &MemoryTracer::onAfterSymbolicDataMemoryAccess));
    }

    if (!m_concreteMemoryMonitor.connected()) {
        m_concreteMemoryMonitor = s2e()->getCorePlugin()->onConcreteDataMemoryAccess.connect(
            sigc::mem_fun(*this, &MemoryTracer::onConcreteDataMemoryAccess));
    }
}

void MemoryTracer::disconnectMemoryTracing() {
    m_symbolicMemoryMonitor.disconnect();
    m_concreteMemoryMonitor.disconnect();
}

void MemoryTracer::disableTracing() {
    disconnectMemoryTracing();
    m_pageFaultsMonitor.disconnect();
    m_tlbMissesMonitor.disconnect();
}

bool MemoryTracer::tracingEnabled() {
    return m_symbolicMemoryMonitor.connected() || m_concreteMemoryMonitor.connected();
}

void MemoryTracer::onTimer() {
    if (m_elapsedTics++ < m_timeTrigger) {
        return;
    }

    enableTracing();

    m_timerConnection.disconnect();
}

void MemoryTracer::onCustomInstruction(S2EExecutionState *state, uint64_t opcode) {
    if (!OPCODE_CHECK(opcode, MEMORY_TRACER_OPCODE)) {
        return;
    }

    // XXX: remove this mess. Should have a function for extracting
    // info from opcodes.
    opcode >>= 16;
    uint8_t op = opcode & 0xFF;
    opcode >>= 8;

    MemoryTracerOpcodes opc = (MemoryTracerOpcodes) op;
    switch (opc) {
        case Enable:
            enableTracing();
            break;

        case Disable:
            disableTracing();
            break;

        default:
            getWarningsStream() << "MemoryTracer: unsupported opcode " << hexval(opc) << '\n';
            break;
    }
}

bool MemoryTracer::getProperty(S2EExecutionState *state, const std::string &name, std::string &value) {
    return false;
}

bool MemoryTracer::setProperty(S2EExecutionState *state, const std::string &name, const std::string &value) {
    if (name == "trace") {
        if (value == "1") {
            enableTracing();
        } else {
            disableTracing();
        }
        return true;
    }
    return false;
}
}
}
