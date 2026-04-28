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

#include "arch/riscv/clb.hh"

#include <cstring>

#include "arch/riscv/faults.hh"
#include "arch/riscv/regs/misc.hh"
#include "base/bitfield.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "debug/CLB.hh"
#include "params/CLB.hh"
#include "sim/byteswap.hh"
#include "sim/system.hh"

namespace gem5
{

namespace RiscvISA
{

namespace
{

Cycles
selectLookupLatency(const CLBParams &params)
{
    return params.clb_lookup_latency != Cycles(1) ?
        params.clb_lookup_latency : params.access_latency;
}

} // anonymous namespace

// Construction and reset ----------------------------------------------------

CLB::CLB(const Params &params)
    : SimObject(params), _enabled(params.enable), _resetEnable(params.enable),
      _lookupLatency(selectLookupLatency(params)),
      _getHitLatency(params.get_hit_latency),
      _getCtableLatency(params.get_ctable_latency),
      _deleteHitLatency(params.delete_hit_latency),
      _deleteMissLatency(params.delete_miss_latency),
      _revokeLookupLatency(params.revoke_lookup_latency),
      _revokeParentCtableLatency(params.revoke_parent_ctable_latency),
      _revokePerChildLatency(params.revoke_per_child_latency),
      _ctableBase(0), _setupBase(0), _setupLimit(0), _setupMeta(0),
      entries(params.clb_entries)
{
}

void
CLB::reset()
{
    _enabled = _resetEnable;
    _ctableBase = 0;
    _setupBase = 0;
    _setupLimit = 0;
    _setupMeta = 0;
    preparedMinorOps.clear();
    pendingGets.clear();
    pendingDeletes.clear();

    for (auto &entry : entries) {
        entry = ClbEntry();
    }
}

// Machine-mode control / setup path -----------------------------------------

void
CLB::setEnable(bool enable)
{
    _enabled = enable;
    DPRINTF(CLB, "CLB runtime enable set to %d\n", _enabled);
}

void
CLB::setCtableBase(Addr base)
{
    _ctableBase = base;
    DPRINTF(CLB, "CLB ctable base set to %#x\n", _ctableBase);
}

void
CLB::setSetupBase(Addr base)
{
    _setupBase = base;
    DPRINTF(CLB, "CLB setup base set to %#x\n", _setupBase);
}

void
CLB::setSetupLimit(Addr limit)
{
    _setupLimit = limit;
    DPRINTF(CLB, "CLB setup limit set to %#x\n", _setupLimit);
}

void
CLB::setSetupMeta(RegVal meta)
{
    _setupMeta = meta;
    DPRINTF(CLB,
            "CLB setup meta set to %#x (cap_index=%u, cap_free=%u, "
            "cap_size=%u, perms=%u)\n",
            _setupMeta, bits(_setupMeta, 31, 21), bits(_setupMeta, 20, 12),
            bits(_setupMeta, 11, 3), bits(_setupMeta, 2, 0));
}

bool
CLB::fillLine(unsigned index, RegVal pid)
{
    if (index >= entries.size()) {
        warn("CLB fill ignored for invalid line index %u.\n", index);
        return false;
    }

    if (_setupLimit <= _setupBase) {
        warn("CLB fill ignored for invalid range [%#x, %#x).\n",
             _setupBase, _setupLimit);
        return false;
    }

    auto &entry = entries[index];
    entry = ClbEntry();
    entry.valid = true;
    entry.range = AddrRange(_setupBase, _setupLimit);
    entry.perms = bits(_setupMeta, 2, 0);
    entry.capSize = bits(_setupMeta, 11, 3);
    entry.capFree = bits(_setupMeta, 20, 12);
    entry.capIndex = bits(_setupMeta, 31, 21);
    entry.pid = bits(pid, 7, 0);

    DPRINTF(CLB,
            "Filled CLB line %u: pid=%u, range=[%#x, %#x), perms=%u, "
            "cap_index=%u, cap_free=%u, cap_size=%u, ctable_base=%#x\n",
            index, entry.pid, entry.range.start(), entry.range.end(),
            entry.perms, entry.capIndex, entry.capFree, entry.capSize,
            _ctableBase);
    return true;
}

RegVal
CLB::readLine(unsigned index, ThreadContext *tc) const
{
    if (!tc) {
        warn("mclb.read ignored without a thread context.\n");
        return static_cast<RegVal>(-1);
    }

    if (index >= entries.size()) {
        DPRINTF(CLB,
                "mclb.read rejected out-of-range line index %u (entries=%u)\n",
                index, entries.size());
        return static_cast<RegVal>(-1);
    }

    const auto &entry = entries[index];
    if (!entry.valid) {
        DPRINTF(CLB, "mclb.read found CLB line %u invalid\n", index);
        return static_cast<RegVal>(-1);
    }

    commitCfr(tc, entry.range.start(), entry.range.end(),
              packMeta(entry.capIndex, entry.capFree,
                       entry.capSize, entry.perms));
    DPRINTF(CLB,
            "mclb.read committed CLB line %u to CFR: pid=%u, "
            "range=[%#x, %#x), cap_index=%u, cap_free=%u, cap_size=%u, "
            "perms=%u\n",
            index, entry.pid, entry.range.start(), entry.range.end(),
            entry.capIndex, entry.capFree, entry.capSize, entry.perms);
    return 0;
}

bool
CLB::invalidateLine(unsigned index)
{
    if (index >= entries.size()) {
        warn("CLB invalidate ignored for invalid line index %u.\n", index);
        return false;
    }

    const bool was_valid = entries[index].valid;
    entries[index] = ClbEntry();
    DPRINTF(CLB, "Invalidated CLB line %u (was_valid=%d)\n",
            index, was_valid);
    return true;
}

RegVal
CLB::packMeta(uint32_t capIndex, uint16_t capFree,
              uint16_t capSize, uint8_t perms) const
{
    RegVal meta = 0;
    replaceBits(meta, 31, 21, capIndex);
    replaceBits(meta, 20, 12, capFree);
    replaceBits(meta, 11, 3, capSize);
    replaceBits(meta, 2, 0, perms);
    return meta;
}

Cycles
CLB::timingSimpleStall(Cycles latency) const
{
    return latency > Cycles(0) ? latency - Cycles(1) : Cycles(0);
}

// CFR and capability-table helpers ------------------------------------------

void
CLB::commitCfr(ThreadContext *tc, Addr base, Addr limit, RegVal meta) const
{
    panic_if(!tc, "Cannot commit CLB CFR state without a thread context.");

    tc->setMiscRegNoEffect(MISCREG_UCLB_CFR_BASE, base);
    tc->setMiscRegNoEffect(MISCREG_UCLB_CFR_LIMIT, limit);
    tc->setMiscRegNoEffect(MISCREG_UCLB_CFR_META, meta);

    DPRINTF(CLB,
            "Committed CFR base=%#x limit=%#x meta=%#x "
            "(cap_index=%u, cap_free=%u, cap_size=%u, perms=%u)\n",
            base, limit, meta, bits(meta, 31, 21), bits(meta, 20, 12),
            bits(meta, 11, 3), bits(meta, 2, 0));
}

void
CLB::clearCfr(ThreadContext *tc) const
{
    panic_if(!tc, "Cannot clear CLB CFR state without a thread context.");

    tc->setMiscRegNoEffect(MISCREG_UCLB_CFR_BASE, 0);
    tc->setMiscRegNoEffect(MISCREG_UCLB_CFR_LIMIT, 0);
    tc->setMiscRegNoEffect(MISCREG_UCLB_CFR_META, 0);

    DPRINTF(CLB, "Cleared CFR state\n");
}

CLB::CtableMemCap
CLB::decodeCtableCap(const uint8_t *raw) const
{
    CtableMemCap cap = {};
    uint16_t owner = 0;
    uint16_t cfree = 0;
    uint16_t csize = 0;
    uint32_t base = 0;
    uint32_t size = 0;

    std::memcpy(&owner, raw, sizeof(owner));
    std::memcpy(&cfree, raw + 2, sizeof(cfree));
    std::memcpy(&csize, raw + 4, sizeof(csize));
    std::memcpy(&cap.slot, raw + 6, sizeof(cap.slot));
    std::memcpy(&cap.rwx, raw + 7, sizeof(cap.rwx));
    std::memcpy(&base, raw + 8, sizeof(base));
    std::memcpy(&size, raw + 12, sizeof(size));

    cap.owner = letoh(owner);
    cap.cfree = letoh(cfree);
    cap.csize = letoh(csize);
    cap.base = letoh(base);
    cap.size = letoh(size);
    return cap;
}

ThreadID
CLB::threadId(ThreadContext *tc) const
{
    panic_if(!tc, "CLB thread-local state requires a thread context.");
    return static_cast<ThreadID>(tc->threadId());
}

void
CLB::stagePendingGet(ThreadContext *tc, Addr base, Addr limit, RegVal meta)
{
    pendingGets[threadId(tc)] = PendingGetState{base, limit, meta};
}

bool
CLB::takePendingGet(ThreadContext *tc, PendingGetState &state)
{
    auto it = pendingGets.find(threadId(tc));
    if (it == pendingGets.end()) {
        return false;
    }

    state = it->second;
    pendingGets.erase(it);
    return true;
}

void
CLB::clearPendingGet(ThreadContext *tc)
{
    if (tc) {
        pendingGets.erase(threadId(tc));
    }
}

void
CLB::stagePendingDelete(ThreadContext *tc, Addr ctableAddr,
                        Addr base, Addr limit, RegVal meta,
                        unsigned invalidateIndex,
                        const CtableMemCap &ctableCap)
{
    pendingDeletes[threadId(tc)] = PendingDeleteState{
        ctableAddr, base, limit, meta, invalidateIndex, ctableCap};
}

bool
CLB::peekPendingDelete(ThreadContext *tc, PendingDeleteState &state) const
{
    auto it = pendingDeletes.find(threadId(tc));
    if (it == pendingDeletes.end()) {
        return false;
    }

    state = it->second;
    return true;
}

bool
CLB::takePendingDelete(ThreadContext *tc, PendingDeleteState &state)
{
    auto it = pendingDeletes.find(threadId(tc));
    if (it == pendingDeletes.end()) {
        return false;
    }

    state = it->second;
    pendingDeletes.erase(it);
    return true;
}

void
CLB::clearPendingDelete(ThreadContext *tc)
{
    if (tc) {
        pendingDeletes.erase(threadId(tc));
    }
}

Addr
CLB::ctableCapAddr(unsigned index) const
{
    panic_if(_ctableBase == 0,
             "CLB ctable address requested before mctable_base setup.");
    return _ctableBase + (index * sizeof(CtableMemCap));
}

unsigned
CLB::ctableCapIndex(Addr capAddr) const
{
    panic_if(_ctableBase == 0,
             "CLB ctable index requested before mctable_base setup.");
    panic_if(capAddr < _ctableBase,
             "CLB ctable address %#x is below base %#x.",
             capAddr, _ctableBase);
    return (capAddr - _ctableBase) / sizeof(CtableMemCap);
}

Addr
CLB::localProxyAddr(ThreadContext *tc, unsigned index) const
{
    if (_ctableBase != 0 && index < MaxCapEntries) {
        return _ctableBase + (index * sizeof(CtableMemCap));
    }

    return tc ? tc->pcState().instAddr() : 0;
}

bool
CLB::isInternalAccess(const RequestPtr &req)
{
    return req && (req->getArchFlags() & InternalCtableAccess);
}

bool
CLB::isInternalAccess(const Request::Flags &flags)
{
    return flags.isSet(InternalCtableAccess);
}

bool
CLB::readCtableCapFunctional(unsigned index, ThreadContext *tc,
                             Addr &capAddr, CtableMemCap &cap) const
{
    if (_ctableBase == 0) {
        warn("CLB cannot read ctable entry %u before mctable_base is "
             "configured.\n", index);
        return false;
    }

    panic_if(!tc || !tc->getSystemPtr(),
             "CLB ctable access requires a thread context and system.");

    cap = {};
    capAddr = _ctableBase + (index * sizeof(CtableMemCap));
    tc->getSystemPtr()->physProxy.readBlob(capAddr, &cap, sizeof(cap));
    return true;
}

void
CLB::writeCtableCapFunctional(Addr capAddr, ThreadContext *tc,
                              const CtableMemCap &cap) const
{
    panic_if(!tc || !tc->getSystemPtr(),
             "CLB ctable write requires a thread context and system.");
    tc->getSystemPtr()->physProxy.writeBlob(capAddr, &cap, sizeof(cap));
}

// User-visible CLB custom instructions --------------------------------------

CLB::PreparedMinorOp
CLB::buildGetOp(unsigned index, ThreadContext *tc) const
{
    PreparedMinorOp op = {};
    op.kind = PreparedMinorOp::Kind::Get;
    op.index = index;

    if (!tc) {
        warn("uclb.get ignored without a thread context.\n");
        op.value = static_cast<RegVal>(-1);
        return op;
    }

    if (index >= MaxCapEntries) {
        DPRINTF(CLB,
                "uclb.get rejected out-of-range cap_index=%u "
                "(max entries=%u)\n",
                index, MaxCapEntries);
        op.value = static_cast<RegVal>(-1);
        return op;
    }

    const uint8_t pid = currentPid(tc);
    bool clbLineSeen = false;

    for (const auto &entry : entries) {
        if (!entry.valid || entry.capIndex != index) {
            continue;
        }

        clbLineSeen = true;

        if (!currentPidMatches(entry, tc)) {
            DPRINTF(CLB,
                    "uclb.get skipped CLB line for cap_index=%u due to "
                    "PID mismatch: entry=%u current=%u\n",
                    index, entry.pid, pid);
            continue;
        }

        op.writeCfr = true;
        op.cfrBase = entry.range.start();
        op.cfrLimit = entry.range.end();
        op.cfrMeta = packMeta(entry.capIndex, entry.capFree,
                              entry.capSize, entry.perms);
        DPRINTF(CLB,
                "uclb.get satisfied from CLB line for cap_index=%u pid=%u\n",
                index, pid);
        op.value = 0;
        op.stall = timingSimpleStall(_getHitLatency);
        return op;
    }

    DPRINTF(CLB, "uclb.get %s for cap_index=%u pid=%u\n",
            clbLineSeen ? "fell through due to owner mismatch" :
                          "missed in CLB",
            index, pid);

    CtableMemCap cap = {};
    Addr capAddr = 0;
    if (!readCtableCapFunctional(index, tc, capAddr, cap)) {
        op.value = static_cast<RegVal>(-1);
        return op;
    }

    if (bits(cap.owner, 7, 0) != pid) {
        DPRINTF(CLB,
                "uclb.get owner mismatch for cap_index=%u from ctable: "
                "owner=%u current=%u\n",
                index, cap.owner, pid);
        op.value = static_cast<RegVal>(-1);
        op.stall = timingSimpleStall(_getCtableLatency);
        return op;
    }

    const Addr base = cap.base;
    const Addr limit = base + cap.size;
    op.writeCfr = true;
    op.cfrBase = base;
    op.cfrLimit = limit;
    op.cfrMeta = packMeta(index, cap.cfree, cap.csize, cap.rwx);
    DPRINTF(CLB,
            "uclb.get loaded cap_index=%u from ctable entry %#x for pid=%u\n",
            index, capAddr, pid);
    op.value = 0;
    op.stall = timingSimpleStall(_getCtableLatency);
    return op;
}

CLB::OpResponse
CLB::startGetFunctional(unsigned index, ThreadContext *tc) const
{
    const auto op = buildGetOp(index, tc);
    auto *self = const_cast<CLB *>(this);
    self->commitPreparedOp(op, tc);
    return {.value = op.value, .stall = op.stall};
}

CLB::MinorLookupResult
CLB::minorLookup(unsigned index, ThreadContext *tc) const
{
    MinorLookupResult result = {};

    if (!tc || index >= MaxCapEntries) {
        return result;
    }

    result.latency = _lookupLatency;
    result.localHit = findOwnedCapLine(index, tc) != nullptr;
    return result;
}

CLB::MinorLookupResult
CLB::lookupMinorGet(unsigned index, ThreadContext *tc) const
{
    return minorLookup(index, tc);
}

CLB::MinorLookupResult
CLB::lookupMinorDelete(unsigned index, ThreadContext *tc) const
{
    return minorLookup(index, tc);
}

bool
CLB::prepareTimingGet(unsigned index, ThreadContext *tc, Addr &addr)
{
    addr = localProxyAddr(tc, index);
    clearPendingGet(tc);

    if (!tc || index >= MaxCapEntries) {
        return true;
    }

    if (const auto *entry = findOwnedCapLine(index, tc)) {
        stagePendingGet(tc, entry->range.start(), entry->range.end(),
                        packMeta(entry->capIndex, entry->capFree,
                                 entry->capSize, entry->perms));
        return true;
    }

    if (_ctableBase == 0) {
        return true;
    }

    addr = ctableCapAddr(index);
    return false;
}

RegVal
CLB::completeTimingGetLocal(ThreadContext *tc)
{
    PendingGetState state;
    if (!takePendingGet(tc, state)) {
        return static_cast<RegVal>(-1);
    }

    commitCfr(tc, state.base, state.limit, state.meta);
    return 0;
}

RegVal
CLB::completeTimingGetMiss(Addr capAddr, const uint8_t *raw,
                           ThreadContext *tc)
{
    if (!tc || _ctableBase == 0) {
        return static_cast<RegVal>(-1);
    }

    const unsigned index = ctableCapIndex(capAddr);
    if (index >= MaxCapEntries) {
        return static_cast<RegVal>(-1);
    }

    const CtableMemCap cap = decodeCtableCap(raw);
    if (bits(cap.owner, 7, 0) != currentPid(tc)) {
        return static_cast<RegVal>(-1);
    }

    commitCfr(tc, cap.base, cap.base + cap.size,
              packMeta(index, cap.cfree, cap.csize, cap.rwx));
    return 0;
}

// CLB metadata maintenance helpers ------------------------------------------

unsigned
CLB::updateCachedCapMeta(unsigned index, uint8_t pid, uint16_t capFree)
{
    unsigned updated = 0;

    for (auto &entry : entries) {
        if (!entry.valid || entry.capIndex != index || entry.pid != pid) {
            continue;
        }

        entry.capFree = capFree;
        ++updated;
    }

    return updated;
}

unsigned
CLB::invalidateCapEntries(unsigned index, uint8_t pid)
{
    unsigned invalidated = 0;

    for (auto &entry : entries) {
        if (!entry.valid || entry.capIndex != index || entry.pid != pid) {
            continue;
        }

        entry = ClbEntry();
        ++invalidated;
    }

    return invalidated;
}

unsigned
CLB::invalidateCapEntries(unsigned index)
{
    unsigned invalidated = 0;

    for (auto &entry : entries) {
        if (!entry.valid || entry.capIndex != index) {
            continue;
        }

        entry = ClbEntry();
        ++invalidated;
    }

    return invalidated;
}

// uclb.revoke ---------------------------------------------------------------

CLB::OpResponse
CLB::startRevokeFunctional(unsigned index, ThreadContext *tc)
{
    if (!tc) {
        warn("uclb.revoke ignored without a thread context.\n");
        return {.value = static_cast<RegVal>(-1)};
    }

    if (index >= MaxCapEntries) {
        DPRINTF(CLB,
                "uclb.revoke rejected out-of-range cap_index=%u "
                "(max entries=%u)\n",
                index, MaxCapEntries);
        return {.value = static_cast<RegVal>(-1)};
    }

    const uint8_t pid = currentPid(tc);
    const bool parentClbHit = findOwnedCapInClb(index, tc);
    Cycles totalLatency = _revokeLookupLatency;
    unsigned childrenProcessed = 0;

    if (!parentClbHit) {
        totalLatency += _revokeParentCtableLatency;
    }

    CtableMemCap parent = {};
    Addr parentAddr = 0;
    if (!readCtableCapFunctional(index, tc, parentAddr, parent)) {
        return {.value = static_cast<RegVal>(-1),
                .stall = timingSimpleStall(totalLatency)};
    }

    if (bits(parent.owner, 7, 0) != pid) {
        DPRINTF(CLB,
                "uclb.revoke owner mismatch for cap_index=%u from ctable: "
                "owner=%u current=%u\n",
                index, parent.owner, pid);
        return {.value = static_cast<RegVal>(-1),
                .stall = timingSimpleStall(totalLatency)};
    }

    while (parent.cfree < parent.csize) {
        const unsigned childIndex = index + parent.cfree;

        if (childIndex >= MaxCapEntries) {
            warn("uclb.revoke stopped on invalid descendant index %u "
                 "for cap_index=%u.\n", childIndex, index);
            break;
        }

        CtableMemCap child = {};
        Addr childAddr = 0;
        if (!readCtableCapFunctional(childIndex, tc, childAddr, child)) {
            return {.value = static_cast<RegVal>(-1)};
        }

        if (child.cfree == 0) {
            warn("uclb.revoke encountered a zero-sized descendant at "
                 "cap_index=%u under parent cap_index=%u.\n",
                 childIndex, index);
            break;
        }

        parent.cfree += child.cfree;
        writeCtableCapFunctional(parentAddr, tc, parent);

        child = {};
        writeCtableCapFunctional(childAddr, tc, child);

        const unsigned invalidated = invalidateCapEntries(childIndex);
        const unsigned updated_parent = updateCachedCapMeta(index, pid,
                                                            parent.cfree);
        ++childrenProcessed;
        totalLatency += _revokePerChildLatency;

        DPRINTF(CLB,
                "uclb.revoke cleared descendant cap_index=%u under parent "
                "cap_index=%u for pid=%u, parent.cfree=%u/%u, "
                "invalidated=%u, updated_parent_lines=%u\n",
                childIndex, index, pid, parent.cfree, parent.csize,
                invalidated, updated_parent);

        if (tc->getCpuPtr()->checkInterrupts(tc->threadId())) {
            break;
        }
    }

    const RegVal remaining = parent.csize - parent.cfree;
    DPRINTF(CLB,
            "uclb.revoke finished chunk for cap_index=%u pid=%u "
            "remaining=%u (parent %s, processed=%u, total latency=%llu)\n",
            index, pid, remaining, parentClbHit ? "hit" : "miss",
            childrenProcessed, totalLatency);
    return {.value = remaining, .stall = timingSimpleStall(totalLatency)};
}

// uclb.delete ---------------------------------------------------------------

CLB::PreparedMinorOp
CLB::buildDeleteOp(unsigned index, ThreadContext *tc) const
{
    PreparedMinorOp op = {};
    op.kind = PreparedMinorOp::Kind::Delete;
    op.index = index;

    if (!tc) {
        warn("uclb.delete ignored without a thread context.\n");
        op.value = static_cast<RegVal>(-1);
        return op;
    }

    if (index >= MaxCapEntries) {
        DPRINTF(CLB,
                "uclb.delete rejected out-of-range cap_index=%u "
                "(max entries=%u)\n",
                index, MaxCapEntries);
        op.value = static_cast<RegVal>(-1);
        return op;
    }

    const uint8_t pid = currentPid(tc);
    for (const auto &entry : entries) {
        if (!entry.valid || entry.capIndex != index) {
            continue;
        }

        if (!currentPidMatches(entry, tc)) {
            continue;
        }

        const Addr capAddr = _ctableBase + (index * sizeof(CtableMemCap));
        DPRINTF(CLB,
                "uclb.delete hit in CLB for cap_index=%u pid=%u, committed "
                "deleted capability to CFR, invalidated cached entries, "
                "and invalidated the ctable owner at %#x\n",
                index, pid, capAddr);
        CtableMemCap cap = {};
        cap.owner = 0;
        cap.cfree = entry.capFree;
        cap.csize = entry.capSize;
        cap.slot = 0;
        cap.rwx = entry.perms;
        cap.base = entry.range.start();
        cap.size = entry.range.end() - entry.range.start();
        op.writeCfr = true;
        op.cfrBase = entry.range.start();
        op.cfrLimit = entry.range.end();
        op.cfrMeta = packMeta(entry.capIndex, entry.capFree,
                              entry.capSize, entry.perms);
        op.writeCtable = true;
        op.ctableAddr = capAddr;
        op.ctableCap = cap;
        op.invalidateCap = true;
        op.invalidateIndex = index;
        op.value = 0;
        op.stall = timingSimpleStall(_deleteHitLatency);
        return op;
    }

    CtableMemCap cap = {};
    Addr capAddr = 0;
    if (!readCtableCapFunctional(index, tc, capAddr, cap)) {
        op.value = static_cast<RegVal>(-1);
        return op;
    }

    if (bits(cap.owner, 7, 0) != pid) {
        DPRINTF(CLB,
                "uclb.delete owner mismatch for cap_index=%u from ctable: "
                "owner=%u current=%u\n",
                index, cap.owner, pid);
        op.value = static_cast<RegVal>(-1);
        op.stall = timingSimpleStall(_deleteMissLatency);
        return op;
    }

    DPRINTF(CLB,
            "uclb.delete missed in CLB for cap_index=%u pid=%u, committed "
            "deleted capability to CFR from ctable, and invalidated cached "
            "entries at ctable entry %#x\n",
            index, pid, capAddr);
    cap.owner = 0;
    cap.slot = 0;
    op.writeCfr = true;
    op.cfrBase = cap.base;
    op.cfrLimit = cap.base + cap.size;
    op.cfrMeta = packMeta(index, cap.cfree, cap.csize, cap.rwx);
    op.writeCtable = true;
    op.ctableAddr = capAddr;
    op.ctableCap = cap;
    op.invalidateCap = true;
    op.invalidateIndex = index;
    op.value = 0;
    op.stall = timingSimpleStall(_deleteMissLatency);
    return op;
}

CLB::OpResponse
CLB::startDeleteFunctional(unsigned index, ThreadContext *tc)
{
    const auto op = buildDeleteOp(index, tc);
    commitPreparedOp(op, tc);
    return {.value = op.value, .stall = op.stall};
}

RegVal
CLB::executeDeleteProbeFunctional(unsigned index, ThreadContext *tc)
{
    clearPendingDelete(tc);

    if (!tc || index >= MaxCapEntries || _ctableBase == 0) {
        return static_cast<RegVal>(-1);
    }

    if (const auto *entry = findOwnedCapLine(index, tc)) {
        CtableMemCap cap = {};
        cap.owner = 0;
        cap.cfree = entry->capFree;
        cap.csize = entry->capSize;
        cap.slot = 0;
        cap.rwx = entry->perms;
        cap.base = entry->range.start();
        cap.size = entry->range.end() - entry->range.start();
        stagePendingDelete(tc, ctableCapAddr(index), entry->range.start(),
                           entry->range.end(),
                           packMeta(entry->capIndex, entry->capFree,
                                    entry->capSize, entry->perms),
                           index, cap);
        return 0;
    }

    CtableMemCap cap = {};
    Addr capAddr = 0;
    if (!readCtableCapFunctional(index, tc, capAddr, cap)) {
        return static_cast<RegVal>(-1);
    }

    if (bits(cap.owner, 7, 0) != currentPid(tc)) {
        return static_cast<RegVal>(-1);
    }

    cap.owner = 0;
    cap.slot = 0;
    stagePendingDelete(tc, capAddr, cap.base, cap.base + cap.size,
                       packMeta(index, cap.cfree, cap.csize, cap.rwx),
                       index, cap);
    return 0;
}

RegVal
CLB::executeDeleteCommitFunctional(ThreadContext *tc)
{
    return completeTimingDeleteCommit(tc, true);
}

bool
CLB::prepareTimingDeleteProbe(unsigned index, ThreadContext *tc, Addr &addr)
{
    addr = localProxyAddr(tc, index);
    clearPendingDelete(tc);

    if (!tc || index >= MaxCapEntries || _ctableBase == 0) {
        return true;
    }

    if (const auto *entry = findOwnedCapLine(index, tc)) {
        CtableMemCap cap = {};
        cap.owner = 0;
        cap.cfree = entry->capFree;
        cap.csize = entry->capSize;
        cap.slot = 0;
        cap.rwx = entry->perms;
        cap.base = entry->range.start();
        cap.size = entry->range.end() - entry->range.start();
        stagePendingDelete(tc, ctableCapAddr(index), entry->range.start(),
                           entry->range.end(),
                           packMeta(entry->capIndex, entry->capFree,
                                    entry->capSize, entry->perms),
                           index, cap);
        return true;
    }

    addr = ctableCapAddr(index);
    return false;
}

RegVal
CLB::completeTimingDeleteProbeLocal(ThreadContext *tc) const
{
    PendingDeleteState state;
    return peekPendingDelete(tc, state) ? 0 : static_cast<RegVal>(-1);
}

RegVal
CLB::completeTimingDeleteProbeMiss(Addr capAddr, const uint8_t *raw,
                                   ThreadContext *tc)
{
    if (!tc || _ctableBase == 0) {
        return static_cast<RegVal>(-1);
    }

    const unsigned index = ctableCapIndex(capAddr);
    if (index >= MaxCapEntries) {
        return static_cast<RegVal>(-1);
    }

    const CtableMemCap cap = decodeCtableCap(raw);
    if (bits(cap.owner, 7, 0) != currentPid(tc)) {
        return static_cast<RegVal>(-1);
    }

    CtableMemCap deleted = cap;
    deleted.owner = 0;
    deleted.slot = 0;
    stagePendingDelete(tc, capAddr, cap.base, cap.base + cap.size,
                       packMeta(index, cap.cfree, cap.csize, cap.rwx),
                       index, deleted);
    return 0;
}

bool
CLB::pendingDeleteWriteback(ThreadContext *tc, Addr &addr, uint8_t *raw,
                            unsigned size) const
{
    PendingDeleteState state;
    if (!peekPendingDelete(tc, state)) {
        return false;
    }

    panic_if(size < sizeof(state.ctableCap),
             "Pending delete writeback buffer too small: %u < %zu",
             size, sizeof(state.ctableCap));
    addr = state.ctableAddr;
    std::memcpy(raw, &state.ctableCap, sizeof(state.ctableCap));
    return true;
}

RegVal
CLB::completeTimingDeleteCommit(ThreadContext *tc, bool writeCtable)
{
    PendingDeleteState state;
    if (!takePendingDelete(tc, state)) {
        return static_cast<RegVal>(-1);
    }

    if (writeCtable) {
        writeCtableCapFunctional(state.ctableAddr, tc, state.ctableCap);
    }

    commitCfr(tc, state.base, state.limit, state.meta);
    invalidateCapEntries(state.invalidateIndex);
    return 0;
}

void
CLB::commitPreparedOp(const PreparedMinorOp &op, ThreadContext *tc)
{
    if (!tc) {
        return;
    }

    if (op.writeCfr) {
        commitCfr(tc, op.cfrBase, op.cfrLimit, op.cfrMeta);
    }

    if (op.writeCtable) {
        writeCtableCapFunctional(op.ctableAddr, tc, op.ctableCap);
    }

    if (op.invalidateCap) {
        invalidateCapEntries(op.invalidateIndex);
    }
}

CLB::OpResponse
CLB::prepareMinorGet(unsigned index, ThreadContext *tc, InstSeqNum seq_num)
{
    panic_if(seq_num == 0, "prepareMinorGet requires a dynamic sequence.");
    panic_if(!tc, "prepareMinorGet requires a thread context.");

    PreparedMinorKey key{static_cast<ThreadID>(tc->threadId()), seq_num};
    panic_if(preparedMinorOps.find(key) != preparedMinorOps.end(),
             "Duplicate prepared CLB op for tid=%u seq=%llu",
             key.tid, key.seqNum);

    const auto op = buildGetOp(index, tc);
    preparedMinorOps.emplace(key, op);
    return {.value = op.value, .stall = op.stall};
}

CLB::OpResponse
CLB::prepareMinorDelete(unsigned index, ThreadContext *tc, InstSeqNum seq_num)
{
    panic_if(seq_num == 0, "prepareMinorDelete requires a dynamic sequence.");
    panic_if(!tc, "prepareMinorDelete requires a thread context.");

    PreparedMinorKey key{static_cast<ThreadID>(tc->threadId()), seq_num};
    panic_if(preparedMinorOps.find(key) != preparedMinorOps.end(),
             "Duplicate prepared CLB op for tid=%u seq=%llu",
             key.tid, key.seqNum);

    const auto op = buildDeleteOp(index, tc);
    preparedMinorOps.emplace(key, op);
    return {.value = op.value, .stall = op.stall};
}

void
CLB::discardPreparedMinorOp(ThreadID tid, InstSeqNum seq_num)
{
    if (seq_num == 0) {
        return;
    }

    preparedMinorOps.erase(PreparedMinorKey{tid, seq_num});
}

CLB::OpResponse
CLB::consumePreparedMinorOp(unsigned index, ThreadContext *tc,
                            InstSeqNum seq_num, PreparedMinorOp::Kind kind)
{
    if (!tc || seq_num == 0) {
        return {.value = static_cast<RegVal>(-1), .stall = Cycles(0)};
    }

    PreparedMinorKey key{static_cast<ThreadID>(tc->threadId()), seq_num};
    auto it = preparedMinorOps.find(key);
    panic_if(it == preparedMinorOps.end(),
             "Missing prepared CLB op for tid=%u seq=%llu",
             key.tid, key.seqNum);

    const PreparedMinorOp op = it->second;
    preparedMinorOps.erase(it);

    panic_if(op.kind != kind || op.index != index,
             "Prepared CLB op mismatch for tid=%u seq=%llu",
             key.tid, key.seqNum);

    commitPreparedOp(op, tc);
    return {.value = op.value, .stall = Cycles(0)};
}

// Public wrappers ------------------------------------------------------------

CLB::OpResponse
CLB::getCfr(unsigned index, ThreadContext *tc, InstSeqNum seq_num)
{
    if (seq_num != 0) {
        PreparedMinorKey key{static_cast<ThreadID>(tc->threadId()), seq_num};
        if (preparedMinorOps.find(key) != preparedMinorOps.end()) {
            return consumePreparedMinorOp(index, tc, seq_num,
                                          PreparedMinorOp::Kind::Get);
        }
    }
    return startGetFunctional(index, tc);
}

CLB::OpResponse
CLB::revokeCap(unsigned index, ThreadContext *tc)
{
    return startRevokeFunctional(index, tc);
}

CLB::OpResponse
CLB::deleteCap(unsigned index, ThreadContext *tc, InstSeqNum seq_num)
{
    if (seq_num != 0) {
        PreparedMinorKey key{static_cast<ThreadID>(tc->threadId()), seq_num};
        if (preparedMinorOps.find(key) != preparedMinorOps.end()) {
            return consumePreparedMinorOp(index, tc, seq_num,
                                          PreparedMinorOp::Kind::Delete);
        }
    }
    return startDeleteFunctional(index, tc);
}

void
CLB::flushAll()
{
    preparedMinorOps.clear();
    pendingGets.clear();
    pendingDeletes.clear();

    for (auto &entry : entries) {
        entry = ClbEntry();
    }

    DPRINTF(CLB, "Flushed all CLB entries\n");
}

// Fault helpers and access-control path -------------------------------------

Fault
CLB::createAccessFault(Addr vaddr, BaseMMU::Mode mode) const
{
    ExceptionCode code;
    if (mode == BaseMMU::Read) {
        code = ExceptionCode::LOAD_ACCESS;
    } else if (mode == BaseMMU::Write) {
        code = ExceptionCode::STORE_ACCESS;
    } else {
        code = ExceptionCode::INST_ACCESS;
    }
    return std::make_shared<AddressFault>(vaddr, code);
}

Fault
CLB::createMissFault(Addr vaddr) const
{
    return std::make_shared<CLBMissFault>(vaddr);
}

bool
CLB::permsAllow(uint8_t perms, BaseMMU::Mode mode) const
{
    if (mode == BaseMMU::Read) {
        return perms & PermRead;
    } else if (mode == BaseMMU::Write) {
        return perms & PermWrite;
    } else {
        return perms & PermExec;
    }
}

uint8_t
CLB::currentPid(ThreadContext *tc) const
{
    if (!tc) {
        return 0;
    }

    RegVal satp = tc->readMiscRegNoEffect(MISCREG_SATP);
    auto isa = static_cast<ISA *>(tc->getIsaPtr());
    if (isa->rvType() == RV32) {
        return bits(satp, 29, 22);
    }
    return bits(satp, 59, 44);
}

bool
CLB::currentPidMatches(const ClbEntry &entry, ThreadContext *tc) const
{
    return entry.pid == currentPid(tc);
}

bool
CLB::findOwnedCapInClb(unsigned index, ThreadContext *tc) const
{
    return findOwnedCapLine(index, tc) != nullptr;
}

const CLB::ClbEntry *
CLB::findOwnedCapLine(unsigned index, ThreadContext *tc) const
{
    for (const auto &entry : entries) {
        if (!entry.valid || entry.capIndex != index) {
            continue;
        }

        if (currentPidMatches(entry, tc)) {
            return &entry;
        }
    }

    return nullptr;
}

Fault
CLB::clbCheck(const RequestPtr &req, BaseMMU::Mode mode,
              PrivilegeMode pmode, ThreadContext *tc, Addr vaddr) const
{
    if (isInternalAccess(req)) {
        return NoFault;
    }

    const Addr faultAddr = req->hasVaddr() ? req->getVaddr() : vaddr;
    return lookupAccess(req->getPaddr(), req->getSize(), mode, pmode, tc,
                        faultAddr).fault;
}

CLB::AccessCheckResult
CLB::lookupAccess(Addr paddr, unsigned size, BaseMMU::Mode mode,
                  PrivilegeMode pmode, ThreadContext *tc, Addr vaddr) const
{
    AccessCheckResult result = {};

    if (!_enabled || pmode == PrivilegeMode::PRV_M) {
        return result;
    }

    result.latency = _lookupLatency;

    const Addr faultAddr = vaddr ? vaddr : paddr;
    const Addr end = paddr + size - 1;
    for (const auto &entry : entries) {
        if (!entry.valid) {
            continue;
        }

        if (!currentPidMatches(entry, tc)) {
            continue;
        }

        if (!entry.range.contains(paddr) || !entry.range.contains(end)) {
            continue;
        }

        if (permsAllow(entry.perms, mode)) {
            return result;
        }

        result.fault = createAccessFault(faultAddr, mode);
        return result;
    }

    warn_once("CLB miss fault raised for virtual address %#x.\n", faultAddr);
    result.fault = createMissFault(faultAddr);
    return result;
}

} // namespace RiscvISA
} // namespace gem5
