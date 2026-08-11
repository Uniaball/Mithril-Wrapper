// Mithril-Wrapper Vulkan backend -- S6 query objects (M6 stage D).
// Implements the glBeginQuery / glQueryCounter family on VkQueryPool:
//  - occlusion (GL_SAMPLES_PASSED / GL_ANY_SAMPLES_PASSED): every draw in
//    the bracketed range records one occlusion slot (the GL layer calls
//    `Draw` while an occlusion capture is active); the result is the sum of
//    the slot samples. Precise counting hangs off the occlusionQueryPrecise
//    device feature; without it SAMPLES_PASSED degrades (per the graceful-
//    fallback contract: result reads 0, immediately available, not cached),
//    while ANY_SAMPLES_PASSED still answers from the non-zero slot sums.
//  - timestamps (GL_TIME_ELAPSED via BeginQuery, GL_TIMESTAMP via
//    QueryCounter): vkCmdWriteTimestamp slots bracketing the draws; the value
//    is (end-begin)*timestampPeriod in ns, or ticks*period for GL_TIMESTAMP.
//
// Slots live in the owning frame slot's pools (occ_pool / ts_pool created at
// init). Because frames are double-buffered and only retire once the GPU is
// done, a slot's value is safe to read at RetireFrame (the fence guarantees
// availability). This is the seam where this file meets draw.cpp: Draw()
// allocates an occlusion slot when an occlusion capture is active, and marks
// timestamp writes into frame.ts_writes, then RetireFrameQueries (called from
// RetireFrame) drains the values into the owning QueryObj.

#include "internal.h"

#include <algorithm>

namespace mithril::vk {

namespace {

// Allocate a slot out of the current frame's query pool. Returns the slot or
// the pool exhausted sentinel (kOcclusionSlotsPerFrame / kTimestampSlotsPerFrame).
uint32_t TakeOccSlot() {
    FrameSlot& fr = g.frames[g.frame_index];
    if (fr.occ_cursor >= kOcclusionSlotsPerFrame) {
        ML_LOG_WARN("vk: occlusion query pool exhausted");
        return kOcclusionSlotsPerFrame;
    }
    return fr.occ_cursor++;
}

uint32_t TakeTsSlot() {
    FrameSlot& fr = g.frames[g.frame_index];
    if (fr.ts_cursor >= kTimestampSlotsPerFrame) {
        ML_LOG_WARN("vk: timestamp query pool exhausted");
        return kTimestampSlotsPerFrame;
    }
    return fr.ts_cursor++;
}

QueryObj* FindLocked(uint64_t handle) {
    auto it = g.queries.find(handle);
    return it != g.queries.end() ? &it->second : nullptr;
}

uint64_t NewHandle() {
    while (g.query_next == 0 ||
           g.queries.count(g.query_next)) {
        ++g.query_next;
    }
    return g.query_next++;
}

} // namespace

// Allocate an occlusion slot for the draw being recorded right now, under the
// active occlusion query. Called from draw.cpp inside Draw().
bool AllocDrawOccSlot(uint32_t* out_slot) {
    if (!g.active_occ) return false;
    uint32_t slot = TakeOccSlot();
    if (slot >= kOcclusionSlotsPerFrame) return false;
    FrameSlot& fr = g.frames[g.frame_index];
    fr.occ_queries.push_back(g.active_occ);
    auto it = g.queries.find(g.active_occ);
    if (it != g.queries.end()) {
        it->second.occ_slots.emplace_back((uint16_t)g.frame_index, slot);
        it->second.occ_expected++;
    }
    if (out_slot) *out_slot = slot;
    return true;
}

void CreateQueryPools() {
    if (g.fn.CreateQueryPool == nullptr) return;
    VkQueryPoolCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qci.queryCount = kOcclusionSlotsPerFrame;
    qci.queryType = VK_QUERY_TYPE_OCCLUSION;
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (g.fn.CreateQueryPool(g.device, &qci, nullptr,
                                 &g.frames[i].occ_pool) != VK_SUCCESS) {
            ML_LOG_ERROR("vk: CreateQueryPool(occlusion) failed");
            return;   // have_occlusion stays false => queries degrade
        }
    }
    g.have_occlusion = true;

    if (!g.have_timestamps) return;
    qci.queryCount = kTimestampSlotsPerFrame;
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (g.fn.CreateQueryPool(g.device, &qci, nullptr,
                                 &g.frames[i].ts_pool) != VK_SUCCESS) {
            ML_LOG_ERROR("vk: CreateQueryPool(timestamp) failed");
            g.have_timestamps = false;
            return;
        }
    }
}

uint64_t BeginOcclusionQuery(uint32_t target) {
    if (!g.initialized || !g.have_occlusion) return 0;
    // SAMPLES_PASSED requires exact counting; without the precise feature we
    // degrade the query instead of returning unusable rounded counts.
    if (target == GL_SAMPLES_PASSED && !g.occlusion_precise) return 0;
    uint64_t handle = NewHandle();
    QueryObj& q = g.queries[handle];
    q.target = target;
    q.active = true;
    g.active_occ = handle;
    return handle;
}

void EndOcclusionQuery(uint64_t handle) {
    QueryObj* q = FindLocked(handle);
    if (!q) return;
    q->active = false;
    q->ended = true;
    if (g.active_occ == handle) g.active_occ = 0;
}

uint64_t BeginTimeElapsedQuery() {
    if (!g.initialized || !g.have_timestamps) return 0;
    FrameSlot& fr = g.frames[g.frame_index];
    uint32_t slot = TakeTsSlot();
    if (slot >= kTimestampSlotsPerFrame) return 0;
    uint64_t handle = NewHandle();
    QueryObj& q = g.queries[handle];
    q.target = GL_TIME_ELAPSED;
    q.ts_begin_frame = (int)g.frame_index;
    q.ts_begin_slot = (int)slot;
    q.active = true;
    fr.ts_writes.push_back({handle, true, slot, (uint32_t)fr.frame_draws.size()});
    return handle;
}

void EndTimeElapsedQuery(uint64_t handle) {
    QueryObj* q = FindLocked(handle);
    if (!q) return;
    FrameSlot& fr = g.frames[g.frame_index];
    uint32_t slot = TakeTsSlot();
    if (slot >= kTimestampSlotsPerFrame) return;
    q->ts_end_frame = (int)g.frame_index;
    q->ts_end_slot = (int)slot;
    q->active = false;
    q->ended = true;
    fr.ts_writes.push_back({handle, false, slot, (uint32_t)fr.frame_draws.size()});
}

uint64_t QueryCounterTimestamp() {
    if (!g.initialized || !g.have_timestamps) return 0;
    FrameSlot& fr = g.frames[g.frame_index];
    uint32_t slot = TakeTsSlot();
    if (slot >= kTimestampSlotsPerFrame) return 0;
    uint64_t handle = NewHandle();
    QueryObj& q = g.queries[handle];
    q.target = GL_TIMESTAMP;
    q.ts_single_frame = (int)g.frame_index;
    q.ts_single_slot = (int)slot;
    q.ended = true;
    fr.ts_writes.push_back({handle, true, slot, (uint32_t)fr.frame_draws.size()});
    return handle;
}

// Drain an occlusion slot's value into the owning query. Must only run once
// the frame fence signaled (RetireFrame).
static void DrainOccSlot(uint64_t handle, uint16_t frame_idx, uint32_t slot) {
    QueryObj* q = FindLocked(handle);
    if (!q) return;
    if (frame_idx >= kMaxFramesInFlight) return;
    if (g.frames[frame_idx].occ_pool == VK_NULL_HANDLE) return;
    uint64_t value = 0;
    if (g.fn.GetQueryPoolResults(g.device, g.frames[frame_idx].occ_pool, slot, 1,
                                 sizeof(value), &value, sizeof(value),
                                 VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        q->acc += value;
        q->occ_drained++;
    }
}

// Drain a timestamp slot's value into the owning query (must run after the
// frame fence signaled). `is_begin` distinguishes the TIME_ELAPSED begin vs
// end write; a GL_TIMESTAMP single write is stored regardless.
static void DrainTsSlot(uint64_t handle, int frame_idx, int slot,
                        bool is_begin) {
    if (frame_idx < 0 || frame_idx >= (int)kMaxFramesInFlight) return;
    QueryObj* q = FindLocked(handle);
    if (!q) return;
    if (g.frames[frame_idx].ts_pool == VK_NULL_HANDLE) return;
    uint64_t ticks = 0;
    if (g.fn.GetQueryPoolResults(g.device, g.frames[frame_idx].ts_pool, slot, 1,
                                 sizeof(ticks), &ticks, sizeof(ticks),
                                 VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    ticks &= g.ts_valid_mask;
    if (q->target == GL_TIMESTAMP) {
        q->ts_single_ticks = ticks;
        q->ts_single_ready = true;
    } else if (is_begin) {
        q->ts_begin_ticks = ticks;
        q->ts_begin_ready = true;
    } else {
        q->ts_end_ticks = ticks;
        q->ts_end_ready = true;
    }
}

// Called from RetireFrame after waiting the fence: pull every occlusion /
// timestamp value this frame's pools hold into the owning QueryObj. The pools
// themselves are reset at the start of the next SubmitFlush recording (a frame
// slot is only re-recorded after retire, so the reset is always ordered after
// the last submission that used the pool).
void RetireFrameQueries(uint32_t idx) {
    FrameSlot& fr = g.frames[idx];
    if (!g.initialized) return;

    // Occlusion: every query that registered slots in this frame drains them.
    for (uint64_t handle : fr.occ_queries) {
        QueryObj* q = FindLocked(handle);
        if (!q) continue;
        for (auto& [fidx, slot] : q->occ_slots) {
            if (fidx == idx) DrainOccSlot(handle, fidx, slot);
        }
    }
    // Timestamps written in this frame.
    for (auto& tw : fr.ts_writes) {
        QueryObj* q = FindLocked(tw.query);
        if (!q) continue;
        if (q->target == GL_TIMESTAMP) {
            if (q->ts_single_frame == (int)idx)
                DrainTsSlot(tw.query, idx, tw.slot, false);
        } else if (tw.is_begin) {
            if (q->ts_begin_frame == (int)idx)
                DrainTsSlot(tw.query, idx, tw.slot, true);
        } else {
            if (q->ts_end_frame == (int)idx)
                DrainTsSlot(tw.query, idx, tw.slot, false);
        }
    }
    fr.occ_cursor = 0;
    fr.ts_cursor = 0;
    fr.occ_queries.clear();
    fr.ts_writes.clear();
}

bool IsQueryResultAvailable(uint64_t handle) {
    QueryObj* q = FindLocked(handle);
    if (!q) return false;
    // Degraded / no slots: immediately available (reads zero).
    if (q->target == GL_SAMPLES_PASSED || q->target == GL_ANY_SAMPLES_PASSED) {
        if (!g.have_occlusion) return true;
        return q->occ_expected == 0 || q->occ_drained == q->occ_expected;
    }
    // Timestamps.
    if (q->target == GL_TIME_ELAPSED) {
        if (!g.have_timestamps) return true;
        return q->ts_begin_ready && q->ts_end_ready;
    }
    if (q->target == GL_TIMESTAMP) {
        if (!g.have_timestamps) return true;
        return q->ts_single_ready;
    }
    return true;
}

// Compute the final query value in GL semantics. Assumes all data drained.
static bool ProduceResult(QueryObj& q, uint64_t* out) {
    if (q.target == GL_SAMPLES_PASSED || q.target == GL_ANY_SAMPLES_PASSED) {
        *out = q.acc;
        return true;
    }
    if (q.target == GL_TIME_ELAPSED) {
        if (!q.ts_begin_ready || !q.ts_end_ready) return false;
        uint64_t begin = q.ts_begin_ticks & g.ts_valid_mask;
        uint64_t end = q.ts_end_ticks & g.ts_valid_mask;
        uint64_t delta = end >= begin ? end - begin : 0;
        *out = (uint64_t)((double)delta * (double)g.ts_period_ns);
        return true;
    }
    if (q.target == GL_TIMESTAMP) {
        if (!q.ts_single_ready) return false;
        *out = (uint64_t)((double)(q.ts_single_ticks & g.ts_valid_mask) *
                          (double)g.ts_period_ns);
        return true;
    }
    *out = 0;
    return true;
}

bool GetQueryResult64(uint64_t handle, bool wait, uint64_t* out) {
    QueryObj* q = FindLocked(handle);
    if (!q) return false;
    if (wait) SubmitFlush(true);   // declared in engine.h (draw.cpp)
    if (q->result_cached) {
        *out = q->result;
        return true;
    }
    if (!IsQueryResultAvailable(handle)) return false;
    if (!ProduceResult(*q, out)) return false;
    q->result = *out;
    q->result_cached = true;
    return true;
}

void DeleteBackendQuery(uint64_t handle) {
    auto it = g.queries.find(handle);
    if (it == g.queries.end()) return;
    if (g.active_occ == handle) g.active_occ = 0;
    g.queries.erase(it);
}

bool IsTimerQuerySupported() { return g.have_timestamps; }
bool IsOcclusionSupported() { return g.have_occlusion; }
bool IsPreciseOcclusionSupported() { return g.occlusion_precise; }

} // namespace mithril::vk