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

#ifndef __ARCH_RISCV_CLB_HH__
#define __ARCH_RISCV_CLB_HH__

#include <map>
#include <vector>

#include "arch/generic/mmu.hh"
#include "arch/riscv/isa.hh"
#include "base/addr_range.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "mem/request.hh"
#include "params/CLB.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class ThreadContext;

namespace RiscvISA
{

/**
 * Capability Lookaside Buffer (CLB) model used by the S3K prototype.
 *
 * This SimObject plays three roles at once:
 * 1. Cache-like capability lookup for ordinary memory access control.
 * 2. Backend implementation of the CLB custom instructions and CSRs.
 * 3. Home of the current instruction-level timing model for those custom ops
 *    that need CLB/backend-dependent latency beyond the core pipeline's
 *    built-in execution latency.
 *
 * The current model is intentionally lightweight:
 * - CLB line state is modeled explicitly.
 * - Capability-table reads/writes still use functional physProxy accesses.
 * - Variable-latency custom ops can return an instruction-local stall.
 * - MinorCPU can also consume CLB lookup latency before a data access enters
 *   its LSQ.
 */
class CLB : public SimObject
{
  private:
    struct CtableMemCap;

  public:
    static constexpr Request::FlagsType InternalCtableAccess =
        Request::FlagsType(0x8);

    /** Result returned by CLB access-control lookups. */
    struct AccessCheckResult
    {
        Fault fault = NoFault;
        Cycles latency = Cycles(0);
    };

    /** Result returned by user-visible CLB operations. */
    struct OpResponse
    {
        RegVal value = 0;
        Cycles stall = Cycles(0);
    };

    /** Result returned by MinorCPU issue-time CLB path selection. */
    struct MinorLookupResult
    {
        bool localHit = false;
        Cycles latency = Cycles(0);
    };

    PARAMS(CLB);
    CLB(const Params &params);

    /** Access-control check injected from the RISC-V TLB path. */
    Fault clbCheck(const RequestPtr &req, BaseMMU::Mode mode,
                   PrivilegeMode pmode, ThreadContext *tc,
                   Addr vaddr = 0) const;
    AccessCheckResult lookupAccess(Addr paddr, unsigned size,
                                   BaseMMU::Mode mode,
                                   PrivilegeMode pmode,
                                   ThreadContext *tc,
                                   Addr vaddr = 0) const;

    /** Reset all CLB-visible state to its power-on values. */
    void reset();

    // Runtime control and machine-mode setup path.
    bool enabled() const { return _enabled; }
    void setEnable(bool enable);
    void setCtableBase(Addr base);
    void setSetupBase(Addr base);
    void setSetupLimit(Addr limit);
    void setSetupMeta(RegVal meta);
    bool fillLine(unsigned index, RegVal pid);
    RegVal readLine(unsigned index, ThreadContext *tc) const;
    bool invalidateLine(unsigned index);

    // User-visible custom instruction backend.
    OpResponse getCfr(unsigned index, ThreadContext *tc,
                      InstSeqNum seq_num = 0);
    OpResponse revokeCap(unsigned index, ThreadContext *tc);
    OpResponse deleteCap(unsigned index, ThreadContext *tc,
                         InstSeqNum seq_num = 0);
    MinorLookupResult lookupMinorGet(unsigned index, ThreadContext *tc) const;
    MinorLookupResult lookupMinorDelete(unsigned index,
                                        ThreadContext *tc) const;
    OpResponse prepareMinorGet(unsigned index, ThreadContext *tc,
                               InstSeqNum seq_num);
    OpResponse prepareMinorDelete(unsigned index, ThreadContext *tc,
                                  InstSeqNum seq_num);
    void discardPreparedMinorOp(ThreadID tid, InstSeqNum seq_num);
    void clearCfr(ThreadContext *tc) const;
    void flushAll();
    bool prepareTimingGet(unsigned index, ThreadContext *tc, Addr &addr);
    RegVal completeTimingGetLocal(ThreadContext *tc);
    RegVal completeTimingGetMiss(Addr capAddr, const uint8_t *raw,
                                 ThreadContext *tc);
    RegVal executeDeleteProbeFunctional(unsigned index, ThreadContext *tc);
    RegVal executeDeleteCommitFunctional(ThreadContext *tc);
    bool prepareTimingDeleteProbe(unsigned index, ThreadContext *tc,
                                  Addr &addr);
    RegVal completeTimingDeleteProbeLocal(ThreadContext *tc) const;
    RegVal completeTimingDeleteProbeMiss(Addr capAddr, const uint8_t *raw,
                                         ThreadContext *tc);
    bool pendingDeleteWriteback(ThreadContext *tc, Addr &addr, uint8_t *raw,
                                unsigned size) const;
    RegVal completeTimingDeleteCommit(ThreadContext *tc, bool writeCtable);
    static constexpr unsigned CtableCapBytes = 16;
    unsigned ctableCapBytes() const { return CtableCapBytes; }
    Addr localProxyAddr(ThreadContext *tc, unsigned index = 0) const;
    Addr ctableCapAddr(unsigned index) const;
    unsigned ctableCapIndex(Addr capAddr) const;

    static bool isInternalAccess(const RequestPtr &req);
    static bool isInternalAccess(const Request::Flags &flags);

  private:
    /** Layout of one capability-table entry as observed by the CLB model. */
    struct alignas(sizeof(RegVal)) CtableMemCap
    {
        uint16_t owner = 0;
        uint16_t cfree = 0;
        uint16_t csize = 0;
        uint8_t slot = 0;
        uint8_t rwx = 0;
        uint32_t base = 0;
        uint32_t size = 0;
    };

    static_assert(sizeof(CtableMemCap) == CtableCapBytes);

    /** Cache line stored in the CLB array. */
    struct ClbEntry
    {
        bool valid = false;
        AddrRange range = AddrRange(0, 0);
        uint8_t perms = 0;
        uint16_t pid = 0;
        uint32_t capIndex = 0;
        uint16_t capSize = 0;
        uint16_t capFree = 0;
    };

    enum : uint8_t
    {
        PermRead = 1 << 0,
        PermWrite = 1 << 1,
        PermExec = 1 << 2,
    };

    static constexpr unsigned MaxCapEntries = 2048;

    struct PreparedMinorKey
    {
        ThreadID tid = 0;
        InstSeqNum seqNum = 0;

        bool
        operator<(const PreparedMinorKey &other) const
        {
            return tid < other.tid ||
                   (tid == other.tid && seqNum < other.seqNum);
        }
    };

    struct PreparedMinorOp
    {
        enum class Kind
        {
            Get,
            Delete,
        };

        Kind kind = Kind::Get;
        unsigned index = 0;
        RegVal value = 0;
        Cycles stall = Cycles(0);
        bool writeCfr = false;
        Addr cfrBase = 0;
        Addr cfrLimit = 0;
        RegVal cfrMeta = 0;
        bool writeCtable = false;
        Addr ctableAddr = 0;
        CtableMemCap ctableCap = {};
        bool invalidateCap = false;
        unsigned invalidateIndex = 0;
    };

    struct PendingGetState
    {
        Addr base = 0;
        Addr limit = 0;
        RegVal meta = 0;
    };

    struct PendingDeleteState
    {
        Addr ctableAddr = 0;
        Addr base = 0;
        Addr limit = 0;
        RegVal meta = 0;
        unsigned invalidateIndex = 0;
        CtableMemCap ctableCap = {};
    };

    // Functional capability-table access helpers.
    bool readCtableCapFunctional(unsigned index, ThreadContext *tc,
                                 Addr &capAddr, CtableMemCap &cap) const;
    void writeCtableCapFunctional(Addr capAddr, ThreadContext *tc,
                                  const CtableMemCap &cap) const;

    // Custom instruction implementations.
    OpResponse startGetFunctional(unsigned index, ThreadContext *tc) const;
    OpResponse startDeleteFunctional(unsigned index, ThreadContext *tc);
    OpResponse startRevokeFunctional(unsigned index, ThreadContext *tc);
    PreparedMinorOp buildGetOp(unsigned index, ThreadContext *tc) const;
    PreparedMinorOp buildDeleteOp(unsigned index, ThreadContext *tc) const;
    MinorLookupResult minorLookup(unsigned index, ThreadContext *tc) const;
    void commitPreparedOp(const PreparedMinorOp &op, ThreadContext *tc);
    OpResponse consumePreparedMinorOp(unsigned index, ThreadContext *tc,
                                      InstSeqNum seq_num,
                                      PreparedMinorOp::Kind kind);

    // CLB line ownership / metadata helpers.
    bool currentPidMatches(const ClbEntry &entry, ThreadContext *tc) const;
    bool findOwnedCapInClb(unsigned index, ThreadContext *tc) const;
    const ClbEntry *findOwnedCapLine(unsigned index,
                                     ThreadContext *tc) const;
    uint8_t currentPid(ThreadContext *tc) const;
    RegVal packMeta(uint32_t capIndex, uint16_t capFree,
                    uint16_t capSize, uint8_t perms) const;
    Cycles timingSimpleStall(Cycles latency) const;
    void commitCfr(ThreadContext *tc, Addr base, Addr limit,
                   RegVal meta) const;
    CtableMemCap decodeCtableCap(const uint8_t *raw) const;
    ThreadID threadId(ThreadContext *tc) const;
    void stagePendingGet(ThreadContext *tc, Addr base, Addr limit,
                         RegVal meta);
    bool takePendingGet(ThreadContext *tc, PendingGetState &state);
    void clearPendingGet(ThreadContext *tc);
    void stagePendingDelete(ThreadContext *tc, Addr ctableAddr,
                            Addr base, Addr limit, RegVal meta,
                            unsigned invalidateIndex,
                            const CtableMemCap &ctableCap);
    bool peekPendingDelete(ThreadContext *tc,
                           PendingDeleteState &state) const;
    bool takePendingDelete(ThreadContext *tc, PendingDeleteState &state);
    void clearPendingDelete(ThreadContext *tc);
    unsigned updateCachedCapMeta(unsigned index, uint8_t pid,
                                 uint16_t capFree);
    unsigned invalidateCapEntries(unsigned index, uint8_t pid);
    unsigned invalidateCapEntries(unsigned index);

    // Persistent CLB state.
    bool _enabled;
    const bool _resetEnable;
    const Cycles _lookupLatency;
    const Cycles _getHitLatency;
    const Cycles _getCtableLatency;
    const Cycles _deleteHitLatency;
    const Cycles _deleteMissLatency;
    const Cycles _revokeLookupLatency;
    const Cycles _revokeParentCtableLatency;
    const Cycles _revokePerChildLatency;
    Addr _ctableBase;
    Addr _setupBase;
    Addr _setupLimit;
    RegVal _setupMeta;
    std::vector<ClbEntry> entries;
    std::map<PreparedMinorKey, PreparedMinorOp> preparedMinorOps;
    std::map<ThreadID, PendingGetState> pendingGets;
    std::map<ThreadID, PendingDeleteState> pendingDeletes;

    // Fault construction and permission checking.
    Fault createAccessFault(Addr vaddr, BaseMMU::Mode mode) const;
    Fault createMissFault(Addr vaddr) const;
    bool permsAllow(uint8_t perms, BaseMMU::Mode mode) const;
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_CLB_HH__
