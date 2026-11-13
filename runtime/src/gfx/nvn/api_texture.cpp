// SPDX-License-Identifier: GPL-3.0-or-later
// NVNtexture entry points (family texture, S4d): initialize and finalize, getters, the storage queries of the builder,
// and texel access (WriteTexels, ReadTexels, Flush, Invalidate). A texture is a host object (be::Texture) bound to its guest
// object; its storage is the game's memory inside a pool (be::MemoryPool::base + offset), in the block-linear layout of
// nvn_texture_layout.cpp. Texel access untiles the selected level into a scratch buffer, copies the region and tiles the
// level back (tegra::untile_level / tile_level). Correct first, not fast.
// nvnTextureGetRawStorageClass (no NVN1 signature) follows the NVN2 driver (raw_storage_class, nvn_decoded.h).
// Debug labels are accepted and dropped (be::Texture has no label field).
// Packaged layout handling: smo-recomp runtime/gfx/nvn_gl.c (GPL-3.0), layout/defaults checked, no code copied.
#include <algorithm>
#include <atomic>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include <cstring>
#include <vector>

#include "nvn_core.h"
#include "nvn_gpu.h"
#include "nvn_impl.h"
#include "nvn_texture_layout.h"
#include "nxrt/gauge.h"

namespace nxrt::nvn {
namespace {

using be::MemoryPool;
using be::Texture;
using be::TexLayout;
namespace gpu = be::gpu;

std::atomic<uint32_t> g_texture_id{1};   // texture ids when the builder names no device

// Texture counts (nxrt/gauge.h). A texture is live from its Initialize to its Finalize; created counts every texture object ever made.
nxrt::GaugeCounter g_textures("nvn.textures");
nxrt::GaugeCounter g_textures_created("nvn.textures_created");

Texture* tex_of(const void* guest) { return be::host_of<Texture>(guest); }

uint64_t div_up(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

uint32_t next_texture_id(const NVNdevice* device) {
    if (be::Device* d = be::host_of<be::Device>(device)) return d->next_texture_id.fetch_add(1);
    return g_texture_id.fetch_add(1);
}

// The level and layer a view selects; the texture's first level and layer when the view names none. False when out of range.
bool select_level_layer(const TexLayout& L, const NVNtextureView* view, uint32_t& level, uint32_t& layer) {
    int lvl = 0, lay = 0;
    if (view) {
        const TextureViewInfo v = decode(view);
        if (v.has_levels) lvl = v.min_level;
        if (v.has_layers) lay = v.min_layer;
    }
    if (lvl < 0 || lay < 0 || static_cast<uint32_t>(lvl) >= L.levels || static_cast<uint32_t>(lay) >= L.layers) return false;
    level = static_cast<uint32_t>(lvl);
    layer = static_cast<uint32_t>(lay);
    return true;
}

// Bytes of one layer of a pitch-linear texture.
uint64_t linear_layer_bytes(const TextureBuilderInfo& b, const TexLayout& L) {
    const uint64_t rows = div_up(static_cast<uint64_t>(b.height), L.fmt->block_h);
    const uint64_t depth = b.target == NVN_TEXTURE_TARGET_3D ? static_cast<uint64_t>(b.depth) : 1;
    return L.pitch * rows * depth;
}

// What a texel call works on: the texture, its layout, and the level and layer the view selects.
struct Target {
    Texture* t = nullptr;
    TexLayout L;
    uint32_t level = 0, layer = 0;
    bool ok = false;   // false: no storage, unknown layout or a view out of range (the call does nothing)
    bool is3d() const { return t && t->b.target == NVN_TEXTURE_TARGET_3D; }
};

Target resolve(const NVNtexture* texture, const NVNtextureView* view) {
    Target r;
    r.t = tex_of(texture);
    if (!r.t || !r.t->mem) return r;
    r.L = be::compute_texture_layout(r.t->b);
    if (!r.L.valid) return r;
    r.ok = select_level_layer(r.L, view, r.level, r.layer);
    return r;
}

// The region lies inside the selected level, and a compressed texture's region starts on a block boundary. The z range is
// checked for 3D textures only: for arrays and cube maps the view's layer is the slice.
bool region_ok(const Target& r, const NVNcopyRegion& g) {
    const TextureBuilderInfo& b = r.t->b;
    const uint64_t lw = std::max<uint32_t>(1u, static_cast<uint32_t>(b.width) >> r.level);
    const uint64_t lh = std::max<uint32_t>(1u, static_cast<uint32_t>(b.height) >> r.level);
    if (g.xoffset < 0 || g.yoffset < 0 || g.width <= 0 || g.height <= 0) return false;
    if (static_cast<uint64_t>(g.xoffset) + static_cast<uint64_t>(g.width) > lw) return false;
    if (static_cast<uint64_t>(g.yoffset) + static_cast<uint64_t>(g.height) > lh) return false;
    if (g.xoffset % r.L.fmt->block_w || g.yoffset % r.L.fmt->block_h) return false;
    if (r.is3d()) {
        const uint64_t ld = std::max<uint32_t>(1u, static_cast<uint32_t>(b.depth) >> r.level);
        if (g.zoffset < 0 || g.depth <= 0) return false;
        if (static_cast<uint64_t>(g.zoffset) + static_cast<uint64_t>(g.depth) > ld) return false;
    }
    return true;
}

// Tight rows and images of the region in the caller's data: whole blocks, rows padded to nothing.
void tight_strides(const Target& r, const NVNcopyRegion& g, int64_t& row, int64_t& image) {
    row = static_cast<int64_t>(div_up(g.width, r.L.fmt->block_w)) * r.L.fmt->bytes;
    image = row * static_cast<int64_t>(div_up(g.height, r.L.fmt->block_h));
}

// ---- block-linear texel copies: untile the level, move the region's rows, tile the level back ----------------------------
void write_block_linear(const Target& r, const NVNcopyRegion& g, const uint8_t* src, int64_t row, int64_t image) {
    const tegra::LevelLayout& lv = r.L.layout.levels[r.level];
    const uint32_t bw = r.L.fmt->block_w, bh = r.L.fmt->block_h, bpb = r.L.fmt->bytes;
    const uint32_t bx0 = static_cast<uint32_t>(g.xoffset / bw), by0 = static_cast<uint32_t>(g.yoffset / bh);
    const uint32_t nbx = static_cast<uint32_t>(div_up(g.width, bw)), nby = static_cast<uint32_t>(div_up(g.height, bh));
    const uint32_t z0 = r.is3d() ? static_cast<uint32_t>(g.zoffset) : 0, nz = r.is3d() ? static_cast<uint32_t>(g.depth) : 1;
    const uint64_t slice = static_cast<uint64_t>(lv.linear_row_stride) * lv.blocks_y;

    uint8_t* tiled = r.t->mem + r.layer * r.L.layout.tiled_layer_stride + lv.tiled_offset;
    std::vector<uint8_t> lin(lv.linear_slice_size);
    tegra::untile_level(r.L.desc, lv, tiled, lin.data());
    for (uint32_t z = 0; z < nz; ++z)
        for (uint32_t y = 0; y < nby; ++y) {
            const uint8_t* s = src + static_cast<int64_t>(z) * image + static_cast<int64_t>(y) * row;
            uint8_t* dst = lin.data() + (z0 + z) * slice + static_cast<uint64_t>(by0 + y) * lv.linear_row_stride + uint64_t(bx0) * bpb;
            std::memcpy(dst, s, static_cast<size_t>(nbx) * bpb);
        }
    tegra::tile_level(r.L.desc, lv, lin.data(), tiled);
}

void read_block_linear(const Target& r, const NVNcopyRegion& g, uint8_t* dst, int64_t row, int64_t image) {
    const tegra::LevelLayout& lv = r.L.layout.levels[r.level];
    const uint32_t bw = r.L.fmt->block_w, bh = r.L.fmt->block_h, bpb = r.L.fmt->bytes;
    const uint32_t bx0 = static_cast<uint32_t>(g.xoffset / bw), by0 = static_cast<uint32_t>(g.yoffset / bh);
    const uint32_t nbx = static_cast<uint32_t>(div_up(g.width, bw)), nby = static_cast<uint32_t>(div_up(g.height, bh));
    const uint32_t z0 = r.is3d() ? static_cast<uint32_t>(g.zoffset) : 0, nz = r.is3d() ? static_cast<uint32_t>(g.depth) : 1;
    const uint64_t slice = static_cast<uint64_t>(lv.linear_row_stride) * lv.blocks_y;

    const uint8_t* tiled = r.t->mem + r.layer * r.L.layout.tiled_layer_stride + lv.tiled_offset;
    std::vector<uint8_t> lin(lv.linear_slice_size);
    tegra::untile_level(r.L.desc, lv, tiled, lin.data());
    for (uint32_t z = 0; z < nz; ++z)
        for (uint32_t y = 0; y < nby; ++y) {
            const uint8_t* s = lin.data() + (z0 + z) * slice + static_cast<uint64_t>(by0 + y) * lv.linear_row_stride + uint64_t(bx0) * bpb;
            uint8_t* d = dst + static_cast<int64_t>(z) * image + static_cast<int64_t>(y) * row;
            std::memcpy(d, s, static_cast<size_t>(nbx) * bpb);
        }
}

// ---- pitch-linear texel copies: rows are stored as they are -------------------------------------------------------------
void write_linear(const Target& r, const NVNcopyRegion& g, const uint8_t* src, int64_t row, int64_t image) {
    const TextureBuilderInfo& b = r.t->b;
    const uint64_t bpb = r.L.fmt->bytes, pitch = r.L.pitch;
    const uint64_t rows = div_up(static_cast<uint64_t>(b.height), r.L.fmt->block_h);
    const uint32_t bw = r.L.fmt->block_w, bh = r.L.fmt->block_h;
    const uint64_t bx0 = static_cast<uint64_t>(g.xoffset) / bw, by0 = static_cast<uint64_t>(g.yoffset) / bh;
    const uint64_t nbx = div_up(g.width, bw), nby = div_up(g.height, bh);
    const uint32_t z0 = r.is3d() ? static_cast<uint32_t>(g.zoffset) : 0, nz = r.is3d() ? static_cast<uint32_t>(g.depth) : 1;
    uint8_t* base = r.t->mem + r.layer * linear_layer_bytes(b, r.L);
    for (uint32_t z = 0; z < nz; ++z)
        for (uint32_t y = 0; y < nby; ++y) {
            uint8_t* d = base + (z0 + z) * rows * pitch + (by0 + y) * pitch + bx0 * bpb;
            std::memcpy(d, src + static_cast<int64_t>(z) * image + static_cast<int64_t>(y) * row, nbx * bpb);
        }
}

void read_linear(const Target& r, const NVNcopyRegion& g, uint8_t* dst, int64_t row, int64_t image) {
    const TextureBuilderInfo& b = r.t->b;
    const uint64_t bpb = r.L.fmt->bytes, pitch = r.L.pitch;
    const uint64_t rows = div_up(static_cast<uint64_t>(b.height), r.L.fmt->block_h);
    const uint32_t bw = r.L.fmt->block_w, bh = r.L.fmt->block_h;
    const uint64_t bx0 = static_cast<uint64_t>(g.xoffset) / bw, by0 = static_cast<uint64_t>(g.yoffset) / bh;
    const uint64_t nbx = div_up(g.width, bw), nby = div_up(g.height, bh);
    const uint32_t z0 = r.is3d() ? static_cast<uint32_t>(g.zoffset) : 0, nz = r.is3d() ? static_cast<uint32_t>(g.depth) : 1;
    const uint8_t* base = r.t->mem + r.layer * linear_layer_bytes(b, r.L);
    for (uint32_t z = 0; z < nz; ++z)
        for (uint32_t y = 0; y < nby; ++y) {
            const uint8_t* s = base + (z0 + z) * rows * pitch + (by0 + y) * pitch + bx0 * bpb;
            std::memcpy(dst + static_cast<int64_t>(z) * image + static_cast<int64_t>(y) * row, s, nbx * bpb);
        }
}

void write_texels(const Target& r, const NVNcopyRegion& g, const uint8_t* src, int64_t row, int64_t image) {
    if (r.L.block_linear) write_block_linear(r, g, src, row, image);
    else write_linear(r, g, src, row, image);
    be::tev(r.t, "write_texels");
    be::tx_mark("tx.write", r.t);
    r.t->cpu_dirty.store(true);
    gpu::mark_texture_cpu_dirty(r.t);
}

void read_texels(const Target& r, const NVNcopyRegion& g, uint8_t* dst, int64_t row, int64_t image) {
    gpu::download_texture(r.t);   // false: no GPU image to read back, the storage is read as it is
    if (r.L.block_linear) read_block_linear(r, g, dst, row, image);
    else read_linear(r, g, dst, row, image);
}

// ---- host-backed storage ------------------------------------------------------------------------------------------------------
// A texture whose storage does not lie inside its pool's memory: past the end (a game whose allocator was given more than the pool,
// games/fefw/fefw_res.cpp) or at offset -1 (its allocator found no room). The game's memory has no bytes for it, but the GPU never reads
// textures from there: every texture is a Vulkan image of its own, and the pool's bytes only carry what the CPU writes into the image
// (uploads) and what the GPU wrote back for the CPU to read (nvnTextureReadTexels / InvalidateTexels). In a pool the CPU cannot access
// (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS: render targets, the shadow maps, the window textures, which are presented from their image) neither
// happens, so the image alone is the texture (Texture::overflow). Why it cannot be one: null when it can.
const char* overflow_refusal(const MemoryPool& pool, const TextureBuilderInfo& info) {
    // [nvn] texture_overflow = 0: such a texture is not created (as before).
    static const bool on = nxrt::config_get_bool("nvn", "texture_overflow", true);
    if (!on) return "[nvn] texture_overflow is off";
    if (!(pool.flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) return "the CPU can access the pool, so the storage must be the game's memory";
    if (info.target == NVN_TEXTURE_TARGET_BUFFER) return "a buffer texture reads its texels from the pool's memory";
    return nullptr;
}

nxrt::GaugeCounter g_textures_overflow("nvn.textures_overflow");               // live host-backed textures
nxrt::GaugeCounter g_textures_overflow_bytes("nvn.textures_overflow.bytes");   // their storage size (the images are about that size)

void overflow_gone(const Texture* t) {
    if (!t || !t->overflow) return;
    g_textures_overflow.sub(1);
    g_textures_overflow_bytes.sub(static_cast<int64_t>(t->storage_size));
}

// ---- entry points ---------------------------------------------------------------------------------------------------------
NVNboolean nvnTextureInitialize(NVNtexture* texture, const NVNtextureBuilder* builder) {
    if (!texture || !builder) return 0;
    const TextureBuilderInfo info = decode(builder);
    const TexLayout L = be::compute_texture_layout(info);
    static std::atomic<int> s_fail_logs{0};
    auto fail = [&](const char* why) -> NVNboolean {
        if (s_fail_logs.fetch_add(1) < 16)
            NXLOG_W("nvn", "nvnTextureInitialize: %s (target %d, format %d, %dx%dx%d, offset %lld): texture not created", why, info.target,
                    info.format, info.width, info.height, info.depth, static_cast<long long>(info.offset));
        return 0;
    };
    if (!L.valid) return fail("no layout for this target/format/size");
    MemoryPool* pool = be::host_of<MemoryPool>(info.pool);
    if (!pool) return fail("no pool, or a pool that is not ours");
    bool overflow = false;
    if (!pool->is_virtual && (info.offset < 0 || static_cast<uint64_t>(info.offset) + L.storage_size > pool->size)) {
        const char* refused = overflow_refusal(*pool, info);
        if (refused) {
            static std::atomic<int> s_size_logs{0};
            if (s_size_logs.fetch_add(1) < 16)
                NXLOG_W("nvn", "nvnTextureInitialize: storage 0x%llx at offset 0x%llx, pool size 0x%llx (pool flags 0x%x, texture flags 0x%x, levels %d, samples %d): "
                               "not host-backed: %s",
                        static_cast<unsigned long long>(L.storage_size), static_cast<unsigned long long>(info.offset), static_cast<unsigned long long>(pool->size),
                        static_cast<unsigned>(pool->flags), static_cast<unsigned>(info.flags), info.levels, info.samples, refused);
            return fail("storage does not fit in the pool");
        }
        overflow = true;
        if (info.offset < 0) {   // the game's own allocator failed: no identity for the storage (logged once)
            static std::atomic<bool> s_neg{false};
            if (!s_neg.exchange(true))
                NXLOG_W("nvn", "nvnTextureInitialize: the game placed a %dx%d texture (format %d, 0x%llx bytes) at offset %lld of a pool of 0x%llx bytes (flags 0x%x; "
                               "its allocator found no room): host-backed, but texture objects the game makes over the same storage cannot share its image "
                               "(first case; later ones are not logged)",
                        info.width, info.height, info.format, static_cast<unsigned long long>(L.storage_size), static_cast<long long>(info.offset),
                        static_cast<unsigned long long>(pool->size), static_cast<unsigned>(pool->flags));
        } else {
            static std::atomic<int> s_ovf_logs{0};
            if (s_ovf_logs.fetch_add(1) < 16)
                NXLOG_I("nvn", "nvnTextureInitialize: %dx%dx%d texture (format %d, %d levels, 0x%llx bytes) at offset 0x%llx, past the end of its pool (0x%llx bytes, "
                               "flags 0x%x): host-backed (GPU memory of its own)",
                        info.width, info.height, info.depth, info.format, info.levels, static_cast<unsigned long long>(L.storage_size),
                        static_cast<unsigned long long>(info.offset), static_cast<unsigned long long>(pool->size), static_cast<unsigned>(pool->flags));
        }
    }

    Texture* t = new Texture();
    t->b = info;
    t->fmt = L.fmt;
    t->mem = (pool->base && !pool->is_virtual && !overflow) ? pool->base + info.offset : nullptr;
    if (pool->is_virtual) {
        t->vpool = be::Ref<MemoryPool>::share(pool);
        t->voffset = static_cast<uint64_t>(std::max<ptrdiff_t>(0, info.offset));
    }
    if (overflow) {
        t->overflow = true;
        t->overflow_pool = pool;
        t->overflow_offset = info.offset < 0 ? -1 : static_cast<int64_t>(info.offset);
        t->cpu_dirty.store(false);   // nothing in the game's memory to upload
        g_textures_overflow.add(1);
        g_textures_overflow_bytes.add(static_cast<int64_t>(L.storage_size));
    }
    // A render target inside a pool the CPU cannot access: the game has written nothing there, so the first use does not upload the
    // pool's bytes into the image (a render-size rebuild made 40 to 60 such uploads, 200 to 320 MB, in one frame: most of its hitch). It
    // starts cleared, as a host-backed one. A write the runtime itself makes to the storage still marks it dirty and uploads.
    // Only textures created as render targets or window textures by default: a pool can be made CPU_NO_ACCESS over memory the game filled
    // before (file textures), and those must still be uploaded. [nvn] tex_no_access_upload = all: every texture of such a pool is
    // uploaded, as before; none: none is (an experiment).
    static const std::string no_access_mode = nxrt::config_get_str("nvn", "tex_no_access_upload", "targets");
    if (!overflow && t->mem && (pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) && info.target != NVN_TEXTURE_TARGET_BUFFER && no_access_mode != "all") {
        const bool target = (info.flags & (NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) != 0;
        if (target || no_access_mode == "none") {
            t->cpu_no_access = true;
            t->cpu_dirty.store(false);
        } else {
            static std::atomic<int> s_logs{0};
            if (s_logs.fetch_add(1) < 12)
                NXLOG_I("nvn", "nvnTextureInitialize: %dx%d texture (format %d, flags 0x%x) in a CPU_NO_ACCESS pool is not a render target: uploaded from the pool",
                        info.width, info.height, info.format, static_cast<unsigned>(info.flags));
        }
    }
    t->storage_size = L.storage_size;
    t->block_linear = L.block_linear;
    t->block_height_gobs = L.block_linear ? L.layout.levels[0].gob_blocks_y : 0;
    t->is_display = (info.flags & NVN_TEXTURE_FLAGS_DISPLAY) != 0;
    t->cpu_coherent = t->mem && (pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED) && !(pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS);
    t->levels = L.levels;
    t->layers = L.layers;
    t->id = next_texture_id(info.device);
    g_textures_created.add(1);
    const bool was_live = tex_of(texture) != nullptr;        // initialized over a live texture: replaced, not added

    if (Texture* old = tex_of(texture)) {                    // initialized twice: the old one goes
        be::texture_registry_remove(old);
        be::texture_registry_remove_virtual(old);
        overflow_gone(old);
        old->destroyed.store(true);
        be::lifetime_texture_finalized();
        gpu::release_texture_gpu(old);
    }
    be::bind_guest(texture, t);                              // the guest object takes the one reference of t
    if (!was_live) g_textures.add(1);
    be::texture_registry_add(t);
    be::texture_registry_add_virtual(t);
    be::tev(t, "init", static_cast<uint64_t>(info.offset), t->storage_size);
    be::tx_mark("tx.init", t);
    be::g_tex_scan_epoch.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

void nvnTextureFinalize(NVNtexture* texture) {
    Texture* t = tex_of(texture);
    if (!t) return;
    be::tev(t, "finalize");
    t->destroyed.store(true);
    be::lifetime_texture_finalized();
    gpu::release_texture_gpu(t);
    be::texture_registry_remove(t);
    be::texture_registry_remove_virtual(t);
    overflow_gone(t);
    g_textures.sub(1);
    be::unbind_guest(texture);
}

void nvnTextureSetDebugLabel(NVNtexture* texture, const char* label) {
    (void)texture;
    (void)label;   // be::Texture has no label field; the label is dropped
}

size_t nvnTextureGetZCullStorageSize(const NVNtexture* texture) {
    (void)texture;
    return 0;   // no ZCull storage in this back end
}

NVNstorageClass nvnTextureGetStorageClass(const NVNtexture* texture) {
    (void)texture;
    return 0;   // the storage class only matters for compressed or sparse textures, not modelled here
}

// NVN2: int (const NVNtexture*), from the flags and format the texture was built with.
int nvnTextureGetRawStorageClass(const NVNtexture* texture) {
    Texture* t = tex_of(texture);
    return t ? raw_storage_class(t->b.flags, t->b.format) : 0;
}

ptrdiff_t nvnTextureGetViewOffset(const NVNtexture* texture, const NVNtextureView* view) {
    Texture* t = tex_of(texture);
    if (!t) return 0;
    const TexLayout L = be::compute_texture_layout(t->b);
    uint32_t level = 0, layer = 0;
    if (!L.valid || !select_level_layer(L, view, level, layer)) return 0;
    if (!L.block_linear) return static_cast<ptrdiff_t>(layer * linear_layer_bytes(t->b, L));
    return static_cast<ptrdiff_t>(L.layout.levels[level].tiled_offset + layer * L.layout.tiled_layer_stride);
}

NVNtextureFlags nvnTextureGetFlags(const NVNtexture* texture) {
    Texture* t = tex_of(texture);
    return static_cast<NVNtextureFlags>(t ? t->b.flags : 0);
}

NVNtextureTarget nvnTextureGetTarget(const NVNtexture* texture) {
    Texture* t = tex_of(texture);
    return static_cast<NVNtextureTarget>(t ? t->b.target : 0);
}

int nvnTextureGetWidth(const NVNtexture* texture) {
    Texture* t = tex_of(texture);
    return t ? t->b.width : 0;
}

int nvnTextureGetHeight(const NVNtexture* texture) {
    Texture* t = tex_of(texture);
    return t ? t->b.height : 0;
}

int nvnTextureGetDepth(const NVNtexture* texture) {
    Texture* t = tex_of(texture);
    return t ? t->b.depth : 0;
}

int nvnTextureGetLevels(const NVNtexture* texture) {
    Texture* t = tex_of(texture);
    return t ? static_cast<int>(t->levels) : 0;
}

NVNformat nvnTextureGetFormat(const NVNtexture* texture) {
    Texture* t = tex_of(texture);
    return static_cast<NVNformat>(t ? t->b.format : 0);
}

int nvnTextureGetSamples(const NVNtexture* texture) {
    Texture* t = tex_of(texture);
    return t ? t->b.samples : 0;
}

void nvnTextureGetSwizzle(const NVNtexture* texture, NVNtextureSwizzle* r, NVNtextureSwizzle* g, NVNtextureSwizzle* bl,
                          NVNtextureSwizzle* a) {
    Texture* t = tex_of(texture);
    const int s[4] = {t ? t->b.swizzle[0] : 0, t ? t->b.swizzle[1] : 0, t ? t->b.swizzle[2] : 0, t ? t->b.swizzle[3] : 0};
    if (r) *r = static_cast<NVNtextureSwizzle>(s[0]);
    if (g) *g = static_cast<NVNtextureSwizzle>(s[1]);
    if (bl) *bl = static_cast<NVNtextureSwizzle>(s[2]);
    if (a) *a = static_cast<NVNtextureSwizzle>(s[3]);
}

NVNtextureDepthStencilMode nvnTextureGetDepthStencilMode(const NVNtexture* texture) {
    Texture* t = tex_of(texture);
    return static_cast<NVNtextureDepthStencilMode>(t ? t->b.depth_stencil_mode : 0);
}

ptrdiff_t nvnTextureGetStride(const NVNtexture* texture) {
    Texture* t = tex_of(texture);
    return t ? t->b.stride : 0;
}

NVNtextureAddress nvnTextureGetTextureAddress(const NVNtexture* texture) {
    Texture* t = tex_of(texture);
    if (!t) return 0;
    const MemoryPool* pool = be::host_of<MemoryPool>(t->b.pool);
    return pool ? pool->gpu_base + static_cast<uint64_t>(t->b.offset) : 0;
}

void fill_sparse_layout(const TextureBuilderInfo& b, NVNtextureSparseTileLayout* layout) {
    if (!layout) return;
    std::memset(layout, 0, sizeof *layout);
    int shape[3];
    if (!be::sparse_tile_shape(b.target, format_info(b.format), shape)) return;
    layout->tileWidth = shape[0];
    layout->tileHeight = shape[1];
    layout->tileDepth = shape[2];
    // Word 0: how many levels, from the base, are whole sparse tiles (NVN numTiledLevels); the smaller levels form the mip tail.
    // Games size per-level tile tables by it (Mario Kart World's virtual textures index level 0's entry unconditionally).
    const bool is3d = b.target == NVN_TEXTURE_TARGET_3D;
    int tiled = 0;
    for (int l = 0; l < (b.levels > 0 ? b.levels : 1); ++l) {
        const int w = b.width >> l, h = b.height >> l, d = is3d ? (b.depth >> l) : 1;
        if (w < shape[0] || h < shape[1] || (is3d && d < shape[2])) break;
        ++tiled;
    }
    layout->numTiledLevels = tiled;
}

void nvnTextureGetSparseTileLayout(const NVNtexture* texture, NVNtextureSparseTileLayout* layout) {
    const Texture* t = tex_of(texture);
    if (!t) {
        if (layout) std::memset(layout, 0, sizeof *layout);
        return;
    }
    fill_sparse_layout(t->b, layout);
}

void nvnTextureWriteTexels(const NVNtexture* texture, const NVNtextureView* view, const NVNcopyRegion* region, const void* data) {
    if (!region || !data) return;
    const Target r = resolve(texture, view);
    if (!r.ok || !region_ok(r, *region)) return;
    int64_t row = 0, image = 0;
    tight_strides(r, *region, row, image);
    write_texels(r, *region, static_cast<const uint8_t*>(data), row, image);
}

void nvnTextureWriteTexelsStrided(const NVNtexture* texture, const NVNtextureView* view, const NVNcopyRegion* region, const void* data,
                                  ptrdiff_t row_stride, ptrdiff_t image_stride) {
    if (!region || !data) return;
    const Target r = resolve(texture, view);
    if (!r.ok || !region_ok(r, *region)) return;
    write_texels(r, *region, static_cast<const uint8_t*>(data), row_stride, image_stride);
}

void nvnTextureReadTexels(const NVNtexture* texture, const NVNtextureView* view, const NVNcopyRegion* region, void* out) {
    if (!region || !out) return;
    const Target r = resolve(texture, view);
    if (!r.ok || !region_ok(r, *region)) return;
    int64_t row = 0, image = 0;
    tight_strides(r, *region, row, image);
    read_texels(r, *region, static_cast<uint8_t*>(out), row, image);
}

void nvnTextureReadTexelsStrided(const NVNtexture* texture, const NVNtextureView* view, const NVNcopyRegion* region, void* out,
                                 ptrdiff_t row_stride, ptrdiff_t image_stride) {
    if (!region || !out) return;
    const Target r = resolve(texture, view);
    if (!r.ok || !region_ok(r, *region)) return;
    read_texels(r, *region, static_cast<uint8_t*>(out), row_stride, image_stride);
}

// Flush: the CPU wrote the storage, so the GPU image is uploaded again before its next use (the whole texture; the region is
// not narrowed). Invalidate: the GPU may have written the storage, so it is read back into the storage now.
void nvnTextureFlushTexels(const NVNtexture* texture, const NVNtextureView* view, const NVNcopyRegion* region) {
    (void)view;
    (void)region;
    Texture* t = tex_of(texture);
    if (!t || !t->mem) return;
    be::tev(t, "flush_texels");
    be::tx_mark("tx.flushtx", t);
    t->cpu_dirty.store(true);
    gpu::mark_texture_cpu_dirty(t);
}

void nvnTextureInvalidateTexels(const NVNtexture* texture, const NVNtextureView* view, const NVNcopyRegion* region) {
    (void)view;
    (void)region;
    Texture* t = tex_of(texture);
    if (!t || !t->mem) return;
    gpu::download_texture(t);
}

const NVNmemoryPool* nvnTextureGetMemoryPool(const NVNtexture* texture) {
    Texture* t = tex_of(texture);
    return t ? t->b.pool : nullptr;
}

ptrdiff_t nvnTextureGetMemoryOffset(const NVNtexture* texture) {
    Texture* t = tex_of(texture);
    return t ? t->b.offset : 0;
}

int nvnTextureGetStorageSize(const NVNtexture* texture) {
    Texture* t = tex_of(texture);
    return t ? static_cast<int>(t->storage_size) : 0;
}

// Same host object, or the same storage (address and size) with the same description and channel mapping. Layout libraries
// (agl) rebuild a capture texture on the same storage with a new swizzle and replace its registered descriptor only when this
// reports a difference; ignoring the swizzle kept the old mapping and drew those captures black (found by seedds/nxnative,
// docs/reference/rendering.md).
NVNboolean nvnTextureCompare(const NVNtexture* a, const NVNtexture* b) {
    Texture* x = tex_of(a);
    Texture* y = tex_of(b);
    if (!x || !y) return a == b ? 1 : 0;
    if (x == y) return 1;
    if (x->overflow || y->overflow) {   // host-backed: the same storage is the same pool and offset (offset -1 names none)
        if (!x->overflow || !y->overflow || x->overflow_offset < 0 || x->overflow_pool != y->overflow_pool || x->overflow_offset != y->overflow_offset ||
            x->storage_size != y->storage_size)
            return 0;
    } else if (!x->mem || x->mem != y->mem || x->storage_size != y->storage_size) {
        return 0;
    }
    const TextureBuilderInfo& p = x->b;
    const TextureBuilderInfo& q = y->b;
    return (p.format == q.format && p.target == q.target && p.width == q.width && p.height == q.height && p.depth == q.depth &&
            p.levels == q.levels && p.samples == q.samples && p.depth_stencil_mode == q.depth_stencil_mode &&
            std::equal(p.swizzle, p.swizzle + 4, q.swizzle)) ? 1 : 0;
}

uint64_t nvnTextureGetDebugID(const NVNtexture* texture) {
    Texture* t = tex_of(texture);
    return t ? t->id : 0;
}

// ---- builder queries that need the layout (the rest of the builder is in api_texture_builder.cpp) -----------------------
size_t nvnTextureBuilderGetStorageSize(const NVNtextureBuilder* builder) {
    if (!builder) return 0;
    const TexLayout L = be::compute_texture_layout(decode(builder));
    return L.valid ? static_cast<size_t>(L.storage_size) : 0;
}

size_t nvnTextureBuilderGetStorageAlignment(const NVNtextureBuilder* builder) {
    if (!builder) return 0;
    const TexLayout L = be::compute_texture_layout(decode(builder));
    return L.valid ? static_cast<size_t>(L.alignment) : 0;
}

NVNstorageClass nvnTextureBuilderGetStorageClass(const NVNtextureBuilder* builder) {
    (void)builder;
    return 0;
}

void nvnTextureBuilderGetSparseTileLayout(const NVNtextureBuilder* builder, NVNtextureSparseTileLayout* layout) {
    if (!builder) {
        if (layout) std::memset(layout, 0, sizeof *layout);
        return;
    }
    fill_sparse_layout(decode(builder), layout);
}

size_t nvnTextureBuilderGetZCullStorageSize(const NVNtextureBuilder* builder) {
    (void)builder;
    return 0;
}

}  // namespace

void register_api_texture() {
    NVN_REG(nvnTextureInitialize);
    NVN_REG(nvnTextureFinalize);
    NVN_REG(nvnTextureSetDebugLabel);
    NVN_REG(nvnTextureGetZCullStorageSize);
    NVN_REG(nvnTextureGetStorageClass);
    NVN_REG(nvnTextureGetRawStorageClass);
    NVN_REG(nvnTextureGetViewOffset);
    NVN_REG(nvnTextureGetFlags);
    NVN_REG(nvnTextureGetTarget);
    NVN_REG(nvnTextureGetWidth);
    NVN_REG(nvnTextureGetHeight);
    NVN_REG(nvnTextureGetDepth);
    NVN_REG(nvnTextureGetLevels);
    NVN_REG(nvnTextureGetFormat);
    NVN_REG(nvnTextureGetSamples);
    NVN_REG(nvnTextureGetSwizzle);
    NVN_REG(nvnTextureGetDepthStencilMode);
    NVN_REG(nvnTextureGetStride);
    NVN_REG(nvnTextureGetTextureAddress);
    NVN_REG(nvnTextureGetSparseTileLayout);
    NVN_REG(nvnTextureWriteTexels);
    NVN_REG(nvnTextureWriteTexelsStrided);
    NVN_REG(nvnTextureReadTexels);
    NVN_REG(nvnTextureReadTexelsStrided);
    NVN_REG(nvnTextureFlushTexels);
    NVN_REG(nvnTextureInvalidateTexels);
    NVN_REG(nvnTextureGetMemoryPool);
    NVN_REG(nvnTextureGetMemoryOffset);
    NVN_REG(nvnTextureGetStorageSize);
    NVN_REG(nvnTextureCompare);
    NVN_REG(nvnTextureGetDebugID);
    NVN_REG(nvnTextureBuilderGetStorageSize);
    NVN_REG(nvnTextureBuilderGetStorageAlignment);
    NVN_REG(nvnTextureBuilderGetStorageClass);
    NVN_REG(nvnTextureBuilderGetSparseTileLayout);
    NVN_REG(nvnTextureBuilderGetZCullStorageSize);
}

}  // namespace nxrt::nvn
