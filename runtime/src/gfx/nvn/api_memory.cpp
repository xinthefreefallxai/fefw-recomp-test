// SPDX-License-Identifier: GPL-3.0-or-later
// NVNmemoryPool, NVNbuffer, NVNtexturePool and NVNsamplerPool (family memory).
//
// Host objects: a memory pool, a texture pool and a sampler pool are host objects (nvn_core.h) behind the guest object's
// GuestHeader. A pool's storage is the game's own memory: gpu_base is the storage address, so a GPU address is a host
// address. A virtual pool (NVN_MEMORY_POOL_FLAGS_VIRTUAL) has no storage: it gets a synthetic GPU range that nothing maps.
// A buffer is plain data: BufferData (address, size, guest pool pointer, magic) lives in the NVNbuffer, so the game may copy
// it. A texture or sampler pool keeps the guest pointer of its memory pool at byte 16 of its guest object (right after the
// header, inside the 0x20 bytes of the NVN struct), which is what nvnTexturePoolGetMemoryPool / nvnSamplerPoolGetMemoryPool
// return. The texture and sampler pools hold a reference on their host memory pool until they are finalized.
// Flush and invalidate behaviour checked against smo-recomp runtime/gfx/nvn_gl.c (GPL-3.0): a flush marks the textures in
// the range as CPU-written, an invalidate does nothing, a virtual mapping is not supported. No code copied.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include "nvn_cmd.h"
#include "nvn_core.h"
#include "nvn_gpu.h"
#include "nvn_impl.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/nvn_backend.h"

namespace nxrt::nvn {
using namespace be;
namespace {

constexpr const char* kTag = "nvn";
constexpr uint64_t kBufferMagic = 0x4e58425546ull;      // "NXBUF"
constexpr uint64_t kVirtualBase = 0x0000200000000000ull; // synthetic GPU range for virtual pools
constexpr uint64_t kVirtualAlign = 64ull << 10;         // 64 KiB: the range length and its start
constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kMaxPoolSize = 1ull << 40;           // sanity bound (keeps the synthetic range from overflowing)

static_assert(sizeof(GuestHeader) + sizeof(uint64_t) <= sizeof(NVNtexturePool), "texture pool guest slot fits");
static_assert(sizeof(GuestHeader) + sizeof(uint64_t) <= sizeof(NVNsamplerPool), "sampler pool guest slot fits");
static_assert(sizeof(BufferData) <= sizeof(NVNbuffer), "buffer layout fits NVNbuffer");

// Live object counts (nxrt/gauge.h). An object counts from its Initialize until its Finalize, or until an Initialize over it replaces it.
nxrt::GaugeCounter g_memory_pools("nvn.memory_pools");
nxrt::GaugeCounter g_memory_pool_bytes("nvn.memory_pools.bytes");   // size of the live pools (a virtual pool counts its reserved range, not committed memory)
nxrt::GaugeCounter g_buffers("nvn.buffers");
nxrt::GaugeCounter g_texture_pools("nvn.texture_pools");
nxrt::GaugeCounter g_sampler_pools("nvn.sampler_pools");
nxrt::GaugeCounter g_texture_handles("nvn.texture_handles");   // texture ids registered in live texture pools
nxrt::GaugeCounter g_sampler_handles("nvn.sampler_handles");   // sampler ids registered in live sampler pools

// Logs a message the first time this call site hits it.
#define NVN_WARN_ONCE(...)                                                  \
    do {                                                                    \
        static std::atomic<bool> warned{false};                             \
        if (!warned.exchange(true)) NXLOG_W(kTag, __VA_ARGS__);             \
    } while (0)

// ---- helpers -----------------------------------------------------------------------------------------------------------------
// The part of [offset, offset + size) that lies inside a range of `total` bytes: its length (0 when none) and its start.
uint64_t clip_range(ptrdiff_t offset, uint64_t size, uint64_t total, uint64_t& start) {
    if (offset < 0) return 0;
    const uint64_t off = static_cast<uint64_t>(offset);
    if (off >= total) return 0;
    start = off;
    return std::min<uint64_t>(size, total - off);
}

// Synthetic GPU range for a virtual pool: 64 KiB aligned start, length rounded up to 64 KiB.
uint64_t alloc_virtual_range(uint64_t size) {
    static std::atomic<uint64_t> next{kVirtualBase};
    const uint64_t len = (size + kVirtualAlign - 1) & ~(kVirtualAlign - 1);
    return next.fetch_add(len);
}

// Takes a pool out of the registry and marks it dead (Finalize, and re-initialisation over a live pool).
void retire_pool(MemoryPool* p) {
    g_memory_pools.sub(1);
    g_memory_pool_bytes.sub(static_cast<int64_t>(p->size));
    handles_release_pool(p);
    pool_unregister(p);
    p->dead.store(true);
    lifetime_pool_retired(p);
}

// The guest pointer of the memory pool a texture or sampler pool was initialised with (byte 16 of the guest object).
uint64_t owner_slot_get(const void* guest) {
    uint64_t v = 0;
    std::memcpy(&v, static_cast<const uint8_t*>(guest) + sizeof(GuestHeader), sizeof v);
    return v;
}
void owner_slot_set(void* guest, uint64_t v) {
    std::memcpy(static_cast<uint8_t*>(guest) + sizeof(GuestHeader), &v, sizeof v);
}

// A texture or sampler pool gives back its reference on the memory pool (Finalize, re-initialisation).
template <class P>
void drop_pool_ref(P* p) {
    if (p->mem) {
        p->mem->unref();
        p->mem = nullptr;
    }
}

// A texture or sampler pool leaves the live set (Finalize, or Initialize over it): the handles it registered leave with it.
void texture_pool_gone(const TexturePool* tp) {
    int64_t used = 0;
    for (const TexturePool::Entry& e : tp->entries) used += e.used ? 1 : 0;
    g_texture_handles.sub(used);
    g_texture_pools.sub(1);
}
void sampler_pool_gone(const SamplerPool* sp) {
    int64_t used = 0;
    for (uint8_t u : sp->used) used += u ? 1 : 0;
    g_sampler_handles.sub(used);
    g_sampler_pools.sub(1);
}

// The memory pool behind a guest NVNmemoryPool (null when the object is not a live pool of ours).
MemoryPool* pool_of_guest(const void* guest) { return host_of<MemoryPool>(guest); }

// ---- buffers (plain data in the guest NVNbuffer) -------------------------------------------------------------------------
bool buffer_load(const void* guest, BufferData& d) {
    if (!guest) return false;
    std::memcpy(&d, guest, sizeof d);
    return d.magic == kBufferMagic;
}
void buffer_store(void* guest, const BufferData& d) { std::memcpy(guest, &d, sizeof d); }

MemoryPool* buffer_pool(const BufferData& d) {
    return host_of<MemoryPool>(reinterpret_cast<const void*>(static_cast<uintptr_t>(d.pool)));
}

// Host address of byte 0 of a buffer; null for a buffer in a virtual pool or whose pool has been finalized.
uint8_t* buffer_host(const BufferData& d) {
    const MemoryPool* p = buffer_pool(d);
    if (!p || p->is_virtual || !p->base || d.address < p->gpu_base) return nullptr;
    return p->base + (d.address - p->gpu_base);
}

// ---- memory pools ---------------------------------------------------------------------------------------------------------------
NVNboolean nvnMemoryPoolInitialize(NVNmemoryPool* pool, const NVNmemoryPoolBuilder* builder) {
    if (!pool || !builder) return 0;
    const MemoryPoolBuilderInfo info = decode(builder);
    const bool is_virtual = (info.flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) != 0;
    if (info.size == 0 || info.size > kMaxPoolSize) {
        NXLOG_W(kTag, "nvnMemoryPoolInitialize: size %llu is not usable, pool not created",
                static_cast<unsigned long long>(info.size));
        return 0;
    }
    if (!is_virtual && !info.storage) {
        NXLOG_W(kTag, "nvnMemoryPoolInitialize: a non-virtual pool has no storage, pool not created");
        return 0;
    }
    const bool misaligned = (info.size & (kPageSize - 1)) != 0 ||
                            (info.storage && (reinterpret_cast<uintptr_t>(info.storage) & (kPageSize - 1)) != 0);
    if (misaligned) NVN_WARN_ONCE("nvnMemoryPoolInitialize: storage or size is not 4096 aligned; accepted");

    if (MemoryPool* old = pool_of_guest(pool)) retire_pool(old);
    auto* p = new MemoryPool();
    p->size = info.size;
    p->flags = info.flags;
    p->is_shader_code = (info.flags & NVN_MEMORY_POOL_FLAGS_SHADER_CODE) != 0;
    p->is_virtual = is_virtual;
    if (is_virtual) {
        p->base = nullptr;
        p->gpu_base = alloc_virtual_range(info.size);
    } else {
        p->base = static_cast<uint8_t*>(info.storage);
        p->gpu_base = reinterpret_cast<uint64_t>(info.storage);
    }
    pool_register(p);
    bind_guest(pool, p);
    g_memory_pools.add(1);
    g_memory_pool_bytes.add(static_cast<int64_t>(p->size));
    return 1;
}

void nvnMemoryPoolSetDebugLabel(NVNmemoryPool*, const char*) {}

void nvnMemoryPoolFinalize(NVNmemoryPool* pool) {
    MemoryPool* p = pool_of_guest(pool);
    if (!p) return;
    retire_pool(p);
    unbind_guest(pool);
}

void* nvnMemoryPoolMap(const NVNmemoryPool* pool) {
    const MemoryPool* p = pool_of_guest(pool);
    return p ? p->base : nullptr;
}

void nvnMemoryPoolFlushMappedRange(const NVNmemoryPool* pool, ptrdiff_t offset, size_t size) {
    const MemoryPool* p = pool_of_guest(pool);
    if (!p || !p->base) return;
    uint64_t start = 0;
    const uint64_t n = clip_range(offset, size, p->size, start);
    if (n) textures_cpu_wrote(p->base + start, n);
}

void nvnMemoryPoolInvalidateMappedRange(const NVNmemoryPool* pool, ptrdiff_t offset, size_t size) {
    const MemoryPool* p = pool_of_guest(pool);
    if (!p || !p->base) return;
    uint64_t start = 0;
    const uint64_t n = clip_range(offset, size, p->size, start);
    if (n) gpu::cpu_will_read(reinterpret_cast<uint64_t>(p->base) + start, n);   // compute results held in video memory come back first
}

NVNbufferAddress nvnMemoryPoolGetBufferAddress(const NVNmemoryPool* pool) {
    const MemoryPool* p = pool_of_guest(pool);
    return p ? p->gpu_base : 0;
}

// A request of nvnMemoryPoolMapVirtual: 40 bytes in the game's layout (MapVirtualTiles builds them): the physical pool, its offset,
// the virtual offset, the size and the storage class.
struct MappingRequest {
    uint64_t physical_pool;
    int64_t physical_offset, virtual_offset;
    uint64_t size;
    int32_t storage_class, pad;
};
static_assert(sizeof(MappingRequest) == 40, "NVNmappingRequest is 40 bytes");

// Sparse (virtual) textures are backed by ordinary images that exist completely (the console fills tiles through copy commands, which
// the images take as they are); the mapping of tiles to physical memory therefore needs no action. The call succeeds.
NVNboolean nvnMemoryPoolMapVirtual(NVNmemoryPool* pool, int count, const NVNmappingRequest* requests) {
    MemoryPool* vp = pool_of_guest(pool);
    if (!vp || count < 0 || (count > 0 && !requests)) return 0;
    static std::atomic<int> logged{0};
    const MappingRequest* r = reinterpret_cast<const MappingRequest*>(requests);
    for (int i = 0; i < count; ++i) {
        if (r[i].virtual_offset < 0 || r[i].size == 0) continue;
        const uint64_t v0 = static_cast<uint64_t>(r[i].virtual_offset), v1 = v0 + r[i].size;
        MemoryPool* phys = host_of<MemoryPool>(reinterpret_cast<const void*>(static_cast<uintptr_t>(r[i].physical_pool)));
        if (logged.load() < 8) {
            logged.fetch_add(1);
            NXLOG_I("nvn", "nvnMemoryPoolMapVirtual: virtual 0x%llx + 0x%llx <- physical pool %p + 0x%llx (storage class %d)%s", static_cast<unsigned long long>(v0),
                    static_cast<unsigned long long>(r[i].size), reinterpret_cast<void*>(static_cast<uintptr_t>(r[i].physical_pool)),
                    static_cast<unsigned long long>(r[i].physical_offset), r[i].storage_class, phys ? "" : " (unmaps the range)");
        }
        {
            std::lock_guard<std::mutex> lk(vp->vmu);
            // the new mapping replaces whatever covered its range
            std::vector<MemoryPool::VirtualMap> kept;
            for (MemoryPool::VirtualMap& m : vp->vmaps) {
                const uint64_t m0 = m.voff, m1 = m.voff + m.size;
                if (m1 <= v0 || m0 >= v1) { kept.push_back(std::move(m)); continue; }
                if (m0 < v0) { MemoryPool::VirtualMap left = m; left.size = v0 - m0; kept.push_back(std::move(left)); }
                if (m1 > v1) { MemoryPool::VirtualMap right = m; right.poff += static_cast<int64_t>(v1 - m0); right.voff = v1; right.size = m1 - v1; kept.push_back(std::move(right)); }
            }
            if (phys) {
                MemoryPool::VirtualMap nm;
                nm.voff = v0;
                nm.size = r[i].size;
                nm.phys = Ref<MemoryPool>::share(phys);
                nm.poff = r[i].physical_offset;
                kept.push_back(std::move(nm));
            }
            vp->vmaps = std::move(kept);
        }
        virtual_textures_remapped(vp, v0, r[i].size);
    }
    return 1;
}

size_t nvnMemoryPoolGetSize(const NVNmemoryPool* pool) {
    const MemoryPool* p = pool_of_guest(pool);
    return p ? static_cast<size_t>(p->size) : 0;
}

NVNmemoryPoolFlags nvnMemoryPoolGetFlags(const NVNmemoryPool* pool) {
    const MemoryPool* p = pool_of_guest(pool);
    return static_cast<NVNmemoryPoolFlags>(p ? p->flags : 0);
}

// ---- buffers ------------------------------------------------------------------------------------------------------------------
NVNboolean nvnBufferInitialize(NVNbuffer* buffer, const NVNbufferBuilder* builder) {
    if (!buffer || !builder) return 0;
    const BufferBuilderInfo info = decode(builder);
    MemoryPool* pool = pool_of_guest(info.pool);
    if (!pool || info.offset < 0) return 0;
    const uint64_t off = static_cast<uint64_t>(info.offset);
    if (off > pool->size || info.size > pool->size - off) {
        NXLOG_W(kTag, "nvnBufferInitialize: %llu+%llu bytes do not fit in a pool of %llu, buffer not created",
                static_cast<unsigned long long>(off), static_cast<unsigned long long>(info.size),
                static_cast<unsigned long long>(pool->size));
        return 0;
    }
    BufferData prev{};
    const bool was_live = buffer_load(buffer, prev);     // initialised over a live buffer: replaced, not added
    BufferData d{};
    d.address = pool->gpu_base + off;
    d.size = info.size;
    d.pool = reinterpret_cast<uint64_t>(info.pool);
    d.magic = kBufferMagic;
    buffer_store(buffer, d);
    if (!was_live) g_buffers.add(1);
    if (gpu::active()) gpu::prefetch_pool(pool);   // a pool that backs a buffer is going to be read by the GPU: import it in the background
    return 1;
}

void nvnBufferSetDebugLabel(NVNbuffer*, const char*) {}

void nvnBufferFinalize(NVNbuffer* buffer) {
    BufferData d{};
    if (!buffer_load(buffer, d)) return;
    g_buffers.sub(1);
    std::memset(buffer, 0, sizeof(NVNbuffer));
}

void* nvnBufferMap(const NVNbuffer* buffer) {
    BufferData d{};
    if (!buffer_load(buffer, d)) return nullptr;
    return buffer_host(d);
}

NVNbufferAddress nvnBufferGetAddress(const NVNbuffer* buffer) {
    BufferData d{};
    return buffer_load(buffer, d) ? d.address : 0;
}

void nvnBufferFlushMappedRange(const NVNbuffer* buffer, ptrdiff_t offset, size_t size) {
    BufferData d{};
    if (!buffer_load(buffer, d)) return;
    uint8_t* map = buffer_host(d);
    if (!map) return;
    uint64_t start = 0;
    const uint64_t n = clip_range(offset, size, d.size, start);
    if (n) textures_cpu_wrote(map + start, n);
}

void nvnBufferInvalidateMappedRange(const NVNbuffer* buffer, ptrdiff_t offset, size_t size) {
    BufferData d{};
    if (!buffer_load(buffer, d)) return;
    uint8_t* map = buffer_host(d);
    if (!map) return;
    uint64_t start = 0;
    const uint64_t n = clip_range(offset, size, d.size, start);
    if (n) gpu::cpu_will_read(reinterpret_cast<uint64_t>(map) + start, n);
}

NVNmemoryPool* nvnBufferGetMemoryPool(const NVNbuffer* buffer) {
    BufferData d{};
    if (!buffer_load(buffer, d)) return nullptr;
    return reinterpret_cast<NVNmemoryPool*>(static_cast<uintptr_t>(d.pool));
}

ptrdiff_t nvnBufferGetMemoryOffset(const NVNbuffer* buffer) {
    BufferData d{};
    if (!buffer_load(buffer, d)) return 0;
    const MemoryPool* p = buffer_pool(d);
    return p && d.address >= p->gpu_base ? static_cast<ptrdiff_t>(d.address - p->gpu_base) : 0;
}

size_t nvnBufferGetSize(const NVNbuffer* buffer) {
    BufferData d{};
    return buffer_load(buffer, d) ? static_cast<size_t>(d.size) : 0;
}

uint64_t nvnBufferGetDebugID(const NVNbuffer* buffer) {
    BufferData d{};
    return buffer_load(buffer, d) ? (d.address & 0xffffffffull) : 0;
}

// ---- texture pools ---------------------------------------------------------------------------------------------------------
NVNboolean nvnTexturePoolInitialize(NVNtexturePool* pool, const NVNmemoryPool* memory, ptrdiff_t offset, int count) {
    if (!pool) return 0;
    MemoryPool* mem = pool_of_guest(memory);
    if (!mem || count < 0) {
        NVN_WARN_ONCE("nvnTexturePoolInitialize: the memory pool is not a live pool or the count is negative; pool not created");
        return 0;
    }
    if (TexturePool* old = host_of<TexturePool>(pool)) {
        texture_pool_gone(old);
        drop_pool_ref(old);
    }
    auto* tp = new TexturePool();
    tp->mem = mem;
    mem->ref();                                 // released by Finalize (or re-initialisation)
    tp->offset = offset;
    tp->count = count;
    tp->entries.resize(static_cast<size_t>(count));
    owner_slot_set(pool, reinterpret_cast<uint64_t>(memory));
    bind_guest(pool, tp);
    g_texture_pools.add(1);
    return 1;
}

void nvnTexturePoolSetDebugLabel(NVNtexturePool*, const char*) {}

void nvnTexturePoolFinalize(NVNtexturePool* pool) {
    TexturePool* tp = host_of<TexturePool>(pool);
    if (!tp) return;
    texture_pool_gone(tp);
    drop_pool_ref(tp);
    unbind_guest(pool);
}

// Registers a texture (or image) under id. Shared by RegisterTexture and RegisterImage, which the reference does the same way.
void texture_pool_register(const NVNtexturePool* pool, int id, const NVNtexture* texture, const NVNtextureView* view) {
    TexturePool* tp = host_of<TexturePool>(pool);
    if (!tp) return;
    if (id < 0 || id >= tp->count) {
        NVN_WARN_ONCE("nvnTexturePoolRegister: a texture id outside the pool is ignored");
        return;
    }
    Texture* t = host_of<Texture>(texture);
    const TextureViewInfo info = view ? decode(view) : TextureViewInfo{};
    std::unique_lock<std::shared_mutex> lk(tp->lock);
    TexturePool::Entry& e = tp->entries[static_cast<size_t>(id)];
    e.tex = Ref<Texture>::share(t);
    e.view = info;
    e.has_view = view != nullptr;
    if (!e.used) g_texture_handles.add(1);
    e.used = true;
    if (t) t->registered_ms.store(texture_clock_ms(), std::memory_order_relaxed);
    pool_note_change(*tp, id);
    be::tx_mark("tx.reg", t);
    be::g_tex_scan_epoch.fetch_add(1, std::memory_order_relaxed);   // a new texture sits behind this id now
    if (t && gpu::active()) gpu::texture_registered(t);   // registered: it is going to be sampled (background preparation, nvn_texprep.h)
}

void nvnTexturePoolRegisterTexture(const NVNtexturePool* pool, int id, const NVNtexture* texture, const NVNtextureView* view) {
    texture_pool_register(pool, id, texture, view);
}

void nvnTexturePoolRegisterImage(const NVNtexturePool* pool, int id, const NVNtexture* texture, const NVNtextureView* view) {
    texture_pool_register(pool, id, texture, view);
}

const NVNmemoryPool* nvnTexturePoolGetMemoryPool(const NVNtexturePool* pool) {
    if (!host_of<TexturePool>(pool)) return nullptr;
    return reinterpret_cast<const NVNmemoryPool*>(static_cast<uintptr_t>(owner_slot_get(pool)));
}

ptrdiff_t nvnTexturePoolGetMemoryOffset(const NVNtexturePool* pool) {
    const TexturePool* tp = host_of<TexturePool>(pool);
    return tp ? static_cast<ptrdiff_t>(tp->offset) : 0;
}

int nvnTexturePoolGetSize(const NVNtexturePool* pool) {
    const TexturePool* tp = host_of<TexturePool>(pool);
    return tp ? tp->count : 0;
}

// ---- sampler pools ----------------------------------------------------------------------------------------------------------
NVNboolean nvnSamplerPoolInitialize(NVNsamplerPool* pool, const NVNmemoryPool* memory, ptrdiff_t offset, int count) {
    if (!pool) return 0;
    MemoryPool* mem = pool_of_guest(memory);
    if (!mem || count < 0) {
        NVN_WARN_ONCE("nvnSamplerPoolInitialize: the memory pool is not a live pool or the count is negative; pool not created");
        return 0;
    }
    if (SamplerPool* old = host_of<SamplerPool>(pool)) {
        sampler_pool_gone(old);
        drop_pool_ref(old);
    }
    auto* sp = new SamplerPool();
    sp->mem = mem;
    mem->ref();                                 // released by Finalize (or re-initialisation)
    sp->offset = offset;
    sp->count = count;
    sp->entries.resize(static_cast<size_t>(count));
    sp->used.resize(static_cast<size_t>(count), 0);
    owner_slot_set(pool, reinterpret_cast<uint64_t>(memory));
    bind_guest(pool, sp);
    g_sampler_pools.add(1);
    return 1;
}

void nvnSamplerPoolSetDebugLabel(NVNsamplerPool*, const char*) {}

void nvnSamplerPoolFinalize(NVNsamplerPool* pool) {
    SamplerPool* sp = host_of<SamplerPool>(pool);
    if (!sp) return;
    sampler_pool_gone(sp);
    drop_pool_ref(sp);
    unbind_guest(pool);
}

// Stores a decoded sampler under id. The builder and sampler forms share this: both decode to a SamplerInfo.
void sampler_pool_store(const NVNsamplerPool* pool, int id, const SamplerInfo& info) {
    SamplerPool* sp = host_of<SamplerPool>(pool);
    if (!sp) return;
    if (id < 0 || id >= sp->count) {
        NVN_WARN_ONCE("nvnSamplerPoolRegister: a sampler id outside the pool is ignored");
        return;
    }
    std::unique_lock<std::shared_mutex> lk(sp->lock);
    sp->entries[static_cast<size_t>(id)] = info;
    if (!sp->used[static_cast<size_t>(id)]) g_sampler_handles.add(1);
    sp->used[static_cast<size_t>(id)] = 1;
    pool_note_change(*sp, id);
}

void nvnSamplerPoolRegisterSampler(const NVNsamplerPool* pool, int id, const NVNsampler* sampler) {
    sampler_pool_store(pool, id, decode(sampler));
}

void nvnSamplerPoolRegisterSamplerBuilder(const NVNsamplerPool* pool, int id, const NVNsamplerBuilder* builder) {
    sampler_pool_store(pool, id, decode(builder));
}

const NVNmemoryPool* nvnSamplerPoolGetMemoryPool(const NVNsamplerPool* pool) {
    if (!host_of<SamplerPool>(pool)) return nullptr;
    return reinterpret_cast<const NVNmemoryPool*>(static_cast<uintptr_t>(owner_slot_get(pool)));
}

ptrdiff_t nvnSamplerPoolGetMemoryOffset(const NVNsamplerPool* pool) {
    const SamplerPool* sp = host_of<SamplerPool>(pool);
    return sp ? static_cast<ptrdiff_t>(sp->offset) : 0;
}

int nvnSamplerPoolGetSize(const NVNsamplerPool* pool) {
    const SamplerPool* sp = host_of<SamplerPool>(pool);
    return sp ? sp->count : 0;
}

}  // namespace

void register_api_memory() {
    // MemoryPool
    NVN_REG(nvnMemoryPoolInitialize);
    NVN_REG(nvnMemoryPoolSetDebugLabel);
    NVN_REG(nvnMemoryPoolFinalize);
    NVN_REG(nvnMemoryPoolMap);
    NVN_REG(nvnMemoryPoolFlushMappedRange);
    NVN_REG(nvnMemoryPoolInvalidateMappedRange);
    NVN_REG(nvnMemoryPoolGetBufferAddress);
    NVN_REG(nvnMemoryPoolMapVirtual);
    NVN_REG(nvnMemoryPoolGetSize);
    NVN_REG(nvnMemoryPoolGetFlags);
    // Buffer
    NVN_REG(nvnBufferInitialize);
    NVN_REG(nvnBufferSetDebugLabel);
    NVN_REG(nvnBufferFinalize);
    NVN_REG(nvnBufferMap);
    NVN_REG(nvnBufferGetAddress);
    NVN_REG(nvnBufferFlushMappedRange);
    NVN_REG(nvnBufferInvalidateMappedRange);
    NVN_REG(nvnBufferGetMemoryPool);
    NVN_REG(nvnBufferGetMemoryOffset);
    NVN_REG(nvnBufferGetSize);
    NVN_REG(nvnBufferGetDebugID);
    // TexturePool
    NVN_REG(nvnTexturePoolInitialize);
    NVN_REG(nvnTexturePoolSetDebugLabel);
    NVN_REG(nvnTexturePoolFinalize);
    NVN_REG(nvnTexturePoolRegisterTexture);
    NVN_REG(nvnTexturePoolRegisterImage);
    NVN_REG(nvnTexturePoolGetMemoryPool);
    NVN_REG(nvnTexturePoolGetMemoryOffset);
    NVN_REG(nvnTexturePoolGetSize);
    // SamplerPool
    NVN_REG(nvnSamplerPoolInitialize);
    NVN_REG(nvnSamplerPoolSetDebugLabel);
    NVN_REG(nvnSamplerPoolFinalize);
    NVN_REG(nvnSamplerPoolRegisterSampler);
    NVN_REG(nvnSamplerPoolRegisterSamplerBuilder);
    NVN_REG(nvnSamplerPoolGetMemoryPool);
    NVN_REG(nvnSamplerPoolGetMemoryOffset);
    NVN_REG(nvnSamplerPoolGetSize);
}

bool memory_pool_info(const void* guest_pool, uint64_t* size, int* flags) {
    const MemoryPool* p = pool_of_guest(guest_pool);
    if (!p || p->dead.load()) return false;
    if (size) *size = p->size;
    if (flags) *flags = p->flags;
    return true;
}

}  // namespace nxrt::nvn
