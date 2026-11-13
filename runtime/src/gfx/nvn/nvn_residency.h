// SPDX-License-Identifier: GPL-3.0-or-later
// Texture residency ([gfx] residency, #133, #116, #123, #137): which textures the shaders still sample, and the video memory of the ones
// they stopped sampling given back while this process is near its video memory budget.
//
// The game registers a texture in its texture pool once and samples it through the pool for as long as it keeps it, and it keeps the
// assets of every area it visited (the 0.1.8 census). A texture a binding names is seen when the draw's descriptors are made
// (tex_sampled_view); one the shaders reach through the pool arrays (kPoolSet) is not seen on the CPU at all. So the shaders that index
// the pool arrays are given a store per index (spirv_pool_feedback): word `id` of a storage buffer of the pool pair (binding
// kPoolFeedback of kPoolSet) becomes 1 when a draw indexes texture id `id`. Every kResidencyPeriod presents the render thread copies the
// buffers to host memory and clears them on the GPU timeline; the readback stamps the textures seen.
//
// While the process uses more than [gfx] residency_percent of its budget, textures not seen for [gfx] residency_idle_s are demoted
// (nvn_gpu_texture.cpp, tex_demote): the image is remade from its smallest levels (at most kDemoteDim texels a side), the larger levels
// are dropped like [gfx] texture_detail drops one. A demoted texture that is seen again is promoted: made whole again from the game's
// memory, as the first time. Only textures the CPU filled (never rendered by the GPU) are demoted, so nothing is lost.
//
// NX2navi (Switch 2 Ocarina of Time) marks pool samples in its shaders and evicts textures unsampled for 120 s under pressure; the idea of
// marking the pool shaders is taken from its notes (no code). Hookshot (GPL-3) keeps an LRU of textures touched per submission; the
// "only CPU-filled textures, never the only copy of GPU-drawn texels" rule follows its evict_reason.h.
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

namespace nxrt::nvn::be {
struct Texture;
}

namespace nxrt::nvn::be::gpu {
struct Frame;

constexpr uint32_t kPoolFeedback = 3;   // binding of kPoolSet: uint[kPoolTextureCount], 1 where a shader indexed that texture id

// [gfx] residency: the shaders mark what they sample and idle textures are demoted under pressure. Decided once (before the first shader
// is loaded); the pool set layout and every shader that indexes the pools follow it.
bool residency_on();

// Adds the marks to a module whose pool texture arrays are already in kPoolSet (move_pool_arrays): after each access chain into binding
// kPoolTex2D or kPoolTexCube, `feedback[index] = 1`. Fragment shaders that cannot discard and write no depth or sample mask also get
// EarlyFragmentTests (a store would otherwise keep the depth test after the shader). False (w untouched) when the module cannot be
// rewritten; true also when there was nothing to mark.
bool spirv_pool_feedback(std::vector<uint32_t>& w);

constexpr uint32_t kResidencyPeriod = 30;   // presents between two readbacks of the marks

// A shader that indexes the pools could not be marked: what only it samples would look idle, so nothing is demoted any more and the
// demoted textures are made whole again.
void residency_unmarked();
bool residency_marks_complete();

// A pool pair's marks (device-local, cleared on the GPU timeline) and their host readback (nvn_pipeline.cpp owns one per pool pair).
struct PoolFeedback;
PoolFeedback* feedback_create();            // null when the buffers cannot be made (residency_unmarked is called)
void feedback_destroy(PoolFeedback* fb);    // after the GPU is done with it (defer_destroy)
VkBuffer feedback_buffer(PoolFeedback* fb);
void feedback_record(PoolFeedback* fb, Frame& f);   // copy to the readback and clear, unless a readback is still in flight
// The readback once the GPU finished it (kPoolTextureCount words, nonzero where marked); null when none is ready.
const uint32_t* feedback_take(PoolFeedback* fb, uint64_t completed_serial);

// Start of a submission (render thread, before anything is recorded): every kResidencyPeriod presents, the marks and the demotions.
void residency_begin_submit(Frame& f);
void pool_feedback_tick(Frame& f);   // nvn_pipeline.cpp
void tex_residency_tick(Frame& f);   // nvn_gpu_texture.cpp
// nvn_gpu_texture.cpp, once per present: the demoted textures seen again are made whole, most recently sampled first, within [gfx]
// residency_restore_max / residency_restore_mb a present
void tex_residency_restore(Frame& f);
void tex_mark_seen(Texture* t);      // nvn_gpu_texture.cpp: a shader indexed t through a pool array
// Set by the pool arrays around their tex_sampled_view calls: writing a pool descriptor is not a use (only the marks are).
extern thread_local bool g_resid_pool_view;

}  // namespace nxrt::nvn::be::gpu
