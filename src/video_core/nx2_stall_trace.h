// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>
#include <mutex>

#include "common/common_types.h"
#include "common/logging.h"

// EDEN_NX2_GPU_STALL_TRACE=1: keeps the last compute dispatches with their storage bindings, and
// when a wait for the GPU passes 5 s, logs the ones the GPU has not finished.
namespace VideoCommon::Nx2StallTrace {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_GPU_STALL_TRACE");
        return value != nullptr && value[0] != '0';
    }();
    return enabled;
}

struct StorageBinding {
    u64 gpu_addr;
    u64 device_addr;
    u32 size;
    u32 index;
    bool written;
    bool multi_range;
};

struct Dispatch {
    u64 tick;
    u64 shader_hash;
    u64 native_key;
    std::array<u32, 3> grid;
    u32 binding_count;
    std::array<StorageBinding, 16> bindings;
};

struct State {
    std::mutex mutex;
    std::array<Dispatch, 128> ring{};
    u64 next = 0;
    Dispatch pending{};
};

inline State& Get() {
    static State state;
    return state;
}

inline void BeginDispatch() {
    if (!Enabled()) {
        return;
    }
    State& state = Get();
    std::scoped_lock lock{state.mutex};
    state.pending = {};
}

inline void NoteStorage(u32 index, u64 gpu_addr, u64 device_addr, u32 size, bool written,
                        bool multi_range) {
    if (!Enabled()) {
        return;
    }
    State& state = Get();
    std::scoped_lock lock{state.mutex};
    Dispatch& pending = state.pending;
    if (pending.binding_count < pending.bindings.size()) {
        pending.bindings[pending.binding_count++] =
            StorageBinding{gpu_addr, device_addr, size, index, written, multi_range};
    }
}

inline void NoteDispatch(u64 tick, u64 shader_hash, u64 native_key, std::array<u32, 3> grid) {
    if (!Enabled()) {
        return;
    }
    State& state = Get();
    std::scoped_lock lock{state.mutex};
    Dispatch& dispatch = state.ring[state.next++ % state.ring.size()];
    dispatch = state.pending;
    dispatch.tick = tick;
    dispatch.shader_hash = shader_hash;
    dispatch.native_key = native_key;
    dispatch.grid = grid;
    state.pending = {};
}

inline void ReportStall(u64 wanted_tick, u64 gpu_tick, u64 waited_ms) {
    State& state = Get();
    std::scoped_lock lock{state.mutex};
    LOG_ERROR(Render_Vulkan,
              "GPU stall: waited {} ms for tick {}, the GPU finished {}; unfinished dispatches:",
              waited_ms, wanted_tick, gpu_tick);
    const u64 count = (std::min<u64>)(state.next, state.ring.size());
    for (u64 i = state.next - count; i < state.next; ++i) {
        const Dispatch& dispatch = state.ring[i % state.ring.size()];
        if (dispatch.tick <= gpu_tick) {
            continue;
        }
        LOG_ERROR(Render_Vulkan, "GPU stall:  tick {} shader {:016x} native {:016x} grid {}x{}x{}",
                  dispatch.tick, dispatch.shader_hash, dispatch.native_key, dispatch.grid[0],
                  dispatch.grid[1], dispatch.grid[2]);
        for (u32 b = 0; b < dispatch.binding_count; ++b) {
            const StorageBinding& binding = dispatch.bindings[b];
            LOG_ERROR(Render_Vulkan,
                      "GPU stall:    ssbo{} gpu {:#x} device {:#x} size {:#x}{}{}", binding.index,
                      binding.gpu_addr, binding.device_addr, binding.size,
                      binding.written ? " written" : "",
                      binding.multi_range ? " multi-range" : "");
        }
    }
}

} // namespace VideoCommon::Nx2StallTrace
