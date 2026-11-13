// SPDX-License-Identifier: GPL-3.0-or-later
// GPU ASTC transcode. See nvn_astc_gpu.h for the design.
// Settings (docs/CONFIG.md): [gfx] astc_gpu (auto / on / off), [gfx] astc_gpu_ms (GPU time per present), [gfx] astc_gpu_cache (auto / on /
// off: store the results in [nvn] bc7_cache), [gfx] astc_gpu_upgrade (the CPU re-encode of [gfx] astc_quality = high still runs); test aids
// [nvn] astc_gpu_probe, astc_gpu_fail_test, astc_gpu_slow.
// The dispatch and scratch-chunk scheme follows astral-chain-pc-recomp's runtime/vk_astc.c (MIT, Copyright (c) 2026 Valiantsin Dzerakh;
// THIRD_PARTY_NOTICES.md).
#include "nvn_astc_gpu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "nxrt/config.h"
#include "nxrt/ftrace.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "astc2bc7.h"
#include "nvn_gpu_internal.h"
#include "nvn_texdata.h"

namespace nxrt::nvn::be::gpu {
namespace {

#include "nvn_astc_gpu_shaders.inc"

constexpr const char* kTag = "nvn.astcgpu";
constexpr VkDeviceSize kChunkOut = 32ull << 20;      // RGBA8 texels decoded into the scratch buffer at a time (or one slice, when larger)
constexpr uint32_t kScratchIdlePresents = 600;       // the scratch buffer is released after this many presents without a transcode
constexpr uint64_t kReadbackMax = 256ull << 20;      // encoded blocks waiting to be written to the cache; over it a texture is not stored
constexpr uint32_t kTimeSlots = 64;
// [gfx] astc_gpu_cache = auto stores a texture's blocks only on a GPU slower than this (texels per ms of decode and encode; 2.5 ms per
// M texel). Measured on an RTX 4070 Super (about 1 ms per M texel): the hub's load frame took 2.6 s transcoding its 2,200 textures and
// 5.5 s reading the same textures back from the cache (1 byte a texel from disk and through staging, against a quarter of that as ASTC).
constexpr double kCacheRate = 400e3;
constexpr double kStandinMs = 1.5;                   // what a quick CPU stand-in costs the render thread (preset 0 on the small levels: 1.4 ms measured)

struct DecodePush { uint32_t in_base, out_base, width, height, bw, bh, bx, bslice, srgb; };
struct EncodePush { uint32_t in_base, out_base, width, height, bx, bslice; };

enum class Kind { Rgba, Bc7, Bc1 };

// One level of a texture, or some of its layers when the level is larger than a chunk.
struct Region {
    uint32_t level = 0, layer0 = 0, slices = 1;   // image level, first layer, layers
    uint32_t w = 1, h = 1;                        // texels: the level's, or a band of its rows (whole rows of ASTC and of 4x4 blocks) from y0
    uint32_t y0 = 0;
    uint64_t src = 0;                             // offset of the first slice's ASTC blocks in the source
};

struct Job {
    VkBuffer src = VK_NULL_HANDLE;
    VkDeviceSize src_base = 0;                    // Region::src counts from here
    uint32_t bw = 8, bh = 8;
    bool srgb = false;
    Kind kind = Kind::Bc7;
    std::vector<Region> rg;
};

struct Chunk { size_t i0, i1; VkDeviceSize in, out, bc; };

struct Scratch {
    gfx::Buffer buf;
    VkDeviceSize cap_in = 0, cap_out = 0, cap_bc = 0;   // the ASTC blocks, then the texels, then the encoded blocks
};

uint64_t div_up(uint64_t a, uint64_t b) { return (a + b - 1) / b; }
VkDeviceSize region_in(const Region& r, uint32_t bw, uint32_t bh) { return div_up(r.w, bw) * div_up(r.h, bh) * r.slices * 16; }
VkDeviceSize region_out(const Region& r) { return static_cast<VkDeviceSize>(r.w) * r.h * r.slices * 4; }
VkDeviceSize region_bc(const Region& r, Kind k) { return k == Kind::Rgba ? 0 : div_up(r.w, 4) * div_up(r.h, 4) * r.slices * (k == Kind::Bc1 ? 8 : 16); }

// A texture on its way through the GPU: recorded at once, or queued and recorded over the following presents.
struct Work {
    Ref<Texture> t;
    uint32_t gen = 0;                 // Texture::prep_gen the blocks were read at
    const void* image = nullptr;      // tex_gpu_identity at the time
    PreparedTexture prep;             // the gpu_astc result (its data is empty when a job staged it)
    gfx::Buffer staged;               // the job's upload buffer with the blocks, or null
    Job j;                            // kind, footprint and every region (Region::src: offsets in the blocks)
    size_t next = 0;                  // the first region not recorded yet
    uint64_t texels = 0;
    bool store = false;               // the encoded blocks go to the cache ...
    gfx::Buffer rb;                   // ... through this buffer, filled pass by pass
    VkDeviceSize rb_off = 0;
    uint32_t queued_present = 0;      // the present it was queued in (its stand-in is first shown at the end of that one)
};

struct StoreItem {
    gfx::Buffer rb;
    PreparedTexture meta;             // the gpu_astc result without its data
};

struct State {
    // ---- set up once (init_mu), read by any thread afterwards ----
    std::mutex init_mu;
    std::atomic<int> st{0};           // 0 not tried, 1 on, 2 off
    bool closed = true;               // no device (before astc_gpu_start, after astc_gpu_shutdown): nothing is built
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline decode = VK_NULL_HANDLE, bc7 = VK_NULL_HANDLE, bc1 = VK_NULL_HANDLE;
    VkDescriptorPool test_pool = VK_NULL_HANDLE;   // the two sets of the self-test and the probe
    VkDescriptorSet test_dec = VK_NULL_HANDLE, test_enc = VK_NULL_HANDLE;
    VkQueryPool qpool = VK_NULL_HANDLE;            // kTimeSlots pairs for the frames, then 3 for the self-test and the probe
    VkDeviceSize align = 256;
    double ts_period_ns = 1.0;
    int cache = 2;                    // [gfx] astc_gpu_cache: 0 off, 1 on, 2 auto (only while the GPU is slower than kCacheRate)
    int read = 2;                     // [gfx] astc_gpu_cache_read: 0 off, 1 on, 2 auto (only while the GPU is slower than kCacheRate)
    std::atomic<int> speed{0};        // 0 not measured yet, 1 faster than kCacheRate, 2 slower (rate_now after 6 samples; read by any thread)
    bool upgrade = false;
    double budget_ms = 8.0;
    // ---- render thread ----
    Scratch sc;
    uint64_t sc_gen = 0;              // bumped when the scratch buffer is made again
    uint32_t sc_used_at = 0;          // present count of its last use
    uint64_t set_serial = 0, set_gen = 0;
    VkDescriptorSet set_dec = VK_NULL_HANDLE, set_enc = VK_NULL_HANDLE;
    bool slot_busy[kTimeSlots] = {};
    std::deque<Work> q;
    std::atomic<uint32_t> work{0};    // q.size() + (a scratch buffer exists)
    uint32_t pump_at = ~0u;           // present count of the last pump
    uint32_t budget_at = ~0u;         // present count `spent_ms` belongs to
    double spent_ms = 0.0;            // estimated GPU time of the passes recorded for that present
    uint64_t present_ns = 0;          // when that present was first seen
    double frame_ms = 1000.0 / 30.0;  // present interval, smoothed
    double rate = 150e3;              // texels per millisecond of GPU time (decode and encode), measured
    uint32_t rate_samples = 0;        // passes that went into it
    double peak = 0.0;                // the fastest pass rate seen: what the GPU can do (the game's own work on the GPU only makes passes look slower,
                                      // and the smoothed rate fell below kCacheRate during a hub load on an RTX 4070 Super), for the cache choices
    double slow = 1.0;                // [nvn] astc_gpu_slow (test aid): the budget, the bands and the cache choice see a GPU this many times slower
    // ---- statistics (render thread; the counters any thread) ----
    uint64_t n_tex = 0, n_texels = 0, n_deferred = 0, n_stale = 0, n_timed = 0, timed_texels = 0, n_failed = 0;
    double gpu_ms = 0.0, max_ms = 0.0, max_wait_ms = 0.0, sum_wait_ms = 0.0;   // waits: how long a queued texture's stand-in was on screen
    uint32_t max_wait_presents = 0;
    std::deque<std::pair<uint32_t, uint64_t>> ticks;   // (present, when it began) of the last presents with work
    uint64_t n_waited = 0, cpu_ns = 0;   // cpu_ns: render-thread time spent in astc_gpu_upload and astc_gpu_pump
    uint64_t logged_tex = 0, last_log_ns = 0;
    std::atomic<uint64_t> stored{0}, store_failed{0}, readback_bytes{0};
    // ---- cache writer ----
    std::mutex wmu;
    std::condition_variable wcv;
    std::deque<StoreItem> wq;
    std::thread writer;
    bool wstop = false;
};

State& S() {
    static State* s = new State();   // never destroyed: the writer thread may outlive static destruction
    return *s;
}

std::atomic<uint64_t> g_transcodes{0}, g_deferrals{0};
const bool g_hitch_registered = (nxrt::hitch_add_counter("astc_gpu_transcodes", []() -> uint64_t { return g_transcodes.load(std::memory_order_relaxed); }),
                                 nxrt::hitch_add_counter("astc_gpu_deferred", []() -> uint64_t { return g_deferrals.load(std::memory_order_relaxed); }), true);

void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = ss;
    b.srcAccessMask = sa;
    b.dstStageMask = ds;
    b.dstAccessMask = da;
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

// Consecutive regions whose texels fit a chunk together.
std::vector<Chunk> plan_chunks(const Job& j) {
    std::vector<Chunk> out;
    for (size_t i = 0; i < j.rg.size();) {
        Chunk c{i, i, 0, 0, 0};
        for (; c.i1 < j.rg.size() && (c.i1 == c.i0 || c.out + region_out(j.rg[c.i1]) <= kChunkOut); ++c.i1) {
            c.in += region_in(j.rg[c.i1], j.bw, j.bh);
            c.out += region_out(j.rg[c.i1]);
            c.bc += region_bc(j.rg[c.i1], j.kind);
        }
        out.push_back(c);
        i = c.i1;
    }
    return out;
}

VkDeviceSize round_up(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

// Makes sc hold the largest chunk. False: the buffer could not be made (sc is then empty).
bool scratch_fit(Scratch& sc, const std::vector<Chunk>& chunks, const std::function<void(gfx::Buffer)>& retire) {
    VkDeviceSize in = 0, out = 0, bc = 0;
    for (const Chunk& c : chunks) {
        in = std::max(in, c.in);
        out = std::max(out, c.out);
        bc = std::max(bc, c.bc);
    }
    const VkDeviceSize al = S().align;
    in = round_up(std::max<VkDeviceSize>(in, 16), al);
    out = round_up(std::max<VkDeviceSize>(out, 16), al);
    bc = round_up(std::max<VkDeviceSize>(bc, 16), al);
    if (sc.buf && sc.cap_in >= in && sc.cap_out >= out && sc.cap_bc >= bc) return true;
    in = std::max(in, sc.cap_in);
    out = std::max(out, sc.cap_out);
    bc = std::max(bc, sc.cap_bc);
    if (sc.buf) retire(sc.buf);
    sc = Scratch();
    if (device().create_buffer(in + out + bc, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               gfx::MemoryUse::GpuOnly, &sc.buf, "nvn astc scratch") != VK_SUCCESS) {
        sc = Scratch();
        return false;
    }
    sc.cap_in = in;
    sc.cap_out = out;
    sc.cap_bc = bc;
    return true;
}

void write_sets(const Scratch& sc, VkDescriptorSet dec, VkDescriptorSet enc) {
    const VkDescriptorBufferInfo bi[4] = {{sc.buf.buffer, 0, sc.cap_in},
                                          {sc.buf.buffer, sc.cap_in, sc.cap_out},
                                          {sc.buf.buffer, sc.cap_in, sc.cap_out},
                                          {sc.buf.buffer, sc.cap_in + sc.cap_out, sc.cap_bc}};
    VkWriteDescriptorSet w[4] = {};
    for (int i = 0; i < 4; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = i < 2 ? dec : enc;
        w[i].dstBinding = static_cast<uint32_t>(i & 1);
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(vkdev(), 4, w, 0, nullptr);
}

// What record() hands out per chunk: the scratch buffer and where the chunk's texels and encoded blocks start in it.
struct ChunkOut {
    VkBuffer buf;
    VkDeviceSize texels, blocks;                 // offsets
    VkDeviceSize texel_bytes, block_bytes;
    std::vector<VkBufferImageCopy> copies;       // into the image, one per region (from the blocks, or the texels of an RGBA8 image)
};

// Records the transcode of j, chunk by chunk: the blocks into the scratch, the decode, the encode, then sink (the image copy and any
// read-back). stamp, when set, is called after the decode and after the encode of each chunk (the probe's timestamps).
void record(VkCommandBuffer cb, const Scratch& sc, VkDescriptorSet dec, VkDescriptorSet enc, const Job& j, const std::vector<Chunk>& chunks,
            const std::function<void(const ChunkOut&)>& sink, const std::function<void(int)>* stamp = nullptr) {
    State& s = S();
    const VkPipelineStageFlags2 kCopy = VK_PIPELINE_STAGE_2_COPY_BIT, kComp = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    std::vector<VkBufferCopy> bcp;
    for (const Chunk& c : chunks) {
        // what used the scratch before (an earlier chunk, texture or frame) is done with it
        barrier(cb, kCopy | kComp, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                kCopy | kComp, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        bcp.clear();
        VkDeviceSize in = 0;
        for (size_t i = c.i0; i < c.i1; ++i) {
            const VkDeviceSize n = region_in(j.rg[i], j.bw, j.bh);
            bcp.push_back({j.src_base + j.rg[i].src, in, n});
            in += n;
        }
        vkCmdCopyBuffer(cb, j.src, sc.buf.buffer, static_cast<uint32_t>(bcp.size()), bcp.data());
        barrier(cb, kCopy, VK_ACCESS_2_TRANSFER_WRITE_BIT, kComp, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

        ChunkOut co;
        co.buf = sc.buf.buffer;
        co.texels = sc.cap_in;
        co.blocks = sc.cap_in + sc.cap_out;
        co.texel_bytes = c.out;
        co.block_bytes = c.bc;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s.decode);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 1, &dec, 0, nullptr);
        VkDeviceSize ib = 0, ob = 0;
        for (size_t i = c.i0; i < c.i1; ++i) {
            const Region& r = j.rg[i];
            const uint32_t bx = static_cast<uint32_t>(div_up(r.w, j.bw)), by = static_cast<uint32_t>(div_up(r.h, j.bh));
            const DecodePush pc = {static_cast<uint32_t>(ib / 16), static_cast<uint32_t>(ob / 4), r.w, r.h, j.bw, j.bh, bx, bx * by, j.srgb ? 1u : 0u};
            vkCmdPushConstants(cb, s.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
            const uint32_t bpw = j.bw * j.bh <= 64 ? 64 / (j.bw * j.bh) : 1;   // blocks per workgroup, as astc_decode.comp main
            vkCmdDispatch(cb, (bx + bpw - 1) / bpw, by, r.slices);
            VkBufferImageCopy cp = {};
            cp.bufferOffset = co.texels + ob;
            cp.bufferRowLength = r.w;
            cp.bufferImageHeight = r.h;
            cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, r.level, r.layer0, r.slices};
            cp.imageOffset = {0, static_cast<int32_t>(r.y0), 0};
            cp.imageExtent = {r.w, r.h, 1};
            co.copies.push_back(cp);
            ib += region_in(r, j.bw, j.bh);
            ob += region_out(r);
        }
        if (stamp) (*stamp)(0);
        if (j.kind != Kind::Rgba) {
            barrier(cb, kComp, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kComp, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, j.kind == Kind::Bc1 ? s.bc1 : s.bc7);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 1, &enc, 0, nullptr);
            const uint32_t bb = j.kind == Kind::Bc1 ? 8 : 16;
            VkDeviceSize to = 0, bo = 0;
            for (size_t i = c.i0; i < c.i1; ++i) {
                const Region& r = j.rg[i];
                const uint32_t bx4 = (r.w + 3) / 4, by4 = (r.h + 3) / 4;
                const EncodePush pc = {static_cast<uint32_t>(to / 4), static_cast<uint32_t>(bo / bb), r.w, r.h, bx4, bx4 * by4};
                vkCmdPushConstants(cb, s.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
                vkCmdDispatch(cb, (bx4 + 7) / 8, (by4 + 7) / 8, r.slices);
                VkBufferImageCopy& cp = co.copies[i - c.i0];
                cp.bufferOffset = co.blocks + bo;
                cp.bufferRowLength = bx4 * 4;
                cp.bufferImageHeight = by4 * 4;
                to += region_out(r);
                bo += region_bc(r, j.kind);
            }
        }
        if (stamp) (*stamp)(1);
        barrier(cb, kComp, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCopy, VK_ACCESS_2_TRANSFER_READ_BIT);
        sink(co);
    }
}

// ---- CPU decoders of what the encoders write (the self-test) ------------------------------------------------------------------
const uint16_t kPart2[64] = {0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
                             0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
                             0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
                             0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};
const uint8_t kAnchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
                              15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15};
const int kW2[4] = {0, 21, 43, 64}, kW3[8] = {0, 9, 18, 27, 37, 46, 55, 64}, kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

struct Bits {
    const uint8_t* p;
    int pos = 0;
    uint32_t get(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i, ++pos) v |= static_cast<uint32_t>((p[pos >> 3] >> (pos & 7)) & 1) << i;
        return v;
    }
};

// One BC7 block of mode 1, 3, 5, 6 or 7 (what bc7_encode.comp writes) to 16 RGBA8 texels. False for any other mode.
bool bc7_decode_block(const uint8_t* blk, uint8_t* out) {
    int mode = 0;
    while (mode < 8 && !((blk[0] >> mode) & 1)) ++mode;
    if (mode != 1 && mode < 3) return false;   // modes 0 and 2 (three subsets) are never written
    if (mode > 7) return false;
    Bits b{blk, mode + 1};
    const auto lerp = [](int a, int c, int w) { return static_cast<uint8_t>((a * (64 - w) + c * w + 32) >> 6); };
    if (mode == 4 || mode == 5) {
        const uint32_t rot = b.get(2);
        const bool swap_idx = mode == 4 && b.get(1) != 0;   // mode 4's index mode: the 3-bit indices are the colour's
        const int cbits = mode == 5 ? 7 : 5, abits = mode == 5 ? 8 : 6;
        int e[2][4];
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 2; ++k) {
                const int v = static_cast<int>(b.get(cbits));
                e[k][c] = (v << (8 - cbits)) | (v >> (2 * cbits - 8));
            }
        for (int k = 0; k < 2; ++k) {
            const int v = static_cast<int>(b.get(abits));
            e[k][3] = abits == 8 ? v : (v << 2) | (v >> 4);
        }
        const int nb2 = mode == 5 ? 2 : 3;      // width of the second index set
        int i1[16], i2[16];
        for (int i = 0; i < 16; ++i) i1[i] = static_cast<int>(b.get(i == 0 ? 1 : 2));
        for (int i = 0; i < 16; ++i) i2[i] = static_cast<int>(b.get(i == 0 ? nb2 - 1 : nb2));
        const int* w2 = nb2 == 3 ? kW3 : kW2;
        for (int i = 0; i < 16; ++i) {
            uint8_t* o = out + i * 4;
            const int wc = swap_idx ? w2[i2[i]] : kW2[i1[i]], wa = swap_idx ? kW2[i1[i]] : w2[i2[i]];
            for (int c = 0; c < 3; ++c) o[c] = lerp(e[0][c], e[1][c], wc);
            o[3] = lerp(e[0][3], e[1][3], wa);
            if (rot) std::swap(o[3], o[rot - 1]);
        }
        return true;
    }
    const int subsets = mode == 6 ? 1 : 2, nc = mode == 1 || mode == 3 ? 3 : 4;
    const int cb = mode == 1 ? 6 : mode == 7 ? 5 : 7, nb = mode == 6 ? 4 : mode == 1 ? 3 : 2;
    const uint32_t part = subsets == 2 ? b.get(6) : 0;
    int e[4][4];
    for (int c = 0; c < nc; ++c)
        for (int k = 0; k < subsets * 2; ++k) e[k][c] = static_cast<int>(b.get(cb));
    int pb[4] = {};
    if (mode == 1) {                 // one p bit per subset
        pb[0] = pb[1] = static_cast<int>(b.get(1));
        pb[2] = pb[3] = static_cast<int>(b.get(1));
    } else {
        for (int k = 0; k < subsets * 2; ++k) pb[k] = static_cast<int>(b.get(1));
    }
    for (int k = 0; k < subsets * 2; ++k) {
        for (int c = 0; c < nc; ++c) {
            const int v = (e[k][c] << 1) | pb[k], n = cb + 1;
            e[k][c] = n == 8 ? v : (v << (8 - n)) | (v >> (2 * n - 8));
        }
        if (nc == 3) e[k][3] = 255;
    }
    const int anchor = subsets == 2 ? kAnchor2[part] : -1;
    const int* w = nb == 4 ? kW4 : nb == 3 ? kW3 : kW2;
    for (int i = 0; i < 16; ++i) {
        const int idx = static_cast<int>(b.get(i == 0 || i == anchor ? nb - 1 : nb));
        const int sub = subsets == 2 ? (kPart2[part] >> i) & 1 : 0;
        for (int c = 0; c < 4; ++c) out[i * 4 + c] = lerp(e[sub * 2][c], e[sub * 2 + 1][c], w[idx]);
    }
    return true;
}

void bc1_decode_block(const uint8_t* blk, uint8_t* out) {
    const uint32_t c0 = blk[0] | (blk[1] << 8), c1 = blk[2] | (blk[3] << 8);
    const auto exp = [](uint32_t c, int col[3]) {
        const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        col[0] = (r << 3) | (r >> 2);
        col[1] = (g << 2) | (g >> 4);
        col[2] = (b << 3) | (b >> 2);
    };
    int p[4][3];
    exp(c0, p[0]);
    exp(c1, p[1]);
    for (int c = 0; c < 3; ++c) {
        if (c0 > c1) {
            p[2][c] = (2 * p[0][c] + p[1][c] + 1) / 3;
            p[3][c] = (p[0][c] + 2 * p[1][c] + 1) / 3;
        } else {
            p[2][c] = (p[0][c] + p[1][c]) / 2;
            p[3][c] = 0;
        }
    }
    const uint32_t idx = blk[4] | (blk[5] << 8) | (blk[6] << 16) | (static_cast<uint32_t>(blk[7]) << 24);
    for (int i = 0; i < 16; ++i) {
        const int k = (idx >> (2 * i)) & 3;
        for (int c = 0; c < 3; ++c) out[i * 4 + c] = static_cast<uint8_t>(p[k][c]);
        out[i * 4 + 3] = 255;
    }
}

// ---- synchronous runs (self-test, probe): their own scratch buffer and command buffer --------------------------------------------
struct SyncResult {
    std::vector<uint8_t> rgba, blocks;
    double decode_ms = 0, encode_ms = 0;
};

// Decodes (and encodes for kind) one w x h image of ASTC blocks, or encodes `texels` (RGBA8, w x h) when blocks is null. Waits for the GPU.
bool run_sync(const uint8_t* blocks, const uint8_t* texels, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, bool srgb, Kind kind, SyncResult& out) {
    State& s = S();
    Job j;
    j.bw = bw;
    j.bh = bh;
    j.srgb = srgb;
    j.kind = kind;
    Region r;
    r.w = w;
    r.h = h;
    j.rg.push_back(r);
    const std::vector<Chunk> chunks = plan_chunks(j);
    Scratch sc;
    if (!scratch_fit(sc, chunks, [](gfx::Buffer) {})) return false;
    const VkDeviceSize in_bytes = region_in(r, bw, bh), out_bytes = region_out(r), bc_bytes = region_bc(r, kind);
    const uint32_t q0 = kTimeSlots * 2;   // the three queries after the frames' slots
    gfx::Buffer src, rb;
    bool ok = device().create_buffer(std::max(in_bytes, out_bytes), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, gfx::MemoryUse::Upload, &src, "nvn astc test in") == VK_SUCCESS && src.mapped &&
              device().create_buffer(out_bytes + bc_bytes + 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Readback, &rb, "nvn astc test out") == VK_SUCCESS && rb.mapped;
    if (ok) {
        if (blocks) std::memcpy(src.mapped, blocks, static_cast<size_t>(in_bytes));
        else std::memcpy(src.mapped, texels, static_cast<size_t>(out_bytes));
        device().flush_buffer(src);
        j.src = src.buffer;
        write_sets(sc, s.test_dec, s.test_enc);
        ok = device().submit_once([&](VkCommandBuffer cb) {
            vkCmdResetQueryPool(cb, s.qpool, q0, 3);
            vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.qpool, q0);
            const std::function<void(int)> stamp = [&](int k) { vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.qpool, q0 + 1 + static_cast<uint32_t>(k)); };
            const auto sink = [&](const ChunkOut& co) {
                const VkBufferCopy c1 = {co.texels, 0, out_bytes};
                vkCmdCopyBuffer(cb, co.buf, rb.buffer, 1, &c1);
                if (bc_bytes) {
                    const VkBufferCopy c2 = {co.blocks, out_bytes, bc_bytes};
                    vkCmdCopyBuffer(cb, co.buf, rb.buffer, 1, &c2);
                }
            };
            if (blocks) {
                record(cb, sc, s.test_dec, s.test_enc, j, chunks, sink, &stamp);
            } else {
                // the encoder alone: the texels go straight into the scratch's texel part
                const VkBufferCopy up = {0, sc.cap_in, out_bytes};
                vkCmdCopyBuffer(cb, src.buffer, sc.buf.buffer, 1, &up);
                barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
                stamp(0);
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, kind == Kind::Bc1 ? s.bc1 : s.bc7);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 1, &s.test_enc, 0, nullptr);
                const uint32_t bx4 = (w + 3) / 4, by4 = (h + 3) / 4;
                const EncodePush pc = {0, 0, w, h, bx4, bx4 * by4};
                vkCmdPushConstants(cb, s.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
                vkCmdDispatch(cb, (bx4 + 7) / 8, (by4 + 7) / 8, 1);
                stamp(1);
                barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                ChunkOut co;
                co.buf = sc.buf.buffer;
                co.texels = sc.cap_in;
                co.blocks = sc.cap_in + sc.cap_out;
                sink(co);
            }
            barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        });
    }
    if (ok) {
        device().invalidate_buffer(rb);
        const uint8_t* p = static_cast<const uint8_t*>(rb.mapped);
        out.rgba.assign(p, p + out_bytes);
        out.blocks.assign(p + out_bytes, p + out_bytes + bc_bytes);
        uint64_t ts[3] = {};
        if (vkGetQueryPoolResults(vkdev(), s.qpool, q0, 3, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
            out.decode_ms = static_cast<double>(ts[1] - ts[0]) * s.ts_period_ns * 1e-6;
            out.encode_ms = static_cast<double>(ts[2] - ts[1]) * s.ts_period_ns * 1e-6;
        }
    }
    if (src) device().destroy_buffer(src);
    if (rb) device().destroy_buffer(rb);
    if (sc.buf) device().destroy_buffer(sc.buf);
    return ok;
}

double psnr(double sq_err, uint64_t values) {
    if (sq_err <= 0.0) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 * static_cast<double>(values) / sq_err);
}

// Decodes w x h encoded blocks and returns the squared error against rgba (the colour channels, and alpha for BC7). ok: every block decoded.
double decoded_error(const std::vector<uint8_t>& blocks, Kind kind, const uint8_t* rgba, uint32_t w, uint32_t h, bool* ok, bool* alpha_opaque_ok) {
    const uint32_t bx = (w + 3) / 4, by = (h + 3) / 4, bb = kind == Kind::Bc1 ? 8 : 16;
    double err = 0.0;
    *ok = blocks.size() == static_cast<size_t>(bx) * by * bb;
    if (alpha_opaque_ok) *alpha_opaque_ok = true;
    if (!*ok) return 0.0;
    for (uint32_t y = 0; y < by; ++y)
        for (uint32_t x = 0; x < bx; ++x) {
            uint8_t px[64];
            const uint8_t* b = blocks.data() + (static_cast<size_t>(y) * bx + x) * bb;
            if (kind == Kind::Bc1) bc1_decode_block(b, px);
            else if (!bc7_decode_block(b, px)) { *ok = false; return 0.0; }
            bool opaque = true;
            for (int i = 0; i < 16; ++i) {
                const uint32_t tx = std::min(x * 4 + (i & 3), w - 1), ty = std::min(y * 4 + (i >> 2), h - 1);
                const uint8_t* t = rgba + (static_cast<size_t>(ty) * w + tx) * 4;
                for (int c = 0; c < (kind == Kind::Bc1 ? 3 : 4); ++c) {
                    const double d = static_cast<double>(px[i * 4 + c]) - t[c];
                    err += d * d;
                }
                opaque = opaque && t[3] == 255;
            }
            if (opaque && alpha_opaque_ok)
                for (int i = 0; i < 16; ++i)
                    if (px[i * 4 + 3] != 255) *alpha_opaque_ok = false;
        }
    return err;
}

uint32_t xorshift(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// The decoder against the CPU decoder on random blocks that decode without an error colour, then both encoders on a synthetic image, decoded
// again here and held to an error bound. A driver that miscompiles a shader fails one of them.
bool self_test(std::string& why) {
    char msg[256];
    if (nxrt::config_get_bool("nvn", "astc_gpu_fail_test", false)) {   // test aid: the fallback a failing device takes
        why = "[nvn] astc_gpu_fail_test is set";
        return false;
    }
    // ---- decode ----
    if (a2b_bc7_encoder(nullptr) != A2B_ENCODER_NONE) {
        const struct { uint32_t bw, bh; bool srgb; } fp[] = {{8, 8, false}, {8, 8, true}, {6, 6, false}, {4, 4, true}, {5, 5, false}, {10, 10, false}};
        for (const auto& f : fp) {
            constexpr uint32_t kCols = 64, kRows = 128, kKeepCols = 16;
            std::vector<uint8_t> cand(static_cast<size_t>(kCols) * kRows * 16);
            uint32_t seed = 0x9E3779B9u ^ (f.bw * 131 + f.bh);
            for (size_t i = 0; i < cand.size(); i += 4) {
                const uint32_t v = xorshift(seed);
                std::memcpy(cand.data() + i, &v, 4);
            }
            a2b_input in{};
            in.format = f.srgb ? A2B_ASTC_SRGB : A2B_ASTC_UNORM;
            in.block_w = f.bw;
            in.block_h = f.bh;
            in.width = kCols * f.bw;
            in.height = kRows * f.bh;
            std::vector<uint8_t> ref(static_cast<size_t>(in.width) * in.height * 4);
            if (a2b_decode_astc_rgba8(&in, cand.data(), cand.size(), 1, ref.data(), nullptr) != A2B_OK) continue;   // no CPU decoder to compare with
            std::vector<uint8_t> keep;
            for (uint32_t by = 0; by < kRows; ++by)
                for (uint32_t bx = 0; bx < kCols; ++bx) {
                    bool good = true;
                    for (uint32_t y = 0; y < f.bh && good; ++y)
                        for (uint32_t x = 0; x < f.bw && good; ++x) {
                            const uint8_t* p = ref.data() + ((static_cast<size_t>(by) * f.bh + y) * in.width + bx * f.bw + x) * 4;
                            good = !(p[0] == 255 && p[1] == 0 && p[2] == 255 && p[3] == 255);
                        }
                    if (good) keep.insert(keep.end(), cand.begin() + (static_cast<size_t>(by) * kCols + bx) * 16, cand.begin() + (static_cast<size_t>(by) * kCols + bx + 1) * 16);
                }
            const uint32_t rows = static_cast<uint32_t>(keep.size() / 16 / kKeepCols);
            if (rows < 2) continue;
            keep.resize(static_cast<size_t>(rows) * kKeepCols * 16);
            in.width = kKeepCols * f.bw - 1;    // odd sizes: the last column and row of blocks are cut
            in.height = rows * f.bh - 1;
            ref.assign(static_cast<size_t>(in.width) * in.height * 4, 0);
            if (a2b_decode_astc_rgba8(&in, keep.data(), keep.size(), 1, ref.data(), nullptr) != A2B_OK) continue;
            SyncResult r;
            if (!run_sync(keep.data(), nullptr, in.width, in.height, f.bw, f.bh, f.srgb, Kind::Rgba, r)) {
                why = "the decode could not be run";
                return false;
            }
            size_t diff = 0;
            int worst = 0;
            for (size_t i = 0; i < ref.size(); ++i) {
                const int d = std::abs(static_cast<int>(ref[i]) - static_cast<int>(r.rgba[i]));
                diff += d != 0;
                worst = std::max(worst, d);
            }
            if (worst > 1 || diff > ref.size() / 50) {
                std::snprintf(msg, sizeof msg, "the %ux%u%s decode differs from the CPU decoder in %zu of %zu bytes (largest difference %d) over %u blocks", f.bw, f.bh,
                              f.srgb ? " sRGB" : "", diff, ref.size(), worst, rows * kKeepCols);
                why = msg;
                return false;
            }
            NXLOG_I(kTag, "self-test: ASTC %ux%u%s decode of %u random blocks: %zu of %zu bytes differ from the CPU decoder (largest difference %d)", f.bw, f.bh,
                    f.srgb ? " sRGB" : "", rows * kKeepCols, diff, ref.size(), worst);
        }
    }
    // ---- encode: gradients, two-colour edges and noise on the left (opaque), an alpha ramp on the right ----
    constexpr uint32_t kW = 126, kH = 62;
    std::vector<uint8_t> img(static_cast<size_t>(kW) * kH * 4);
    uint32_t seed = 12345;
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x) {
            uint8_t* p = img.data() + (static_cast<size_t>(y) * kW + x) * 4;
            const uint32_t n = xorshift(seed) & 7;
            if (x < 64) {
                const bool edge = ((x / 3 + y / 5) & 4) != 0 && y >= 32;
                p[0] = static_cast<uint8_t>(edge ? 200 - x : 30 + 2 * x + n);
                p[1] = static_cast<uint8_t>(edge ? 40 + y : 220 - 3 * y + n);
                p[2] = static_cast<uint8_t>(edge ? 90 : 100 + x + y);
                p[3] = 255;
            } else {
                p[0] = static_cast<uint8_t>(250 - n);
                p[1] = static_cast<uint8_t>(120 + (x - 64) + n);
                p[2] = static_cast<uint8_t>(60 + y);
                p[3] = static_cast<uint8_t>(std::min<uint32_t>(255, (x - 64) * 4 + y));
            }
        }
    const struct { Kind kind; const char* name; double floor_db; } enc[] = {{Kind::Bc7, "BC7", 40.0}, {Kind::Bc1, "BC1", 30.0}};
    for (const auto& e : enc) {
        SyncResult r;
        if (!run_sync(nullptr, img.data(), kW, kH, 4, 4, false, e.kind, r)) {
            why = std::string("the ") + e.name + " encode could not be run";
            return false;
        }
        bool ok = false, alpha_ok = true;
        const double err = decoded_error(r.blocks, e.kind, img.data(), kW, kH, &ok, &alpha_ok);
        const uint64_t values = static_cast<uint64_t>((kW + 3) / 4) * ((kH + 3) / 4) * 16 * (e.kind == Kind::Bc1 ? 3 : 4);
        const double db = psnr(err, values);
        // BC1 stores no alpha: its bound is on the colour of the whole image
        if (!ok || db < e.floor_db || (e.kind == Kind::Bc7 && !alpha_ok)) {
            std::snprintf(msg, sizeof msg, "the %s encoder's blocks %s (%.1f dB against the source, at least %.0f expected%s)", e.name, ok ? "are too far from the source" : "do not decode",
                          db, e.floor_db, alpha_ok ? "" : "; opaque blocks decode with alpha under 255");
            why = msg;
            return false;
        }
        NXLOG_I(kTag, "self-test: %s encode of a %ux%u image: %.1f dB against the source", e.name, kW, kH, db);
    }
    return true;
}

// [nvn] astc_gpu_probe = DIR (test aid): every astc_*_<w>x<h>_<bw>x<bh>_<srgb>.bin there (level 0 of a texture as [nvn] dump_astc writes it)
// is decoded and encoded; the texels go to <file>.gpu.rgba, the blocks to <file>.gpu.bc7 and <file>.gpu.bc1, the GPU times to the log.
// The process ends afterwards.
void run_probe(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    uint64_t n = 0, texels = 0;
    double dec_ms = 0, bc7_ms = 0, bc1_ms = 0;
    std::vector<fs::path> files;
    for (const auto& de : fs::directory_iterator(dir, ec))
        if (de.path().extension() == ".bin") files.push_back(de.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& p : files) {
        const std::string name = p.filename().string();
        unsigned w = 0, h = 0, bw = 0, bh = 0, srgb = 0;
        const size_t us = name.find('_', name.find('_', name.find('_') + 1) + 1);   // after astc_t<id>_<key>
        if (us == std::string::npos || std::sscanf(name.c_str() + us, "_%ux%u_%ux%u_%u.bin", &w, &h, &bw, &bh, &srgb) != 5) continue;
        std::ifstream f(p, std::ios::binary);
        std::vector<uint8_t> blocks((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!w || !h || bw < 4 || bh < 4 || blocks.size() < div_up(w, bw) * div_up(h, bh) * 16) continue;
        SyncResult r7, r1;
        if (!run_sync(blocks.data(), nullptr, w, h, bw, bh, srgb != 0, Kind::Bc7, r7) || !run_sync(blocks.data(), nullptr, w, h, bw, bh, srgb != 0, Kind::Bc1, r1)) {
            NXLOG_W(kTag, "probe: %s could not be run", name.c_str());
            continue;
        }
        const auto put = [&](const char* ext, const std::vector<uint8_t>& d) {
            std::ofstream o(p.string() + ext, std::ios::binary | std::ios::trunc);
            o.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
        };
        put(".gpu.rgba", r7.rgba);
        put(".gpu.bc7", r7.blocks);
        put(".gpu.bc1", r1.blocks);
        ++n;
        texels += static_cast<uint64_t>(w) * h;
        dec_ms += r7.decode_ms;
        bc7_ms += r7.encode_ms;
        bc1_ms += r1.encode_ms;
        NXLOG_I(kTag, "probe: %s: decode %.3f ms, BC7 encode %.3f ms, BC1 encode %.3f ms", name.c_str(), r7.decode_ms, r7.encode_ms, r1.encode_ms);
    }
    const double mt = static_cast<double>(texels) / 1e6;
    NXLOG_I(kTag, "probe: %llu textures, %.1f M texels: decode %.1f ms (%.2f ms per M texel), BC7 encode %.1f ms (%.2f), BC1 encode %.1f ms (%.2f)",
            static_cast<unsigned long long>(n), mt, dec_ms, mt > 0 ? dec_ms / mt : 0.0, bc7_ms, mt > 0 ? bc7_ms / mt : 0.0, bc1_ms, mt > 0 ? bc1_ms / mt : 0.0);
    nxrt::log_flush();
    std::_Exit(0);
}

void destroy_objects() {
    State& s = S();
    VkDevice d = vkdev();
    if (s.decode) vkDestroyPipeline(d, s.decode, nullptr);
    if (s.bc7) vkDestroyPipeline(d, s.bc7, nullptr);
    if (s.bc1) vkDestroyPipeline(d, s.bc1, nullptr);
    if (s.layout) vkDestroyPipelineLayout(d, s.layout, nullptr);
    if (s.test_pool) vkDestroyDescriptorPool(d, s.test_pool, nullptr);
    if (s.dsl) vkDestroyDescriptorSetLayout(d, s.dsl, nullptr);
    if (s.qpool) vkDestroyQueryPool(d, s.qpool, nullptr);
    s.decode = s.bc7 = s.bc1 = VK_NULL_HANDLE;
    s.layout = VK_NULL_HANDLE;
    s.test_pool = VK_NULL_HANDLE;
    s.test_dec = s.test_enc = VK_NULL_HANDLE;
    s.dsl = VK_NULL_HANDLE;
    s.qpool = VK_NULL_HANDLE;
}

bool make_pipeline(const uint32_t* code, size_t bytes, VkPipeline* out) {
    State& s = S();
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = bytes;
    mi.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(vkdev(), &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    ci.layout = s.layout;
    const VkResult r = vkCreateComputePipelines(vkdev(), VK_NULL_HANDLE, 1, &ci, nullptr, out);
    vkDestroyShaderModule(vkdev(), m, nullptr);
    return r == VK_SUCCESS;
}

bool create_objects(std::string& why) {
    State& s = S();
    VkDevice d = vkdev();
    const VkPhysicalDeviceProperties& props = device().properties();
    s.align = std::max<VkDeviceSize>(16, props.limits.minStorageBufferOffsetAlignment);
    s.ts_period_ns = props.limits.timestampPeriod;
    if (props.limits.maxComputeWorkGroupInvocations < 64 || props.limits.maxComputeWorkGroupSize[0] < 64) return why = "compute work groups of 64 are not supported", false;
    const VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo sli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 2;
    sli.pBindings = b;
    if (vkCreateDescriptorSetLayout(d, &sli, nullptr, &s.dsl) != VK_SUCCESS) return why = "descriptor set layout", false;
    const VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DecodePush)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &s.dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d, &pli, nullptr, &s.layout) != VK_SUCCESS) return why = "pipeline layout", false;
    if (!make_pipeline(kAstcDecodeComp, sizeof kAstcDecodeComp, &s.decode)) return why = "the decode pipeline did not build", false;
    if (!make_pipeline(kBc7EncodeComp, sizeof kBc7EncodeComp, &s.bc7)) return why = "the BC7 encode pipeline did not build", false;
    if (!make_pipeline(kBc1EncodeComp, sizeof kBc1EncodeComp, &s.bc1)) return why = "the BC1 encode pipeline did not build", false;
    const VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 2;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d, &dpi, nullptr, &s.test_pool) != VK_SUCCESS) return why = "descriptor pool", false;
    for (VkDescriptorSet* set : {&s.test_dec, &s.test_enc}) {
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = s.test_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &s.dsl;
        if (vkAllocateDescriptorSets(d, &ai, set) != VK_SUCCESS) return why = "descriptor sets", false;
    }
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = kTimeSlots * 2 + 3;
    if (vkCreateQueryPool(d, &qi, nullptr, &s.qpool) != VK_SUCCESS) return why = "query pool", false;
    return true;
}

// Caller holds init_mu. Decides st.
void init_locked() {
    State& s = S();
    std::string m = nxrt::config_get_str("gfx", "astc_gpu", "auto");
    std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (m.empty()) m = "auto";
    if (m != "auto" && m != "on" && m != "off" && m != "1" && m != "0" && m != "true" && m != "false") {
        NXLOG_W(kTag, "[gfx] astc_gpu '%s' is not auto, on or off: using auto", m.c_str());
        m = "auto";
    }
    if (m == "off" || m == "0" || m == "false") {
        NXLOG_I(kTag, "ASTC textures are transcoded on the CPU ([gfx] astc_gpu = off)");
        s.st.store(2);
        return;
    }
    const gfx::DeviceFeatures& ft = device().features();
    if (ft.astc_ldr || !ft.bc) {   // nothing to transcode, or nothing to transcode to
        s.st.store(2);
        return;
    }
    // auto leaves integrated GPUs on the CPU path: on an AMD Radeon iGPU a new area's transcodes (with [gfx] astc_bc = bc1, its default
    // there) held frames for 1 to 2 s each, up to Windows' 2 s GPU timeout, and one run ended in VK_ERROR_DEVICE_LOST. [gfx] astc_gpu = on
    // still uses the GPU there.
    if (m == "auto" && device().properties().deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        NXLOG_I(kTag, "ASTC textures are transcoded on the CPU on this integrated GPU ([gfx] astc_gpu = auto; on uses the GPU)");
        s.st.store(2);
        return;
    }
    {
        std::string c = nxrt::config_get_str("gfx", "astc_gpu_cache", "auto");
        std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (c == "on" || c == "1" || c == "true" || c == "yes") s.cache = 1;
        else if (c == "off" || c == "0" || c == "false" || c == "no") s.cache = 0;
        else {
            if (c != "auto" && !c.empty()) NXLOG_W(kTag, "[gfx] astc_gpu_cache '%s' is not auto, on or off: using auto", c.c_str());
            s.cache = 2;
        }
    }
    {
        std::string c = nxrt::config_get_str("gfx", "astc_gpu_cache_read", "auto");
        std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (c == "on" || c == "1" || c == "true" || c == "yes") s.read = 1;
        else if (c == "off" || c == "0" || c == "false" || c == "no") s.read = 0;
        else {
            if (c != "auto" && !c.empty()) NXLOG_W(kTag, "[gfx] astc_gpu_cache_read '%s' is not auto, on or off: using auto", c.c_str());
            s.read = 2;
        }
    }
    s.upgrade = nxrt::config_get_bool("gfx", "astc_gpu_upgrade", false);
    s.budget_ms = std::clamp(nxrt::config_get_double("gfx", "astc_gpu_ms", 8.0), 0.5, 1000.0);
    s.slow = std::clamp(nxrt::config_get_double("nvn", "astc_gpu_slow", 1.0), 1.0, 1000.0);
    const auto t0 = std::chrono::steady_clock::now();
    std::string why;
    bool ok = create_objects(why);
    const double build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ok) ok = self_test(why);
    if (!ok) {
        NXLOG_W(kTag, "GPU ASTC transcode is not used on %s: %s. ASTC textures are transcoded on the CPU, as with [gfx] astc_gpu = off", device().properties().deviceName, why.c_str());
        destroy_objects();
        s.st.store(2);
        return;
    }
    NXLOG_I(kTag, "ASTC textures are decoded and encoded on the GPU ([gfx] astc_gpu = %s): pipelines built in %.0f ms, self-test passed after %.0f ms; up to %.1f ms of GPU time per present, "
                  "results %s, CPU re-encode %s",
            m.c_str(), build_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), s.budget_ms,
            s.cache == 1 ? "stored in [nvn] bc7_cache" : s.cache == 2 ? "stored in [nvn] bc7_cache only if this GPU turns out slow" : "not cached",
            s.upgrade ? "on ([gfx] astc_gpu_upgrade)" : "off");
    NXLOG_I(kTag, "BC7/BC1 entries of [nvn] bc7_cache are %s ([gfx] astc_gpu_cache_read = %s)",
            s.read == 1 ? "read" : s.read == 0 ? "not read: the GPU transcodes again" : (s.cache == 1 || s.upgrade) ? "read" : "not read while this GPU is fast",
            s.read == 1 ? "on" : s.read == 0 ? "off" : "auto");
    const std::string probe = nxrt::config_get_str("nvn", "astc_gpu_probe", "");
    if (!probe.empty()) run_probe(probe);
    s.st.store(1);
}

// ---- cache writer -------------------------------------------------------------------------------------------------------------
void writer_main() {
    log_set_thread_name("nvn-astcgpu");
    State& s = S();
    std::unique_lock<std::mutex> lk(s.wmu);
    for (;;) {
        s.wcv.wait(lk, [&] { return s.wstop || !s.wq.empty(); });
        if (s.wq.empty()) return;   // stop asked and nothing left
        StoreItem it = std::move(s.wq.front());
        s.wq.pop_front();
        lk.unlock();
        const uint64_t bytes = it.rb.size;
        device().invalidate_buffer(it.rb);
        const uint8_t* p = static_cast<const uint8_t*>(it.rb.mapped);
        uint64_t want = 0;
        for (const PreparedLevel& l : it.meta.level) want += div_up(l.width, 4) * div_up(l.height, 4) * it.meta.block_bytes * l.depth * it.meta.layers;
        bool ok = p && want <= bytes;
        if (ok) {
            std::vector<uint8_t> data(p, p + want);
            ok = cache_store_encoded(it.meta, std::move(data));
        }
        device().destroy_buffer(it.rb);
        s.readback_bytes.fetch_sub(bytes);
        if (ok) s.stored.fetch_add(1);
        else s.store_failed.fetch_add(1);
        lk.lock();
    }
}

void store_async(gfx::Buffer rb, PreparedTexture&& meta) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.wmu);
    if (s.wstop) {
        device().destroy_buffer(rb);
        s.readback_bytes.fetch_sub(rb.size);
        return;
    }
    s.wq.push_back(StoreItem{rb, std::move(meta)});
    if (!s.writer.joinable()) s.writer = std::thread(writer_main);
    s.wcv.notify_one();
}

// ---- the frame path (render thread) -----------------------------------------------------------------------------------------
uint32_t presents_now() { return static_cast<uint32_t>(counters().presents.load(std::memory_order_relaxed)); }

Kind kind_of(VkFormat f, bool* ok) {
    *ok = true;
    switch (f) {
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK: return Kind::Bc7;
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return Kind::Bc1;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB: return Kind::Rgba;
        default: *ok = false; return Kind::Rgba;
    }
}

void log_stats(bool force) {
    State& s = S();
    const uint64_t now = ft::now_ns();
    if (!force && (s.n_tex == s.logged_tex || now - s.last_log_ns < 10ull * 1000000000ull)) return;
    if (s.n_tex == 0) return;
    s.last_log_ns = now;
    s.logged_tex = s.n_tex;
    const double mt = static_cast<double>(s.timed_texels) / 1e6;
    NXLOG_I(kTag, "%llu textures (%.1f M texels) transcoded on the GPU, %.0f ms of render thread time for it (%.3f ms a texture): %.1f ms of GPU time over %llu timed passes (%.1f M texels, "
                  "%.2f ms per M texel, longest pass %.1f ms); %llu textures queued over the budget (their stand-ins on screen for %.0f ms on average, %.0f ms and %u presents at most; %zu "
                  "waiting, %llu dropped as stale); %llu stored in the cache (%llu not), %llu failed",
            static_cast<unsigned long long>(s.n_tex), static_cast<double>(s.n_texels) / 1e6, static_cast<double>(s.cpu_ns) * 1e-6,
            static_cast<double>(s.cpu_ns) * 1e-6 / static_cast<double>(s.n_tex), s.gpu_ms, static_cast<unsigned long long>(s.n_timed), mt, mt > 0 ? s.gpu_ms / mt : 0.0, s.max_ms,
            static_cast<unsigned long long>(s.n_deferred), s.n_waited ? s.sum_wait_ms / static_cast<double>(s.n_waited) : 0.0, s.max_wait_ms, s.max_wait_presents, s.q.size(),
            static_cast<unsigned long long>(s.n_stale), static_cast<unsigned long long>(s.stored.load()), static_cast<unsigned long long>(s.store_failed.load()),
            static_cast<unsigned long long>(s.n_failed));
}

struct CpuTimer {
    uint64_t t0 = ft::now_ns();
    ~CpuTimer() { S().cpu_ns += ft::now_ns() - t0; }
};

void update_work() {
    State& s = S();
    s.work.store(static_cast<uint32_t>(s.q.size()) + (s.sc.buf ? 1u : 0u), std::memory_order_relaxed);
}

// ---- the budget ---------------------------------------------------------------------------------------------------------------
// A new present: nothing spent yet. The budget is [gfx] astc_gpu_ms at 30 presents a second and follows the (smoothed) present interval
// from there, so the transcodes take the same share of every frame (a quarter with the default) whatever the frame rate. A long frame
// raises it for the frames after it: the frame that loads an area takes seconds, and what the queue still holds then goes through in about
// one more second of slow frames (the interval decays by a quarter a frame) instead of five seconds at the steady rate.
void tick_present() {
    State& s = S();
    const uint32_t now = presents_now();
    if (s.budget_at == now) return;
    const uint64_t t = ft::now_ns();
    if (s.present_ns && now > s.budget_at) {
        const double dt = static_cast<double>(t - s.present_ns) * 1e-6 / static_cast<double>(now - s.budget_at);   // ms per present since the last look
        s.frame_ms = std::clamp(0.7 * s.frame_ms + 0.3 * dt, 1.0, 1000.0);
    }
    s.present_ns = t;
    s.budget_at = now;
    s.spent_ms = 0.0;
    s.ticks.emplace_back(now, t);
    if (s.ticks.size() > 4096) s.ticks.pop_front();
}

double budget_now() {
    State& s = S();
    return s.budget_ms * s.frame_ms / (1000.0 / 30.0);
}

double rate_now() { return S().rate / S().slow; }
double cost_ms(uint64_t texels) { return static_cast<double>(texels) / rate_now(); }

bool affordable(uint64_t texels) {
    State& s = S();
    return s.spent_ms + cost_ms(texels) <= budget_now();
}

// ---- one texture --------------------------------------------------------------------------------------------------------------
// Checks prep against the image of t and cuts the levels the image takes into regions: whole levels, or for a large level bands of block rows
// of about band_texels (a band can be recorded in a later present than the one before it). False: not a texture this path takes.
bool build_work(Texture* t, PreparedTexture& prep, gfx::Buffer& staged, Work& w) {
    State& s = S();
    bool kind_ok = false;
    w.j.kind = kind_of(prep.format, &kind_ok);
    w.j.bw = prep.src_block_w;
    w.j.bh = prep.src_block_h;
    w.j.srgb = prep.src_srgb;
    if (!kind_ok || w.j.bw < 4 || w.j.bh < 4 || w.j.bw > 12 || w.j.bh > 12 || prep.level.empty() || !prep.layers) return false;
    const uint32_t drop = tex_level_drop(t), levels = tex_levels(t);
    if (tex_layers(t) != prep.layers || tex_vk_format(t) != prep.format) return false;
    // a band is about 2 ms of GPU time at the measured rate, and whole rows of both block sizes (ASTC rows of bh texels, BC rows of 4)
    const uint64_t band_texels = static_cast<uint64_t>(std::clamp(rate_now() * 2.0, 65536.0, 4194304.0));
    uint32_t step = w.j.bh;
    while (step % 4) step += w.j.bh;
    w.texels = 0;
    for (uint32_t i = drop; i < prep.level.size() && i - drop < levels; ++i) {
        const PreparedLevel& pl = prep.level[i];
        if (pl.depth != 1) return false;
        const uint64_t bx = div_up(pl.width, w.j.bw), slice_in = bx * div_up(pl.height, w.j.bh) * 16;
        if (pl.size != slice_in * prep.layers || pl.offset + pl.size > (staged ? staged.size : prep.data.size())) return false;
        const uint64_t slice_texels = static_cast<uint64_t>(pl.width) * pl.height;
        if (slice_texels <= band_texels) {          // whole slices, as many as fit a chunk
            const uint32_t per = static_cast<uint32_t>(std::clamp<uint64_t>(kChunkOut / std::max<uint64_t>(slice_texels * 4, 1), 1, prep.layers));
            for (uint32_t l0 = 0; l0 < prep.layers; l0 += per) {
                Region r;
                r.level = i - drop;
                r.layer0 = l0;
                r.slices = std::min(per, prep.layers - l0);
                r.w = pl.width;
                r.h = pl.height;
                r.src = pl.offset + slice_in * l0;
                w.j.rg.push_back(r);
            }
        } else {
            const uint32_t rows = static_cast<uint32_t>(std::max<uint64_t>(step, band_texels / pl.width / step * step));
            for (uint32_t l = 0; l < prep.layers; ++l)
                for (uint32_t y = 0; y < pl.height; y += rows) {
                    Region r;
                    r.level = i - drop;
                    r.layer0 = l;
                    r.slices = 1;
                    r.w = pl.width;
                    r.h = std::min(rows, pl.height - y);
                    r.y0 = y;
                    r.src = pl.offset + slice_in * l + static_cast<uint64_t>(y / w.j.bh) * bx * 16;
                    w.j.rg.push_back(r);
                }
        }
        w.texels += slice_texels * prep.layers;
    }
    if (w.j.rg.empty()) return false;
    // the cache copy: every level's blocks, read back when the GPU has run them
    const bool cache = s.cache == 1 || (s.cache == 2 && s.speed.load(std::memory_order_relaxed) == 2);
    w.store = cache && w.j.kind != Kind::Rgba && !prep.gpu_cache_file.empty() && drop == 0 && levels >= prep.level.size();
    w.t = Ref<Texture>::share(t);
    w.gen = t->prep_gen.load(std::memory_order_acquire);
    w.image = tex_gpu_identity(t);
    w.prep = std::move(prep);
    w.staged = staged;
    staged = gfx::Buffer{};
    prep = PreparedTexture();
    return true;
}

void drop_work(Work& w) {
    State& s = S();
    if (w.staged) {
        gfx::Buffer b = w.staged;
        defer_destroy([b]() mutable { device().destroy_buffer(b); });   // commands recorded earlier may still read it
        w.staged = gfx::Buffer{};
    }
    if (w.rb) {
        gfx::Buffer b = w.rb;
        s.readback_bytes.fetch_sub(b.size);
        defer_destroy([b]() mutable { device().destroy_buffer(b); });
        w.rb = gfx::Buffer{};
    }
}

enum class Pass { Finished, More, Failed };

// Records the next regions of w into f: all of them, or (budgeted) as many as the present's budget still takes. Failed: nothing usable was
// recorded and w is to be dropped.
Pass run_work(Work& w, Frame& f, bool budgeted) {
    State& s = S();
    Texture* t = w.t.get();
    // the regions of this pass
    Job j;
    j.kind = w.j.kind;
    j.bw = w.j.bw;
    j.bh = w.j.bh;
    j.srgb = w.j.srgb;
    uint64_t texels = 0, src_bytes = 0;
    size_t end = w.next;
    const double room = budget_now() - s.spent_ms;
    for (; end < w.j.rg.size(); ++end) {
        const Region& r = w.j.rg[end];
        const uint64_t n = static_cast<uint64_t>(r.w) * r.h * r.slices;
        // within the budget; the first region of a present goes whatever its size (a band is about 2 ms)
        if (budgeted && cost_ms(texels + n) > room && (end > w.next || s.spent_ms > 0.0)) break;
        texels += n;
        src_bytes += region_in(r, j.bw, j.bh);
        j.rg.push_back(r);
    }
    if (end == w.next) return Pass::More;
    const bool last = end == w.j.rg.size();
    const std::vector<Chunk> chunks = plan_chunks(j);
    const gfx::Buffer before = s.sc.buf;
    if (!scratch_fit(s.sc, chunks, [](gfx::Buffer old) { defer_destroy([old]() mutable { device().destroy_buffer(old); }); })) {
        update_work();
        return Pass::Failed;
    }
    if (s.sc.buf.buffer != before.buffer) ++s.sc_gen;
    update_work();
    if (s.set_serial != f.serial || s.set_gen != s.sc_gen || !s.set_dec || !s.set_enc) {   // sets of this submission's pool, over the current scratch
        s.set_dec = f.alloc_set(s.dsl);
        s.set_enc = f.alloc_set(s.dsl);
        if (!s.set_dec || !s.set_enc) {
            s.set_dec = s.set_enc = VK_NULL_HANDLE;
            return Pass::Failed;
        }
        write_sets(s.sc, s.set_dec, s.set_enc);
        s.set_serial = f.serial;
        s.set_gen = s.sc_gen;
    }
    if (w.store && !w.rb && w.next == 0) {
        uint64_t bytes = 0;
        for (const Region& r : w.j.rg) bytes += region_bc(r, w.j.kind);
        if (s.readback_bytes.load() + bytes > kReadbackMax ||
            device().create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Readback, &w.rb, "nvn astc readback") != VK_SUCCESS || !w.rb.mapped) {
            if (w.rb) device().destroy_buffer(w.rb);
            w.rb = gfx::Buffer{};
            w.store = false;
        } else {
            s.readback_bytes.fetch_add(w.rb.size);
        }
    }
    // the blocks of this pass: in the job's upload buffer (kept until the last pass), or copied from the result into staging
    if (w.staged) {
        j.src = w.staged.buffer;
        j.src_base = 0;
    } else {
        UploadAlloc st = f.staging(src_bytes);
        if (!st) return Pass::Failed;
        uint64_t off = 0;
        for (Region& r : j.rg) {
            const uint64_t n = region_in(r, j.bw, j.bh);
            std::memcpy(st.ptr + off, w.prep.data.data() + r.src, static_cast<size_t>(n));
            r.src = off;
            off += n;
        }
        j.src = st.buffer;
        j.src_base = st.offset;
    }

    int slot = -1;
    for (uint32_t i = 0; i < kTimeSlots; ++i)
        if (!s.slot_busy[i]) { slot = static_cast<int>(i); break; }
    if (slot >= 0) {
        s.slot_busy[slot] = true;
        vkCmdResetQueryPool(f.cb, s.qpool, static_cast<uint32_t>(slot) * 2, 2);
        vkCmdWriteTimestamp2(f.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.qpool, static_cast<uint32_t>(slot) * 2);
    }
    bool copied = true;
    const VkBuffer rb = w.rb.buffer;
    VkDeviceSize rb_off = w.rb_off;
    record(f.cb, s.sc, s.set_dec, s.set_enc, j, chunks, [&](const ChunkOut& co) {
        copied = tex_copy_buffer_regions(t, f, co.buf, co.copies) && copied;
        if (rb) {
            const VkBufferCopy c = {co.blocks, rb_off, co.block_bytes};
            vkCmdCopyBuffer(f.cb, co.buf, rb, 1, &c);
            rb_off += co.block_bytes;
        }
    });
    w.rb_off = rb_off;
    if (slot >= 0) vkCmdWriteTimestamp2(f.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.qpool, static_cast<uint32_t>(slot) * 2 + 1);
    if (rb && last) barrier(f.cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    w.next = end;
    s.spent_ms += cost_ms(texels);
    s.sc_used_at = presents_now();
    s.n_texels += texels;
    if (!copied) {                        // the image went away under it
        if (slot >= 0) s.slot_busy[slot] = false;
        return Pass::Failed;
    }

    // when the GPU has run this submission: the pass's time, and after the last pass the blocks for the cache
    gfx::Buffer store_rb;
    PreparedTexture meta;
    if (last) {
        ++s.n_tex;
        g_transcodes.fetch_add(1, std::memory_order_relaxed);
        if (w.rb) {                       // what cache_store_encoded needs: the format, the level table, the cache file and key (not the blocks)
            store_rb = w.rb;
            w.rb = gfx::Buffer{};
            meta.format = w.prep.format;
            meta.levels = w.prep.levels;
            meta.layers = w.prep.layers;
            meta.block_w = w.prep.block_w;
            meta.block_h = w.prep.block_h;
            meta.block_bytes = w.prep.block_bytes;
            meta.gpu_astc = true;
            meta.gpu_cache_key = w.prep.gpu_cache_key;
            meta.gpu_cache_file = w.prep.gpu_cache_file;
            meta.level = w.prep.level;
        }
        if (w.staged) {
            f.adopt_staging(w.staged);    // freed when this submission has completed
            w.staged = gfx::Buffer{};
        }
    }
    if (slot >= 0 || store_rb) {
        const uint32_t tid = t->id;
        f.defer([slot, store_rb, texels, tid, meta = std::move(meta)]() mutable {
            State& s2 = S();
            if (slot >= 0) {
                uint64_t r[4] = {};
                if (s2.qpool && vkGetQueryPoolResults(vkdev(), s2.qpool, static_cast<uint32_t>(slot) * 2, 2, sizeof r, r, 2 * sizeof(uint64_t),
                                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS && r[1] && r[3] && r[2] >= r[0]) {
                    const double ms = static_cast<double>(r[2] - r[0]) * s2.ts_period_ns * 1e-6;
                    if (ms < 60000.0) {
                        ++s2.n_timed;
                        s2.timed_texels += texels;
                        s2.gpu_ms += ms;
                        if (ms > s2.max_ms) {
                            s2.max_ms = ms;
                            if (ms > 4.0) NXLOG_I(kTag, "texture %u: a pass of %.2f M texels took %.2f ms of GPU time (the longest so far)", tid, static_cast<double>(texels) / 1e6, ms);
                        }
                        if (texels >= 65536 && ms > 0.0) {   // small ones time the fixed cost, not the rate
                            s2.rate = 0.8 * s2.rate + 0.2 * (static_cast<double>(texels) / ms);
                            ++s2.rate_samples;
                            s2.peak = std::max(s2.peak, static_cast<double>(texels) / ms);
                            if (s2.rate_samples >= 6) s2.speed.store(s2.peak / s2.slow < kCacheRate ? 2 : 1, std::memory_order_relaxed);
                        }
                    }
                }
                s2.slot_busy[slot] = false;
            }
            if (store_rb) store_async(store_rb, std::move(meta));
        });
    }
    log_stats(false);
    return last ? Pass::Finished : Pass::More;
}

}  // namespace

bool astc_gpu_enabled(bool wait) {
    State& s = S();
    int v = s.st.load(std::memory_order_acquire);
    if (v == 0 && counters_ready()) {
        if (wait) {
            std::lock_guard<std::mutex> lk(s.init_mu);
            if (s.st.load() == 0 && !s.closed && counters_ready()) init_locked();
        } else {
            // the render thread does not wait for the pipelines: a thread of its own builds them (unless a background job already is)
            static std::atomic<bool> started{false};
            if (!started.exchange(true)) std::thread([] { log_set_thread_name("nvn-astcgpu-init"); astc_gpu_enabled(true); started.store(false); }).detach();
        }
        v = s.st.load(std::memory_order_acquire);
    }
    return v == 1;
}

void astc_gpu_start() {
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.init_mu);
        s.closed = false;
    }
    astc_gpu_enabled(false);
}

bool astc_gpu_ready() { return S().st.load(std::memory_order_acquire) != 0; }

bool astc_gpu_upgrade() { return S().upgrade; }

bool astc_gpu_skip_entries() {
    State& s = S();
    if (s.st.load(std::memory_order_acquire) != 1) return false;
    if (s.read != 2) return s.read == 0;
    // auto: a fast GPU transcodes again (also before its rate is measured: the first area's load is where reading entries costs most); a slow
    // one, one that stores its results, or one with the CPU re-encode on reads them
    return s.cache != 1 && !s.upgrade && s.speed.load(std::memory_order_relaxed) != 2;
}

bool astc_gpu_work() { return S().work.load(std::memory_order_relaxed) != 0; }

AstcGpu astc_gpu_upload(Texture* t, Frame& f, PreparedTexture& prep, gfx::Buffer& staged) {
    State& s = S();
    if (!t || !prep.gpu_astc || s.st.load(std::memory_order_acquire) != 1) return AstcGpu::Failed;
    const CpuTimer timer;
    Work w;
    if (!build_work(t, prep, staged, w)) return AstcGpu::Failed;   // prep and staged are still the caller's
    tick_present();
    // Over this present's budget, or textures are waiting already (they keep their order): queued. Not an RGBA8 image (no stand-in exists for
    // it, and a decode alone is short), and not a texture the GPU is done with sooner than the render thread would be with its stand-in: queueing
    // that one costs the frame more than transcoding it (an area's textures all arrive in one frame: 2,200 stand-ins were 3.3 s of it).
    const bool cheap = cost_ms(w.texels) <= kStandinMs;
    if (w.j.kind != Kind::Rgba && !cheap && (!s.q.empty() || !affordable(w.texels))) {
        w.queued_present = presents_now();
        s.q.push_back(std::move(w));
        ++s.n_deferred;
        g_deferrals.fetch_add(1, std::memory_order_relaxed);
        update_work();
        return AstcGpu::Deferred;
    }
    const Pass p = run_work(w, f, false);
    drop_work(w);
    if (p != Pass::Finished) {
        ++s.n_failed;
        return AstcGpu::Failed;
    }
    return AstcGpu::Done;
}

void astc_gpu_pump(Frame& f) {
    State& s = S();
    const uint32_t now = presents_now();
    if (s.pump_at == now) return;
    s.pump_at = now;
    const CpuTimer timer;
    tick_present();
    while (!s.q.empty()) {
        Work& w = s.q.front();
        Texture* t = w.t.get();
        const bool live = !t->destroyed.load() && t->prep_gen.load(std::memory_order_acquire) == w.gen && w.image && tex_gpu_identity(t) == w.image && tex_vk_format(t) == w.prep.format;
        if (!live) {                       // finalized, written again (the new content comes the usual way) or its image was made again
            drop_work(w);
            s.q.pop_front();
            ++s.n_stale;
            continue;
        }
        const Pass p = run_work(w, f, true);
        if (p == Pass::More) break;        // the budget is used up
        if (p == Pass::Finished) {
            // how long its stand-in was on screen: from the first present that began after it was queued (the frame it was queued in showed it first)
            uint64_t shown = 0;
            for (const auto& tk : s.ticks)
                if (tk.first > w.queued_present) { shown = tk.second; break; }
            const double waited = shown ? static_cast<double>(ft::now_ns() - shown) * 1e-6 : 0.0;
            s.max_wait_ms = std::max(s.max_wait_ms, waited);
            s.sum_wait_ms += waited;
            s.max_wait_presents = std::max(s.max_wait_presents, now - w.queued_present);
            ++s.n_waited;
        }
        if (p == Pass::Failed) {
            // the CPU path takes it from here: the storage is looked at again as after a write
            ++s.n_failed;
            t->cpu_dirty.store(true);
            pool_note_cpu_write(t);
        }
        drop_work(w);
        s.q.pop_front();
    }
    if (s.q.empty() && s.sc.buf && now - s.sc_used_at > kScratchIdlePresents) {
        gfx::Buffer old = s.sc.buf;
        defer_destroy([old]() mutable { device().destroy_buffer(old); });
        s.sc = Scratch();
        ++s.sc_gen;
        log_stats(true);
    }
    update_work();
}

void astc_gpu_shutdown() {
    State& s = S();
    {
        std::unique_lock<std::mutex> lk(s.wmu);
        s.wstop = true;
    }
    s.wcv.notify_all();
    if (s.writer.joinable()) s.writer.join();   // writes what is queued (the device is idle: every read-back is complete)
    {
        std::unique_lock<std::mutex> lk(s.wmu);
        s.wstop = false;
    }
    std::lock_guard<std::mutex> lk(s.init_mu);   // (an initialisation in flight finishes first)
    s.closed = true;
    log_stats(true);
    for (Work& w : s.q) {
        if (w.staged) device().destroy_buffer(w.staged);
        if (w.rb) {
            s.readback_bytes.fetch_sub(w.rb.size);
            device().destroy_buffer(w.rb);
        }
    }
    s.q.clear();
    if (s.sc.buf) device().destroy_buffer(s.sc.buf);
    s.sc = Scratch();
    ++s.sc_gen;
    s.set_dec = s.set_enc = VK_NULL_HANDLE;
    s.set_serial = 0;
    for (bool& b : s.slot_busy) b = false;
    if (s.st.load() == 1) destroy_objects();
    s.st.store(0);
    s.work.store(0);
    s.pump_at = s.budget_at = ~0u;
    s.present_ns = 0;
    s.ticks.clear();
}

}  // namespace nxrt::nvn::be::gpu
