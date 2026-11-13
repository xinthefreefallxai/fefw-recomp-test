// SPDX-License-Identifier: GPL-3.0-or-later
// [gfx] astc_gpu (src/gfx/nvn/nvn_astc_gpu.h): ASTC textures decoded and encoded by compute shaders, through gpu::tex_ensure_current as the
// game's uploads go. The textures are random ASTC blocks that the CPU decoder decodes without an error colour (so every block mode the
// random bits reach is in them); the CPU decode is what the image must hold.
//   * an RGBA8 image ([gfx] astc_quality = uncompressed): the decode alone, compared byte for byte (mip levels, odd sizes, two footprints)
//   * a BC7 image: the blocks read back, decoded with the reference decoder and held to a PSNR bound; opaque 4x4 blocks stay opaque
//   * a texture over the budget: queued, shown as the CPU stand-in, then transcoded in bands by astc_gpu_pump over the next presents
//   * [gfx] astc_gpu_cache = on: the blocks reach the cache folder, and a second texture with the same storage is served from that entry
//     with the same blocks
// Each case checks the validation layer (when it is installed) and the allocation count after the back end is shut down.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <nxrt/config.h>

#include "astc2bc7.h"
#include "nvn_astc_gpu.h"
#include "nvn_core.h"
#include "nvn_enums.h"
#include "nvn_format.h"
#include "nvn_gpu.h"
#include "nvn_gpu_internal.h"
#include "nvn_texdata.h"
#include "nvn_texture_layout.h"
#include "nxrt/gfx.h"
#include "tegra_tiling.h"
#include "test_framework.h"
#ifdef NXRT_HAVE_BC7_REF
#include "bc7decomp.h"
#endif

NXTEST_GROUP(gfx_nvn_astc_gpu)

namespace nxrt::nvn::be::gpu {
void tex_shutdown();   // nvn_gpu_texture.cpp
}  // namespace nxrt::nvn::be::gpu

using namespace nxrt;
using namespace nxrt::gfx;
using namespace nxrt::nvn;
using nxrt::nvn::be::Ref;
using nxrt::nvn::be::Texture;
using nxrt::nvn::be::TexLayout;
using nxrt::nvn::be::compute_texture_layout;
using nxrt::nvn::be::gpu::Frame;
namespace gpu = nxrt::nvn::be::gpu;

namespace {

uint32_t g_next_id = 9000;

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (*value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

struct Tex {
    Ref<Texture> t;
    TexLayout L;
    std::vector<uint8_t> lin;   // the ASTC blocks, levels back to back
    std::vector<uint8_t> mem;   // the tiled storage
};

bool make_tex(Tex& x, int target, int w, int h, int depth, int levels, int format) {
    TextureBuilderInfo b;
    b.target = target;
    b.width = w;
    b.height = h;
    b.depth = depth;
    b.levels = levels;
    b.format = format;
    x.L = compute_texture_layout(b);
    if (!x.L.valid || !x.L.block_linear) return false;
    x.lin.assign(static_cast<size_t>(x.L.layout.linear_size), 0);
    x.mem.assign(static_cast<size_t>(x.L.storage_size), 0);
    x.t = Ref<Texture>(new Texture());
    Texture& t = *x.t;
    t.b = b;
    t.fmt = x.L.fmt;
    t.mem = x.mem.data();
    t.storage_size = x.L.storage_size;
    t.block_linear = true;
    t.block_height_gobs = x.L.layout.levels[0].gob_blocks_y;
    t.levels = x.L.levels;
    t.layers = x.L.layers;
    t.id = g_next_id++;
    return true;
}

// Random blocks of one footprint that the CPU decoder decodes without an error colour (magenta), 16 bytes each. opaque: only blocks whose every
// texel has alpha 255.
std::vector<uint8_t> valid_blocks(uint32_t bw, uint32_t bh, bool srgb, uint32_t seed, bool opaque) {
    constexpr uint32_t kN = 16384;
    std::vector<uint8_t> cand(static_cast<size_t>(kN) * 16);
    std::mt19937 rng(seed);
    for (size_t i = 0; i < cand.size(); i += 4) {
        const uint32_t v = rng();
        std::memcpy(cand.data() + i, &v, 4);
    }
    a2b_input in{};
    in.format = srgb ? A2B_ASTC_SRGB : A2B_ASTC_UNORM;
    in.block_w = bw;
    in.block_h = bh;
    in.width = kN * bw;
    in.height = bh;
    std::vector<uint8_t> px(static_cast<size_t>(in.width) * in.height * 4);
    std::vector<uint8_t> keep;
    if (a2b_decode_astc_rgba8(&in, cand.data(), cand.size(), 1, px.data(), nullptr) != A2B_OK) return keep;
    for (uint32_t b = 0; b < kN; ++b) {
        bool good = true;
        for (uint32_t y = 0; y < bh && good; ++y)
            for (uint32_t x = 0; x < bw && good; ++x) {
                const uint8_t* p = px.data() + (static_cast<size_t>(y) * in.width + b * bw + x) * 4;
                good = !(p[0] == 255 && p[1] == 0 && p[2] == 255 && p[3] == 255) && (!opaque || p[3] == 255);
            }
        if (good) keep.insert(keep.end(), cand.begin() + static_cast<size_t>(b) * 16, cand.begin() + static_cast<size_t>(b + 1) * 16);
    }
    return keep;
}

// Fills every level and layer with blocks from `pool` and tiles them into the storage. expect: the CPU decode, level after level, layer after layer.
bool fill_and_decode(Tex& x, const std::vector<uint8_t>& pool, std::vector<std::vector<uint8_t>>& expect) {
    if (pool.size() < 16 * 8) return false;
    const FormatInfo& fmt = *x.L.fmt;
    size_t k = 0;
    for (const tegra::LevelLayout& lv : x.L.layout.levels)
        for (uint32_t layer = 0; layer < x.L.layers; ++layer) {
            uint8_t* base = x.lin.data() + lv.linear_offset + static_cast<uint64_t>(layer) * lv.linear_slice_size;
            for (uint32_t i = 0; i < lv.blocks_x * lv.blocks_y; ++i, k = (k + 16) % (pool.size() / 16 * 16)) std::memcpy(base + static_cast<size_t>(i) * 16, pool.data() + k, 16);
            a2b_input in{};
            in.format = fmt.srgb ? A2B_ASTC_SRGB : A2B_ASTC_UNORM;
            in.block_w = fmt.block_w;
            in.block_h = fmt.block_h;
            in.width = lv.width;
            in.height = lv.height;
            std::vector<uint8_t> px(static_cast<size_t>(lv.width) * lv.height * 4);
            if (a2b_decode_astc_rgba8(&in, base, static_cast<size_t>(lv.blocks_x) * lv.blocks_y * 16, 1, px.data(), nullptr) != A2B_OK) return false;
            expect.push_back(std::move(px));
        }
    tegra::tile(x.L.desc, x.L.layout, x.lin.data(), x.mem.data());
    return true;
}

// Every level and layer of the image as the GPU holds it (RGBA8 texels, or 16-byte blocks of a BC7 image), level after level, layer after layer.
bool read_image(Device& dev, Texture* t, const TexLayout& L, bool blocks, std::vector<std::vector<uint8_t>>& out) {
    std::vector<uint64_t> off, size;
    uint64_t total = 0;
    for (const tegra::LevelLayout& lv : L.layout.levels) {
        const uint64_t n = blocks ? static_cast<uint64_t>((lv.width + 3) / 4) * ((lv.height + 3) / 4) * 16 : static_cast<uint64_t>(lv.width) * lv.height * 4;
        off.push_back(total);
        size.push_back(n);
        total += n * L.layers;
    }
    gfx::Buffer rb;
    gpu::finish();
    gpu::run_frame_sync([&](Frame& f) {
        if (dev.create_buffer(total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUse::Readback, &rb, "test readback") != VK_SUCCESS) return;
        const VkImageLayout prev = gpu::tex_layout(t);
        gpu::tex_transition(t, f, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        std::vector<VkBufferImageCopy> regions;
        for (uint32_t l = 0; l < L.levels; ++l) {
            const tegra::LevelLayout& lv = L.layout.levels[l];
            VkBufferImageCopy r = {};
            r.bufferOffset = off[l];
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, L.layers};
            r.imageExtent = {lv.width, lv.height, 1};
            regions.push_back(r);
        }
        vkCmdCopyImageToBuffer(f.cb, gpu::tex_image(t), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, static_cast<uint32_t>(regions.size()), regions.data());
        if (prev != VK_IMAGE_LAYOUT_UNDEFINED) gpu::tex_transition(t, f, prev);
    });
    if (!rb) return false;
    dev.invalidate_buffer(rb);
    const uint8_t* p = static_cast<const uint8_t*>(rb.mapped);
    for (uint32_t l = 0; l < L.levels; ++l)
        for (uint32_t layer = 0; layer < L.layers; ++layer) out.emplace_back(p + off[l] + size[l] * layer, p + off[l] + size[l] * (layer + 1));
    dev.destroy_buffer(rb);
    return true;
}

struct Quality {
    double db = 0;
    uint64_t opaque_lost = 0;   // texels of 4x4 blocks that are opaque in the source and decode with another alpha
    bool decoded = false;
};

// BC7 blocks of a w x h level against the expected texels.
Quality bc7_quality(const std::vector<uint8_t>& blocks, const std::vector<uint8_t>& expect, uint32_t w, uint32_t h) {
    Quality q;
#ifdef NXRT_HAVE_BC7_REF
    const uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    if (blocks.size() != static_cast<size_t>(bx) * by * 16) return q;
    double se = 0;
    for (uint32_t yb = 0; yb < by; ++yb)
        for (uint32_t xb = 0; xb < bx; ++xb) {
            bc7decomp::color_rgba c[16];
            bc7decomp_ref::unpack_bc7(blocks.data() + (static_cast<size_t>(yb) * bx + xb) * 16, c);
            bool opaque = true;   // every texel the block covers (past an edge: the last column or row again)
            for (uint32_t i = 0; i < 16; ++i) {
                const uint32_t x = std::min(xb * 4 + (i & 3), w - 1), y = std::min(yb * 4 + (i >> 2), h - 1);
                opaque = opaque && expect[(static_cast<size_t>(y) * w + x) * 4 + 3] == 255;
            }
            for (uint32_t i = 0; i < 16; ++i) {
                const uint32_t x = xb * 4 + (i & 3), y = yb * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                const uint8_t* e = expect.data() + (static_cast<size_t>(y) * w + x) * 4;
                const uint8_t got[4] = {c[i].r, c[i].g, c[i].b, c[i].a};
                for (int k = 0; k < 4; ++k) {
                    const double d = static_cast<double>(got[k]) - e[k];
                    se += d * d;
                }
                if (opaque && c[i].a != 255) ++q.opaque_lost;
            }
        }
    q.db = se == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 * static_cast<double>(w) * h * 4 / se);
    q.decoded = true;
#else
    (void)blocks;
    (void)expect;
    (void)w;
    (void)h;
#endif
    return q;
}

struct Rig {
    std::unique_ptr<Device> dev;
    uint64_t baseline_allocs = 0;
    std::string cache_dir;
};

// cache: a new folder for [nvn] bc7_cache with [gfx] astc_gpu_cache = on, else no cache at all.
bool bring_up(Rig& r, bool cache, const char* budget_ms = "8", const char* slow = "1") {
    if (cache) {
        std::error_code ec;
        r.cache_dir = (std::filesystem::temp_directory_path(ec) / ("nxrt_astc_gpu_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
        std::filesystem::create_directories(r.cache_dir, ec);
    }
    set_env("NXRT_NVN_BC7_CACHE", r.cache_dir.c_str());
    config_set("nvn", "bc7_cache", r.cache_dir.c_str());
    config_set("gfx", "astc_gpu", "on");
    config_set("gfx", "astc_gpu_cache", cache ? "on" : "off");
    config_set("gfx", "astc_gpu_ms", budget_ms);
    config_set("nvn", "astc_gpu_slow", slow);
    DeviceDesc dd;
    dd.validation = 1;
    dd.headless = 1;
    std::string err;
    r.dev = Device::create(dd, &err);
    if (!r.dev) {
        std::printf("    device: %s\n", err.c_str());
        return false;
    }
    if (!r.dev->validation_enabled()) std::printf("    (no validation layer on this machine: Vulkan usage is not checked)\n");
    r.baseline_allocs = r.dev->memory_stats().allocations;
    if (!gpu::init_for_test(*r.dev, nullptr)) {
        std::printf("    back end did not start\n");
        return false;
    }
    return true;
}

void teardown(Rig& r) {
    if (!r.dev) return;
    gpu::finish();
    gpu::run_on_render_thread([] { gpu::tex_shutdown(); });
    gpu::shutdown_for_test();
    const ValidationStats v = r.dev->validation_stats();
    std::printf("    validation: %llu errors, %llu warnings\n", static_cast<unsigned long long>(v.errors), static_cast<unsigned long long>(v.warnings));
    if (!v.first_error.empty()) std::printf("      first error: %s\n", v.first_error.c_str());
    CHECK_EQ(v.errors, 0ull);
    CHECK_EQ(v.warnings, 0ull);
    CHECK_EQ(r.dev->memory_stats().allocations, r.baseline_allocs);
    r.dev.reset();
    config_set("gfx", "astc_quality", "");
    config_set("gfx", "astc_gpu", "auto");
    config_set("gfx", "astc_gpu_cache", "auto");
    config_set("gfx", "astc_gpu_ms", "8");
    config_set("nvn", "astc_gpu_slow", "1");
    set_env("NXRT_NVN_BC7_CACHE", "");
    config_set("nvn", "bc7_cache", "");
    if (!r.cache_dir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(r.cache_dir, ec);
    }
}

// True when the GPU path can be tested here: the device transcodes ASTC and the path's self-test passed.
bool gpu_path(Rig& r) {
    if (!r.dev->features().bc || r.dev->features().astc_ldr) {
        nxtest::skip("this GPU does not transcode ASTC to BC");
        return false;
    }
    const bool on = gpu::astc_gpu_enabled(true);
    CHECK_MSG(on, "[gfx] astc_gpu = on, but its pipelines or self-test failed on this device");
    return on;
}

uint64_t gpu_transcodes() {
    uint64_t n = 0;
    gpu::run_on_render_thread([&] { n = gpu::tex_stats().transcodes; });
    return n;
}

}  // namespace

// ---- 1: an RGBA8 image holds the CPU decoder's bytes --------------------------------------------------------------------------
NXTEST(decode_to_rgba8_is_bit_exact) {
    Rig r;
    if (!bring_up(r, false)) {
        CHECK(false);
        return;
    }
    if (gpu_path(r)) {
        config_set("gfx", "astc_quality", "uncompressed");
        const struct { int format; int w, h, levels; const char* what; } cases[] = {
            {NVN_FORMAT_RGBA_ASTC_8x8_SRGB, 200, 136, 4, "8x8 sRGB 200x136, 4 levels"},
            {NVN_FORMAT_RGBA_ASTC_5x4, 43, 27, 1, "5x4 43x27"},
            {NVN_FORMAT_RGBA_ASTC_6x6, 96, 96, 7, "6x6 96x96, full chain"},
            {NVN_FORMAT_RGBA_ASTC_4x4_SRGB, 64, 20, 2, "4x4 sRGB 64x20, 2 levels"},
        };
        for (const auto& c : cases) {
            Tex x;
            std::vector<std::vector<uint8_t>> expect, got;
            if (!make_tex(x, NVN_TEXTURE_TARGET_2D, c.w, c.h, 1, c.levels, c.format) ||
                !fill_and_decode(x, valid_blocks(x.L.fmt->block_w, x.L.fmt->block_h, x.L.fmt->srgb, 100 + c.w, false), expect)) {
                CHECK_MSG(false, c.what);
                continue;
            }
            Texture* t = x.t.get();
            VkFormat vk = VK_FORMAT_UNDEFINED;
            gpu::run_frame_sync([&](Frame& f) {
                gpu::tex_ensure_current(t, f);
                vk = gpu::tex_vk_format(t);
            });
            CHECK_MSG(vk == VK_FORMAT_R8G8B8A8_UNORM || vk == VK_FORMAT_R8G8B8A8_SRGB, c.what);
            const bool read = read_image(*r.dev, t, x.L, false, got);
            CHECK_MSG(read && got.size() == expect.size(), c.what);
            size_t differ = 0;
            for (size_t i = 0; read && i < got.size() && i < expect.size(); ++i) differ += got[i] != expect[i];
            CHECK_MSG(differ == 0, (std::string(c.what) + ": levels that differ from the CPU decode: " + std::to_string(differ)).c_str());
            std::printf("    %s: %zu levels, %s\n", c.what, expect.size(), differ == 0 ? "bit exact" : "DIFFERS");
        }
    }
    teardown(r);
}

// ---- 2: a BC7 image is close to the CPU decode, layers and levels in place -----------------------------------------------------
NXTEST(encode_to_bc7_layers_and_levels) {
    Rig r;
    if (!bring_up(r, false)) {
        CHECK(false);
        return;
    }
    if (gpu_path(r)) {
        const struct { int target, format, w, h, depth, levels; bool opaque; const char* what; } cases[] = {
            {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA_ASTC_8x8, 256, 128, 1, 9, false, "8x8 256x128, full chain"},
            {NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA_ASTC_5x4_SRGB, 60, 44, 3, 2, false, "5x4 sRGB 60x44, 3 layers, 2 levels"},
            {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA_ASTC_8x8_SRGB, 128, 128, 1, 1, true, "8x8 sRGB 128x128, opaque blocks"},
        };
        for (const auto& c : cases) {
            Tex x;
            std::vector<std::vector<uint8_t>> expect, got;
            if (!make_tex(x, c.target, c.w, c.h, c.depth, c.levels, c.format) ||
                !fill_and_decode(x, valid_blocks(x.L.fmt->block_w, x.L.fmt->block_h, x.L.fmt->srgb, 200 + c.w, c.opaque), expect)) {
                CHECK_MSG(false, c.what);
                continue;
            }
            Texture* t = x.t.get();
            VkFormat vk = VK_FORMAT_UNDEFINED;
            const uint64_t n0 = gpu_transcodes();
            gpu::run_frame_sync([&](Frame& f) {
                gpu::tex_ensure_current(t, f);
                vk = gpu::tex_vk_format(t);
            });
            CHECK_MSG(vk == VK_FORMAT_BC7_UNORM_BLOCK || vk == VK_FORMAT_BC7_SRGB_BLOCK, c.what);
            CHECK_EQ(gpu_transcodes() - n0, 1ull);
            const bool read = read_image(*r.dev, t, x.L, true, got);
            CHECK_MSG(read && got.size() == expect.size(), c.what);
            double worst = 99.0;
            uint64_t lost = 0;
            bool decoded = true;
            size_t i = 0;
            for (const tegra::LevelLayout& lv : x.L.layout.levels)
                for (uint32_t layer = 0; layer < x.L.layers && read && i < got.size(); ++layer, ++i) {
                    const Quality q = bc7_quality(got[i], expect[i], lv.width, lv.height);
                    decoded = decoded && q.decoded;
                    worst = std::min(worst, q.db);
                    lost += q.opaque_lost;
                }
            if (!decoded) {
                std::printf("    %s: uploaded (no BC7 reference decoder in this build: the blocks are not checked)\n", c.what);
                continue;
            }
            // random blocks are the hardest input there is (the game's textures give 47 dB; these 30 to 43 on an RTX 4070 Super): a level in
            // the wrong place or of the wrong layer gives under 12
            std::printf("    %s: lowest PSNR of a level against the CPU decode %.1f dB (bound 25)\n", c.what, worst);
            CHECK_MSG(worst >= 25.0, c.what);
            CHECK_MSG(lost == 0, (std::string(c.what) + ": texels of opaque blocks that decode with alpha under 255: " + std::to_string(lost)).c_str());
        }
    }
    teardown(r);
}

// ---- 3: over the budget: a stand-in first, then bands over the following presents ---------------------------------------------
NXTEST(queued_texture_is_transcoded_in_bands) {
    Rig r;
    // half a millisecond a present for a GPU the scheduler takes to be 400 times slower: a 512x512 texture is far over it
    if (!bring_up(r, false, "0.5", "400")) {
        CHECK(false);
        return;
    }
    if (gpu_path(r)) {
        Tex x;
        std::vector<std::vector<uint8_t>> expect, standin, got;
        if (make_tex(x, NVN_TEXTURE_TARGET_2D, 512, 512, 1, 10, NVN_FORMAT_RGBA_ASTC_8x8) && fill_and_decode(x, valid_blocks(8, 8, false, 77, false), expect)) {
            Texture* t = x.t.get();
            gpu::run_frame_sync([&](Frame& f) { gpu::tex_ensure_current(t, f); });
            CHECK(gpu::astc_gpu_work());                       // queued
            CHECK(read_image(*r.dev, t, x.L, true, standin));   // the stand-in is in the image
            // one pass a present (the first pass of a present always goes): level 0 is four bands, each smaller level one pass
            const auto present = [&] {
                gpu::counters().presents.fetch_add(1);          // a present went by
                gpu::run_frame_sync([&](Frame& f) { gpu::astc_gpu_pump(f); });
            };
            for (int i = 0; i < 2; ++i) present();
            gpu::run_frame_sync([&](Frame& f) { gpu::tex_ensure_current(t, f); });   // a draw that uses it meanwhile changes nothing
            std::vector<std::vector<uint8_t>> mid;
            CHECK(read_image(*r.dev, t, x.L, true, mid));
            const int presents = 2 + 48;
            for (int i = 0; i < 48; ++i) present();
            CHECK(read_image(*r.dev, t, x.L, true, got));
            const Quality q0 = got.empty() ? Quality() : bc7_quality(got[0], expect[0], 512, 512);
            const Quality qm = mid.empty() ? Quality() : bc7_quality(mid[0], expect[0], 512, 512);
            const Quality qs = standin.empty() ? Quality() : bc7_quality(standin[0], expect[0], 512, 512);
            if (q0.decoded) {
                std::printf("    512x512 level 0: stand-in %.1f dB, after 2 presents %.1f dB, after %d presents %.1f dB\n", qs.db, qm.db, presents, q0.db);
                CHECK(qs.db < qm.db);                           // two bands are in
                CHECK(qm.db < q0.db);                           // ... and two were still to come: the budget held
                double worst = 99.0;
                for (size_t i = 0; i < got.size() && i < expect.size(); ++i) worst = std::min(worst, bc7_quality(got[i], expect[i], x.L.layout.levels[i].width, x.L.layout.levels[i].height).db);
                CHECK_MSG(worst >= 25.0, "a level of the queued texture is not final");
            } else {
                std::printf("    512x512: queued and transcoded over %d presents (no BC7 reference decoder in this build: the blocks are not checked)\n", presents);
            }
        } else {
            CHECK(false);
        }
    }
    teardown(r);
}

// ---- 4: the cache: the blocks are stored, and a texture with the same storage is served from the entry -------------------------
NXTEST(encoded_blocks_reach_the_cache) {
    Rig r;
    if (!bring_up(r, true)) {
        CHECK(false);
        return;
    }
    if (gpu_path(r)) {
        Tex a, b;
        std::vector<std::vector<uint8_t>> expect, ea, got_a, got_b;
        if (make_tex(a, NVN_TEXTURE_TARGET_2D, 256, 256, 1, 9, NVN_FORMAT_RGBA_ASTC_8x8_SRGB) && fill_and_decode(a, valid_blocks(8, 8, true, 5, false), expect) &&
            make_tex(b, NVN_TEXTURE_TARGET_2D, 256, 256, 1, 9, NVN_FORMAT_RGBA_ASTC_8x8_SRGB)) {
            b.lin = a.lin;
            b.mem = a.mem;
            b.t->mem = b.mem.data();
            const uint64_t n0 = gpu_transcodes();
            gpu::run_frame_sync([&](Frame& f) { gpu::tex_ensure_current(a.t.get(), f); });
            CHECK(read_image(*r.dev, a.t.get(), a.L, true, got_a));
            // the entry appears once the GPU has run the frame and the writer thread has written it
            size_t files = 0;
            for (int i = 0; i < 200 && files == 0; ++i) {
                gpu::run_frame_sync([](Frame&) {});   // lets the back end run what the finished submission deferred
                std::error_code ec;
                for (const auto& de : std::filesystem::recursive_directory_iterator(r.cache_dir, ec))
                    if (de.is_regular_file(ec) && de.path().extension() == ".bc7") ++files;
                if (files == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            CHECK_EQ(files, static_cast<size_t>(1));
            // the same storage under another texture: the entry serves it (no transcode), with the blocks the GPU made
            gpu::run_frame_sync([&](Frame& f) { gpu::tex_ensure_current(b.t.get(), f); });
            CHECK(read_image(*r.dev, b.t.get(), b.L, true, got_b));
            CHECK_EQ(gpu_transcodes() - n0, 2ull);   // (the counter counts every upload of transcoded data, from the cache too)
            CHECK(got_a == got_b);
            be::TexCaps caps;
            caps.cache_only = true;                  // an entry or nothing
            be::PreparedTexture prep;
            const bool hit = be::prepare_texture(*b.t, caps, prep);
            CHECK(hit && prep.from_cache && !prep.gpu_astc);
            std::vector<uint8_t> all;
            for (const auto& lvl : got_a) all.insert(all.end(), lvl.begin(), lvl.end());
            CHECK(prep.data == all);
            std::printf("    256x256, 9 levels: %zu cache file, second texture %s\n", files, got_a == got_b ? "identical from the entry" : "DIFFERS");
        } else {
            CHECK(false);
        }
    }
    teardown(r);
}
