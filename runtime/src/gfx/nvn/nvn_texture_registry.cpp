// SPDX-License-Identifier: GPL-3.0-or-later
// Registry of initialized textures by storage range (see nvn_core.h).
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/ftrace.h"
#include "nxrt/gauge.h"
#include "nvn_texdata.h"
#include "nxrt/log.h"
#include "nvn_format.h"
#include "nvn_gpu.h"

namespace nxrt::nvn::be {
namespace {
using OverflowKey = std::pair<const MemoryPool*, int64_t>;
std::mutex g_mu;
std::multimap<uint64_t, Texture*> g_by_start;   // storage start -> texture
std::vector<Texture*> g_virtual;                // sparse textures
std::multimap<OverflowKey, Texture*> g_overflow;   // host-backed textures: (pool, offset) -> texture

OverflowKey overflow_key(const Texture* t) { return {t->overflow_pool, t->overflow_offset}; }

// The same shape: the image made for one serves the other.
bool same_shape(const Texture* o, const Texture* t) {
    return o->storage_size == t->storage_size && o->levels == t->levels && o->layers == t->layers && o->fmt == t->fmt && o->block_linear == t->block_linear &&
           o->block_height_gobs == t->block_height_gobs && o->b.target == t->b.target && o->b.width == t->b.width && o->b.height == t->b.height &&
           o->b.depth == t->b.depth && o->b.samples == t->b.samples;
}
}  // namespace

void texture_registry_add(Texture* t) {
    if (!t) return;
    if (t->overflow) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_overflow.emplace(overflow_key(t), t);
        return;
    }
    if (!t->mem) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_by_start.emplace(reinterpret_cast<uint64_t>(t->mem), t);
}

void texture_registry_remove(Texture* t) {
    if (!t) return;
    if (t->overflow) {
        std::lock_guard<std::mutex> lk(g_mu);
        auto r = g_overflow.equal_range(overflow_key(t));
        for (auto it = r.first; it != r.second; ++it)
            if (it->second == t) { g_overflow.erase(it); break; }
        return;
    }
    if (!t->mem) return;
    std::lock_guard<std::mutex> lk(g_mu);
    auto r = g_by_start.equal_range(reinterpret_cast<uint64_t>(t->mem));
    for (auto it = r.first; it != r.second; ++it)
        if (it->second == t) { g_by_start.erase(it); break; }
}

void tx_mark(const char* what, const Texture* t) {
    if (t && ft::on()) ft::mark(what, t->id);
}

void tev(const Texture* t, const char* what, uint64_t a, uint64_t b) {
    static const bool on = nxrt::config_get_int("nvn", "trace_tex_events", 0) != 0;
    if (!on || !t || !t->fmt || t->b.width != 128 || t->layers != 6 || t->levels != 8) return;
    NXLOG_I("nvn.tev", "t%u %s mem %p size %llu: %llx %llx", t->id, what, static_cast<const void*>(t->mem), static_cast<unsigned long long>(t->storage_size),
            static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
}

void texture_registry_add_virtual(Texture* t) {
    if (!t || !t->vpool) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_virtual.push_back(t);
}

void texture_registry_remove_virtual(Texture* t) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_virtual.erase(std::remove(g_virtual.begin(), g_virtual.end(), t), g_virtual.end());
}

namespace {
void mark_dirty_list(std::vector<Ref<Texture>>& hit, uint64_t a, uint64_t b) {
    for (auto& t : hit) {
        if (t->vpool) {   // a and b are virtual pool offsets: the part of the texture's storage they cover
            const uint64_t lo = std::max(a, t->voffset), hi = std::min(b, t->voffset + t->storage_size);
            if (lo < hi) {
                std::lock_guard<std::mutex> sl(t->sparse_mu);
                t->sparse_dirty.emplace_back(lo - t->voffset, hi - t->voffset);
            }
        }
        tev(t.get(), "cpu_wrote", a, b - a);
        t->cpu_dirty.store(true);
        gpu::mark_texture_cpu_dirty(t.get());
    }
}
}  // namespace

void virtual_textures_remapped(const MemoryPool* vpool, uint64_t voff, uint64_t size) {
    std::vector<Ref<Texture>> hit;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (Texture* t : g_virtual)
            if (t->vpool.get() == vpool && t->voffset < voff + size && t->voffset + t->storage_size > voff) { t->ref(); hit.emplace_back(t); }
    }
    mark_dirty_list(hit, voff, voff + size);
}

namespace {
// The mapped pieces of a sparse texture's range, in map order. The physical pools are kept alive by the references, so the copy or the hash can run
// without the pool's lock (several textures of one virtual pool are prepared at the same time by the background workers).
struct VSeg {
    uint64_t at;                 // offset in the texture's storage
    const uint8_t* ptr;
    uint64_t len;
    Ref<MemoryPool> keep;
};
bool virtual_segments(const Texture& t, std::vector<VSeg>& out) {
    MemoryPool* vp = t.vpool.get();
    if (!vp || !t.storage_size) return false;
    const uint64_t t0 = t.voffset, t1 = t.voffset + t.storage_size;
    std::lock_guard<std::mutex> lk(vp->vmu);
    for (const MemoryPool::VirtualMap& m : vp->vmaps) {
        const uint64_t a = std::max(t0, m.voff), b = std::min(t1, m.voff + m.size);
        if (a >= b || !m.phys || !m.phys->base) continue;
        const int64_t src = m.poff + static_cast<int64_t>(a - m.voff);
        if (src < 0 || static_cast<uint64_t>(src) + (b - a) > m.phys->size) continue;
        out.push_back({a - t0, m.phys->base + src, b - a, m.phys});
    }
    return true;
}
}  // namespace

bool texture_gather_virtual(const Texture& t, std::vector<uint8_t>& out) {
    std::vector<VSeg> segs;
    if (!virtual_segments(t, segs)) return false;
    out.assign(static_cast<size_t>(t.storage_size), 0);
    for (const VSeg& s : segs) std::memcpy(out.data() + s.at, s.ptr, static_cast<size_t>(s.len));
    return true;
}

bool texture_read_virtual(const Texture& t, uint64_t at, uint64_t len, uint8_t* dst) {
    std::memset(dst, 0, static_cast<size_t>(len));
    std::vector<VSeg> segs;
    if (!virtual_segments(t, segs)) return false;
    // later maps win (segments are in map order); coverage: the union of the pieces over the range
    std::vector<std::pair<uint64_t, uint64_t>> cov;
    for (const VSeg& s : segs) {
        const uint64_t a = std::max(at, s.at), b = std::min(at + len, s.at + s.len);
        if (a >= b) continue;
        std::memcpy(dst + (a - at), s.ptr + (a - s.at), static_cast<size_t>(b - a));
        cov.emplace_back(a, b);
    }
    std::sort(cov.begin(), cov.end());
    uint64_t p = at;
    for (const auto& c : cov) {
        if (c.first > p) return false;
        p = std::max(p, c.second);
    }
    return p >= at + len;
}

void texture_virtual_tile_ptrs(const Texture& t, std::vector<const uint8_t*>& out) {
    constexpr uint64_t kTile = 0x10000;
    out.assign(static_cast<size_t>((t.storage_size + kTile - 1) / kTile), nullptr);
    std::vector<VSeg> segs;
    if (!virtual_segments(t, segs)) return;
    for (const VSeg& s : segs) {
        const uint64_t first = (s.at + kTile - 1) / kTile, last = (s.at + s.len) / kTile;   // tiles wholly inside the piece
        for (uint64_t i = first; i < last && i < out.size(); ++i) out[static_cast<size_t>(i)] = s.ptr + (i * kTile - s.at);
    }
}

const uint8_t* texture_virtual_direct(const Texture& t, Ref<MemoryPool>& keep) {
    std::vector<VSeg> segs;
    if (!virtual_segments(t, segs) || segs.empty()) return nullptr;
    // a later map over the same range wins in the gather; the direct pointer is only used when the range is one piece
    if (segs.size() != 1 || segs[0].at != 0 || segs[0].len != t.storage_size) return nullptr;
    keep = segs[0].keep;
    return segs[0].ptr;
}

bool texture_hash_virtual(const Texture& t, uint64_t& hash) {
    MemoryPool* vp = t.vpool.get();
    if (!vp || !t.storage_size) return false;
    std::vector<VSeg> segs;
    if (!virtual_segments(t, segs)) return false;
    uint64_t h = 0x9E3779B97F4A7C15ull ^ t.storage_size;
    for (const VSeg& s : segs) h = hash_bytes(s.ptr, static_cast<size_t>(s.len)) ^ (h * 0xC2B2AE3D27D4EB4Full + s.at);   // the position of the segment is part of the hash
    hash = h;
    return true;
}

void textures_cpu_wrote(const void* lo, uint64_t size) {
    const uint64_t a = reinterpret_cast<uint64_t>(lo), b = a + size;
    std::vector<Ref<Texture>> hit;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        // sparse textures whose mapped tiles lie in the written physical memory
        for (Texture* t : g_virtual) {
            MemoryPool* vp = t->vpool.get();
            if (!vp) continue;
            std::lock_guard<std::mutex> vl(vp->vmu);
            bool over = false;
            for (const MemoryPool::VirtualMap& m : vp->vmaps) {
                if (!m.phys || !m.phys->base) continue;
                const uint64_t pa = reinterpret_cast<uint64_t>(m.phys->base) + static_cast<uint64_t>(m.poff), pb = pa + m.size;
                if (pa >= b || pb <= a) continue;
                if (m.voff < t->voffset + t->storage_size && m.voff + m.size > t->voffset) {
                    over = true;
                    // the written bytes as offsets in the texture's storage
                    const uint64_t wa = std::max(a, pa), wb = std::min(b, pb);
                    const uint64_t va = m.voff + (wa - pa), vb = m.voff + (wb - pa);
                    const uint64_t lo = std::max(va, t->voffset), hi = std::min(vb, t->voffset + t->storage_size);
                    if (lo < hi) {
                        std::lock_guard<std::mutex> sl(t->sparse_mu);
                        t->sparse_dirty.emplace_back(lo - t->voffset, hi - t->voffset);
                    }
                }
            }
            if (over) { t->ref(); hit.emplace_back(t); }
        }
        // textures start at or before b; a texture can start before a and reach into the range, so scan back a bounded amount
        auto it = g_by_start.upper_bound(b - 1);
        while (it != g_by_start.begin()) {
            --it;
            Texture* t = it->second;
            const uint64_t ts = it->first, te = ts + std::max<uint64_t>(t->storage_size, 1);
            if (te > a && ts < b) { t->ref(); hit.emplace_back(t); }
            else if (ts + (64ull << 20) < a) break;   // textures are far smaller than 64 MiB
        }
    }
    for (auto& t : hit) {
        tx_mark("tx.flush", t.get());
        tev(t.get(), "cpu_wrote", reinterpret_cast<uint64_t>(lo), size);
        t->cpu_dirty.store(true);
        gpu::mark_texture_cpu_dirty(t.get());
    }
}

void texture_registry_detach_gpu() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& kv : g_by_start) kv.second->gpu = nullptr;
    for (Texture* t : g_virtual) t->gpu = nullptr;
    for (auto& kv : g_overflow) kv.second->gpu = nullptr;
}

Ref<Texture> texture_registry_find_alias(const Texture* t) {
    if (!t || t->is_display) return Ref<Texture>();
    const bool ovf = t->overflow;
    if (ovf ? t->overflow_offset < 0 : !t->mem) return Ref<Texture>();   // offset -1 names no storage
    std::lock_guard<std::mutex> lk(g_mu);
    auto check = [t](Texture* o) { return o != t && !o->destroyed.load() && o->gpu && same_shape(o, t); };
    if (ovf) {
        auto r = g_overflow.equal_range(overflow_key(t));
        for (auto it = r.first; it != r.second; ++it)
            if (check(it->second)) { it->second->ref(); return Ref<Texture>(it->second); }
        return Ref<Texture>();
    }
    auto r = g_by_start.equal_range(reinterpret_cast<uint64_t>(t->mem));
    for (auto it = r.first; it != r.second; ++it)
        if (check(it->second)) { it->second->ref(); return Ref<Texture>(it->second); }
    return Ref<Texture>();
}

size_t texture_registry_overflow_size() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_overflow.size();
}

void textures_mark_override_candidates_dirty() {
    std::vector<Ref<Texture>> hit;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto& kv : g_by_start) {
            Texture* t = kv.second;
            if (t->destroyed.load() || !t->fmt || t->fmt->vk != VK_FORMAT_BC7_SRGB_BLOCK || t->levels != 1 || t->layers != 1) continue;
            if (t->b.width > 128 || t->b.height > 128 || t->b.samples > 1) continue;
            t->ref();
            hit.emplace_back(t);
        }
    }
    for (auto& t : hit) {
        t->cpu_dirty.store(true);
        gpu::mark_texture_cpu_dirty(t.get());
    }
}

std::vector<Ref<Texture>> texture_registry_collect(bool (*pred)(const Texture& t, void* user), void* user) {
    std::vector<Ref<Texture>> hit;
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& kv : g_by_start) {
        Texture* t = kv.second;
        if (t->destroyed.load() || !pred(*t, user)) continue;
        t->ref();
        hit.emplace_back(t);
    }
    return hit;
}

uint32_t texture_clock_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()) + 1;
}

std::vector<Ref<Texture>> texture_registry_all() {
    std::vector<Ref<Texture>> all;
    std::lock_guard<std::mutex> lk(g_mu);
    all.reserve(g_by_start.size() + g_overflow.size() + g_virtual.size());
    auto add = [&](Texture* t) { t->ref(); all.emplace_back(t); };
    for (auto& kv : g_by_start) add(kv.second);
    for (auto& kv : g_overflow) add(kv.second);
    for (Texture* t : g_virtual) add(t);
    return all;
}

std::atomic<uint64_t> g_tex_scan_epoch{1};
std::atomic<bool> g_tex_prep_on{false};

void textures_note_bound(TexturePool* tp, const uint64_t* handles, int count) {
    if (!tp || !handles || count <= 0 || !g_tex_prep_on.load(std::memory_order_relaxed) || !gpu::active()) return;
    thread_local uint64_t t_epoch = 0;
    thread_local uint64_t t_seen[256];
    const uint64_t ep = g_tex_scan_epoch.load(std::memory_order_relaxed);
    if (ep != t_epoch) {
        std::memset(t_seen, 0, sizeof t_seen);
        t_epoch = ep;
    }
    std::shared_lock<std::shared_mutex> lk(tp->lock, std::defer_lock);
    for (int i = 0; i < count; ++i) {
        const uint64_t key = handles[i] * 0x9E3779B97F4A7C15ull ^ reinterpret_cast<uint64_t>(tp);
        uint64_t& slot = t_seen[(key >> 56) & 255];
        if (slot == key) continue;                         // looked at since the last bump: nothing new can be waiting behind it
        slot = key;
        if (!lk.owns_lock()) lk.lock();
        const uint32_t tid = handle_texture_id(handles[i]);
        if (static_cast<int>(tid) >= tp->count || tid >= tp->entries.size()) continue;
        const TexturePool::Entry& e = tp->entries[tid];
        if (e.used && e.tex) gpu::texture_bound(e.tex.get());
    }
}

size_t texture_registry_size() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_by_start.size();
}

namespace {
// Gauges (nxrt/gauge.h). The callbacks take g_mu; they are removed before the maps they read are destroyed.
struct RegistryGauges {
    RegistryGauges() {
        nxrt::gauge_register("nvn.texture_registry", [] { return static_cast<int64_t>(texture_registry_size()); });
        nxrt::gauge_register("nvn.texture_registry.virtual", [] {
            std::lock_guard<std::mutex> lk(g_mu);
            return static_cast<int64_t>(g_virtual.size());
        });
        nxrt::gauge_register("nvn.texture_registry.overflow", [] { return static_cast<int64_t>(texture_registry_overflow_size()); });
    }
    ~RegistryGauges() {
        nxrt::gauge_unregister("nvn.texture_registry");
        nxrt::gauge_unregister("nvn.texture_registry.virtual");
        nxrt::gauge_unregister("nvn.texture_registry.overflow");
    }
};
RegistryGauges g_registry_gauges;
}  // namespace

}  // namespace nxrt::nvn::be
