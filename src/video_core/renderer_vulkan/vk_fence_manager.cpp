// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <memory>

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/gpu_split.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_fence_manager.h"
#include "video_core/renderer_vulkan/vk_query_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

InnerFence::InnerFence(Scheduler& scheduler_, bool is_stubbed_)
    : FenceBase{is_stubbed_}, scheduler{scheduler_} {}

InnerFence::~InnerFence() = default;

void InnerFence::Queue() {
    if (is_stubbed) {
        return;
    }
    // Get the current tick so we can wait for it
    wait_tick = scheduler.CurrentTick();
    if (!scheduler.DeferFlush()) {
        scheduler.Flush();
    }
}

bool InnerFence::IsSignaled() const {
    if (is_stubbed) {
        return true;
    }
    return scheduler.IsFree(wait_tick);
}

void InnerFence::Wait() {
    if (is_stubbed) {
        return;
    }
    static const bool trace = std::getenv("EDEN_NX2_WAIT_TRACE") != nullptr;
    const auto start = trace ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
    const u64 before = trace ? scheduler.GetMasterSemaphore().KnownGpuTick() : 0;
    const bool submitted = trace && scheduler.IsSubmitted(wait_tick);
    static thread_local u64 wait_count{};
    const bool report = trace && (++wait_count <= 32 || wait_count % 128 == 0);
    if (report) {
        LOG_INFO(Render_Vulkan, "NX2 fence wait begin target={} known_gpu={} submitted={} back={}",
                 wait_tick, before, submitted, VideoCommon::GpuSplit::OnBackThread());
    }
    // A deferred fence flush (Scheduler::DeferFlush) may not be submitted yet. Only the
    // split's back may submit; other threads (the fencing thread) ask it to and wait. If the back
    // has stopped, nothing else uses the scheduler and Wait below submits it.
    if (!scheduler.IsSubmitted(wait_tick) && VideoCommon::GpuSplit::Get().Active() &&
        !VideoCommon::GpuSplit::OnBackThread() && scheduler.WaitSubmitted(wait_tick)) {
        scheduler.GetMasterSemaphore().Wait(wait_tick);
    } else {
        scheduler.Wait(wait_tick);
    }
    if (trace) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start).count();
        static thread_local u64 slow_count{};
        const bool slow_report = us >= 10000 && (++slow_count <= 32 || slow_count % 128 == 0);
        if (report || slow_report) {
            LOG_INFO(Render_Vulkan, "NX2 fence wait end target={} known_before={} known_after={} "
                                    "submitted_before={} elapsed_us={}",
                     wait_tick, before, scheduler.GetMasterSemaphore().KnownGpuTick(), submitted, us);
        }
    }
}

FenceManager::FenceManager(VideoCore::RasterizerInterface& rasterizer_, Tegra::GPU& gpu_,
                           TextureCache& texture_cache_, BufferCache& buffer_cache_,
                           QueryCache& query_cache_, const Device& device_, Scheduler& scheduler_)
    : GenericFenceManager{rasterizer_, gpu_, texture_cache_, buffer_cache_, query_cache_},
      scheduler{scheduler_} {}

Fence FenceManager::CreateFence(bool is_stubbed) {
    return std::make_shared<InnerFence>(scheduler, is_stubbed);
}

void FenceManager::QueueFence(Fence& fence) {
    fence->Queue();
}

bool FenceManager::IsFenceSignaled(Fence& fence) const {
    return fence->IsSignaled();
}

void FenceManager::WaitFence(Fence& fence) {
    fence->Wait();
}

} // namespace Vulkan
