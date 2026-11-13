// SPDX-License-Identifier: GPL-3.0-or-later
// GPU boundary without a GPU: for unit tests of the api files (scripts\nvn_unit.ps1 links this file instead of the real back end).
// The real implementation is nvn_gpu.cpp; the build excludes this file from the library (see cmake/gfx.cmake).
#include <atomic>
#include <chrono>

#include "nvn_gpu.h"

namespace nxrt::nvn::be {
// Destructors of host objects that own GPU resources (defined here so tests link without Vulkan).
Texture::~Texture() { lifetime_texture_freed(this); }
MemoryPool::~MemoryPool() { lifetime_pool_freed(this); }
}  // namespace nxrt::nvn::be

namespace nxrt::nvn::be::gpu {
namespace {
std::atomic<uint64_t> g_serial{0};
}
bool active() { return false; }
bool debug_labels() { return false; }
uint64_t submit(const std::vector<Ref<CmdList>>& lists) { (void)lists; return ++g_serial; }
uint64_t flush() { return g_serial.load(); }
void finish() {}
uint64_t last_serial() { return g_serial.load(); }
bool wait_serial(uint64_t, uint64_t) { return true; }
bool serial_done(uint64_t) { return true; }
void present(Window*, int) {}
void destroy_texture_gpu(TexGpu*) {}
void destroy_pool_gpu(PoolGpu*) {}
void release_texture_gpu(Texture*) {}
void prefetch_pool(MemoryPool*) {}
void prewarm_program_shaders(const ShaderInfo* const*) {}
void program_bound(const ShaderInfo* const*) {}
void mark_texture_cpu_dirty(Texture*) {}
void texture_bound(Texture*) {}
void texture_registered(Texture*) {}
bool download_texture(Texture*) { return false; }
void cpu_will_read(uint64_t, uint64_t) {}
void set_window_modes(int, int) {}
uint64_t timestamp_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace nxrt::nvn::be::gpu
namespace nxrt::nvn::be {
bool astc_bc1_active() { return false; }   // [gfx] astc_bc: no GPU, no BC1 choice (nvn_texdata.h)
}  // namespace nxrt::nvn::be
