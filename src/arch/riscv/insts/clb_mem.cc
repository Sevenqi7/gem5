/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "arch/riscv/insts/clb_mem.hh"

#include <array>
#include <sstream>
#include <type_traits>
#include <vector>

#include "arch/riscv/clb.hh"
#include "arch/riscv/faults.hh"
#include "arch/riscv/mmu.hh"
#include "arch/riscv/regs/int.hh"
#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/utility.hh"
#include "base/cprintf.hh"
#include "base/logging.hh"
#include "cpu/exec_context.hh"
#include "cpu/op_class.hh"
#include "cpu/thread_context.hh"

namespace gem5
{

namespace RiscvISA
{

namespace
{

Request::Flags
localAccessFlags()
{
    Request::Flags flags;
    flags.set(Request::PHYSICAL | Request::NO_ACCESS |
              CLB::InternalCtableAccess | MMU::ByteAlign);
    return flags;
}

Request::Flags
ctableAccessFlags()
{
    Request::Flags flags;
    flags.set(Request::PHYSICAL | CLB::InternalCtableAccess |
              MMU::QuadWordAlign);
    return flags;
}

std::vector<bool>
allBytesEnabled(unsigned size)
{
    return std::vector<bool>(size, true);
}

Fault
resolveUserClb(ExecContext *xc, ExtMachInst machInst,
               const char *mnemonic, CLB *&clb)
{
    auto pm = static_cast<PrivilegeMode>(xc->readMiscReg(MISCREG_PRV));
    if (pm != PRV_U) {
        return std::make_shared<IllegalInstFault>(
            csprintf("%s outside U-mode", mnemonic), machInst);
    }

    auto *mmu = dynamic_cast<MMU *>(xc->tcBase()->getMMUPtr());
    panic_if(!mmu, "Invalid MMU for %s.", mnemonic);

    clb = mmu->getCLB();
    if (!clb) {
        return std::make_shared<IllegalInstFault>(
            csprintf("%s without CLB", mnemonic), machInst);
    }

    return NoFault;
}

} // namespace

ClbGetInst::ClbGetInst(ExtMachInst machInst)
    : RiscvStaticInst("uclb_get", machInst, MemReadOp)
{
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

    _numSrcRegs = 0;
    _numDestRegs = 0;
    setSrcRegIdx(_numSrcRegs++, intRegClass[machInst.rs1]);
    setDestRegIdx(_numDestRegs++, intRegClass[machInst.rd]);
    _numTypedDestRegs[IntRegClass]++;

    setFlag(IsLoad);
    setFlag(IsInteger);
    setFlag(IsSerializeAfter);
    setFlag(IsNonSpeculative);
}

Fault
ClbGetInst::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    CLB *clb = nullptr;
    Fault fault = resolveUserClb(xc, machInst, "uclb.get", clb);
    if (fault != NoFault) {
        return fault;
    }

    auto response = clb->getCfr(rvZext(xc->getRegOperand(this, 0)),
                                xc->tcBase());
    xc->setRegOperand(this, 0, response.value);
    xc->addTimingStall(response.stall);
    return NoFault;
}

Fault
ClbGetInst::initiateAcc(ExecContext *xc, trace::InstRecord *traceData) const
{
    CLB *clb = nullptr;
    Fault fault = resolveUserClb(xc, machInst, "uclb.get", clb);
    if (fault != NoFault) {
        return fault;
    }

    Addr addr = 0;
    const unsigned index = rvZext(xc->getRegOperand(this, 0));
    if (clb->prepareTimingGet(index, xc->tcBase(), addr)) {
        static const std::vector<bool> localEnable = allBytesEnabled(1);
        return xc->initiateMemRead(addr, 1, localAccessFlags(), localEnable);
    }

    const unsigned capBytes = clb->ctableCapBytes();
    return xc->initiateMemRead(addr, capBytes, ctableAccessFlags(),
                               allBytesEnabled(capBytes));
}

Fault
ClbGetInst::completeAcc(PacketPtr pkt, ExecContext *xc,
                        trace::InstRecord *traceData) const
{
    CLB *clb = nullptr;
    Fault fault = resolveUserClb(xc, machInst, "uclb.get", clb);
    if (fault != NoFault) {
        return fault;
    }

    RegVal result = 0;
    if (pkt && pkt->req->getFlags().isSet(Request::NO_ACCESS)) {
        result = clb->completeTimingGetLocal(xc->tcBase());
    } else if (pkt) {
        result = clb->completeTimingGetMiss(pkt->req->getPaddr(),
                                            pkt->getConstPtr<uint8_t>(),
                                            xc->tcBase());
    } else {
        result = static_cast<RegVal>(-1);
    }

    xc->setRegOperand(this, 0, result);
    return NoFault;
}

std::string
ClbGetInst::generateDisassembly(Addr pc,
                                const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", "
       << registerName(srcRegIdx(0));
    return ss.str();
}

ClbDeleteMacroInst::ClbDeleteMacroInst(ExtMachInst machInst)
    : RiscvMacroInst("uclb_delete", machInst, MemReadOp)
{
    StaticInstPtr probe = new ClbDeleteProbeMicroInst(machInst);
    StaticInstPtr commit = new ClbDeleteCommitMicroInst(machInst);

    probe->setFirstMicroop();
    commit->setLastMicroop();
    microops = {probe, commit};
}

std::string
ClbDeleteMacroInst::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(intRegClass[machInst.rd]) << ", "
       << registerName(intRegClass[machInst.rs1]);
    return ss.str();
}

ClbDeleteProbeMicroInst::ClbDeleteProbeMicroInst(ExtMachInst machInst)
    : RiscvMicroInst("uclb_delete_probe", machInst, MemReadOp)
{
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

    _numSrcRegs = 0;
    _numDestRegs = 0;
    setSrcRegIdx(_numSrcRegs++, intRegClass[machInst.rs1]);
    setDestRegIdx(_numDestRegs++, intRegClass[machInst.rd]);
    _numTypedDestRegs[IntRegClass]++;

    setFlag(IsLoad);
    setFlag(IsInteger);
    setFlag(IsSerializeAfter);
    setFlag(IsNonSpeculative);
}

Fault
ClbDeleteProbeMicroInst::execute(ExecContext *xc,
                                 trace::InstRecord *traceData) const
{
    CLB *clb = nullptr;
    Fault fault = resolveUserClb(xc, machInst, "uclb.delete", clb);
    if (fault != NoFault) {
        return fault;
    }

    xc->setRegOperand(this, 0,
                      clb->executeDeleteProbeFunctional(
                          rvZext(xc->getRegOperand(this, 0)), xc->tcBase()));
    return NoFault;
}

Fault
ClbDeleteProbeMicroInst::initiateAcc(ExecContext *xc,
                                     trace::InstRecord *traceData) const
{
    CLB *clb = nullptr;
    Fault fault = resolveUserClb(xc, machInst, "uclb.delete", clb);
    if (fault != NoFault) {
        return fault;
    }

    Addr addr = 0;
    const unsigned index = rvZext(xc->getRegOperand(this, 0));
    if (clb->prepareTimingDeleteProbe(index, xc->tcBase(), addr)) {
        static const std::vector<bool> localEnable = allBytesEnabled(1);
        return xc->initiateMemRead(addr, 1, localAccessFlags(), localEnable);
    }

    const unsigned capBytes = clb->ctableCapBytes();
    return xc->initiateMemRead(addr, capBytes, ctableAccessFlags(),
                               allBytesEnabled(capBytes));
}

Fault
ClbDeleteProbeMicroInst::completeAcc(PacketPtr pkt, ExecContext *xc,
                                     trace::InstRecord *traceData) const
{
    CLB *clb = nullptr;
    Fault fault = resolveUserClb(xc, machInst, "uclb.delete", clb);
    if (fault != NoFault) {
        return fault;
    }

    RegVal result = 0;
    if (pkt && pkt->req->getFlags().isSet(Request::NO_ACCESS)) {
        result = clb->completeTimingDeleteProbeLocal(xc->tcBase());
    } else if (pkt) {
        result = clb->completeTimingDeleteProbeMiss(
            pkt->req->getPaddr(), pkt->getConstPtr<uint8_t>(),
            xc->tcBase());
    } else {
        result = static_cast<RegVal>(-1);
    }

    xc->setRegOperand(this, 0, result);
    return NoFault;
}

std::string
ClbDeleteProbeMicroInst::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", "
       << registerName(srcRegIdx(0));
    return ss.str();
}

ClbDeleteCommitMicroInst::ClbDeleteCommitMicroInst(ExtMachInst machInst)
    : RiscvMicroInst("uclb_delete_commit", machInst, MemWriteOp)
{
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

    _numSrcRegs = 0;
    _numDestRegs = 0;
    setSrcRegIdx(_numSrcRegs++, intRegClass[machInst.rd]);
    setSrcRegIdx(_numSrcRegs++, intRegClass[machInst.rs1]);
    setDestRegIdx(_numDestRegs++, intRegClass[machInst.rd]);
    _numTypedDestRegs[IntRegClass]++;

    setFlag(IsStore);
    setFlag(IsInteger);
    setFlag(IsSerializeAfter);
    setFlag(IsNonSpeculative);
}

Fault
ClbDeleteCommitMicroInst::execute(ExecContext *xc,
                                  trace::InstRecord *traceData) const
{
    CLB *clb = nullptr;
    Fault fault = resolveUserClb(xc, machInst, "uclb.delete", clb);
    if (fault != NoFault) {
        return fault;
    }

    const RegVal probeResult = xc->getRegOperand(this, 0);
    const RegVal result = (probeResult == 0) ?
        clb->executeDeleteCommitFunctional(xc->tcBase()) :
        static_cast<RegVal>(-1);
    xc->setRegOperand(this, 0, result);
    return NoFault;
}

Fault
ClbDeleteCommitMicroInst::initiateAcc(ExecContext *xc,
                                      trace::InstRecord *traceData) const
{
    CLB *clb = nullptr;
    Fault fault = resolveUserClb(xc, machInst, "uclb.delete", clb);
    if (fault != NoFault) {
        return fault;
    }

    const RegVal probeResult = xc->getRegOperand(this, 0);
    if (probeResult != 0) {
        static const std::vector<bool> localEnable = allBytesEnabled(1);
        uint8_t dummy = 0;
        return xc->writeMem(&dummy, 1, clb->localProxyAddr(xc->tcBase()),
                            localAccessFlags(), nullptr, localEnable);
    }

    Addr addr = 0;
    std::array<uint8_t, 16> updated = {};
    if (!clb->pendingDeleteWriteback(xc->tcBase(), addr,
                                     updated.data(), updated.size())) {
        static const std::vector<bool> localEnable = allBytesEnabled(1);
        uint8_t dummy = 0;
        return xc->writeMem(&dummy, 1, clb->localProxyAddr(xc->tcBase()),
                            localAccessFlags(), nullptr, localEnable);
    }

    const unsigned capBytes = clb->ctableCapBytes();
    panic_if(capBytes != updated.size(),
             "Unexpected CLB ctable entry size %u.", capBytes);
    return xc->writeMem(updated.data(), capBytes, addr, ctableAccessFlags(),
                        nullptr, allBytesEnabled(capBytes));
}

Fault
ClbDeleteCommitMicroInst::completeAcc(PacketPtr pkt, ExecContext *xc,
                                      trace::InstRecord *traceData) const
{
    CLB *clb = nullptr;
    Fault fault = resolveUserClb(xc, machInst, "uclb.delete", clb);
    if (fault != NoFault) {
        return fault;
    }

    RegVal result = static_cast<RegVal>(-1);
    if (pkt && !pkt->req->getFlags().isSet(Request::NO_ACCESS)) {
        result = clb->completeTimingDeleteCommit(xc->tcBase(), false);
    }

    xc->setRegOperand(this, 0, result);
    return NoFault;
}

std::string
ClbDeleteCommitMicroInst::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", "
       << registerName(srcRegIdx(1));
    return ss.str();
}

} // namespace RiscvISA
} // namespace gem5
