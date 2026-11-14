// SPDX-License-Identifier: GPL-3.0-or-later
// Fire Emblem: Fortune's Weave: output and render size (notes/resolution.md). Offsets are in main of update 131072.
//
// The game picks one of six render configs once at boot (0x1818350) and applies it once (0x64a660): DLSS output size, render size (half
// per side) and shadow-map size. The configs are built at 0xaa8f90 as std::shared_ptrs at 0x223e0b0 (16 bytes each); the data is the
// object + 0x18: output w/h (+0, +4), render w/h (+8, +0xc), shadow-map base size (+0x10), then quality factors and flags up to +0x7c.
// The selection calls nn::oe::GetCurrentGraphicsScale after the table is built and before it reads it, so the host's answer to that call
// rewrites the sizes first; the game then makes its targets, DLSS network, UI layers and jitter sequence (8 x ratio^2 phases) itself.
// The window textures are 3840x2160 (the graphics object's seed size, 0x18102a4), the cap for the output; the window crop is
// min(output, display size), so the vi answer must give 2160p for outputs above 1440p (the docked default does).
//
// [fefw] output = console | 720p | 1080p | 1440p | 2160p | 4k | window | WxH     (console: each config keeps its own)
// [fefw] ultrawide = auto | off                                                   (auto: an output wider than 16:9 keeps its shape,
//                                                                                  ObeliskChamber_wide.cpp; a narrower one gets 16:9 inside it)
// [fefw] render_ratio = 0 (the config's own, 0.5) | 0.25..1.0 per side            (1.0: DLAA)
// [fefw] render = WxH                                                              (wins over render_ratio)
// [fefw] shadow_size = 0 (the config's) | 512..8192                                (shadow-map base size)
// [fefw] quality = console | docked                                                (docked: config 0's quality fields in every config)
// [fefw] rt_pool_overflow_mb = 3072 | 0..3072                                       (room past the render-target pool, below)
//
// The render-target pool. Graphics init makes it at 0x646f88 (`mov w9, #0x3f000000`, flags 0xa1: CPU no access, GPU cached,
// compressible) as a pool object of 0x160 bytes (0x5d640): the NVNmemoryPool at +8 over 0x3f000000 bytes of the game's heap, and at +0x108
// an nn::mem::NumberLineAllocator in 256-byte units that is given the whole pool (AddRange(0, size / 256)). Every render target and the
// shadow maps are placed by it. 1008 MiB holds the console's sizes; 4096 or 8192 shadows (64 or 256 MiB each, five of them) or a 2160p
// output rendered above half per side do not fit: the allocator answers offset -1 and the targets get no storage (a black picture).
// The back end gives a texture placed past the end of a pool the CPU cannot access GPU memory of its own (nvnTextureInitialize, host-backed
// storage). So the host stands in front of that allocator (its SDK imports): when it finds no room, a room past the pool answers instead,
// rt_pool_overflow_mb from offset 1 GiB on (first fit; Free gives the place back). The allocator itself is left as it is: whatever fits in
// the pool is placed exactly where it was before. 0 = off: such targets get offset -1 (the back end still backs them, but texture objects over
// one of them cannot share its image), and above 1440p output the render is capped at half per side.
#include "ObeliskChamber_game.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <cstdlib>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/nvn_backend.h"
#include "nxrt/platform.h"

namespace nxrt::games::ObeliskChamber {

namespace {

constexpr const char* kTag = "fefw";
constexpr const char* kGetScale = "_ZN2nn2oe23GetCurrentGraphicsScaleEv";
constexpr uint64_t kConfigs = 0x223e0b0;   // 6 x std::shared_ptr<config>
constexpr int kConfigCount = 6;
// Configs 0 to 2 are the docked ones, 3 to 5 the handheld ones. The handheld ones differ from the docked in a few quality fields (a user's
// log: +0x10 shadow size 1536 for 2048, +0x30 0.75 for 1, +0x34 1 for 0, +0x58 0.7 for 1, +0x64 0.8 for 1, +0x78 1 for 0): what the console
// gives up in handheld mode. [fefw] quality = handheld puts config 3's fields in every config, for a PC that wants the frame rate.
constexpr int kHandheldConfig = 3;
constexpr uint64_t kConfigBytes = 0x80;
constexpr uint64_t kShadow = 0x10;
constexpr int kMaxW = 3840, kMaxH = 2160;
// Changing the render size while the game runs ([nvn] dynres). The game has its own path for it (a console is docked and undocked while it
// runs): each frame its task 0x1818600 (priority 324) looks at the operation and performance mode events and at a "settings changed"
// byte (0x22ab001, set at 0x1814210); when one of them is set it picks the config again and writes its sizes as the pending ones in the
// graphics object (render +0x3408/+0x340c, output +0x3410/+0x3414), and the next task, 0x64a660 (priority 326), sees pending != current
// (+0x33f8..+0x3404), waits for the GPU, frees and rebuilds the render targets and sets the upscaler up again (0x1913b40). So the host
// rewrites the configs' render size and sets that byte; the rest is the game's.
constexpr uint64_t kSettingsChanged = 0x22ab001;
constexpr uint64_t kGraphicsGot = 0x216a050;    // -> the global that holds the graphics object (0x1813ff4)
constexpr uint64_t kCurRender = 0x33f8;         // in the graphics object: the render size in force (w, h)

struct Res {
    uint64_t base = 0;
    uint64_t sdk_fn = 0;
    int out_w = 0, out_h = 0;      // 0: the config's own
    int render_w = 0, render_h = 0;
    double ratio = 0;              // 0: the config's own
    int shadow = 0;
    bool docked_quality = false;
    bool handheld_quality = false; // [fefw] quality = handheld: the first handheld config's quality fields in every config
    struct Set { uint64_t off; uint32_t bits; uint32_t size; };
    std::vector<Set> sets;         // [fefw] config_set: quality fields written into every config
    std::atomic<float> dyn{1.0f};  // [nvn] dynres: the share of the configured render size in force
    std::atomic<bool> logged{false};
    // Changes while the game runs (res_live_apply): the configs as the game built them, so "the config's own" can come back, and a lock,
    // since the main thread (the menu) and the render thread (dynres) both rewrite them.
    std::mutex mu;
    bool have_orig = false;
    uint8_t orig[kConfigCount][kConfigBytes] = {};
    bool quality_written = false;  // the fields past the sizes were rewritten at least once
    bool nudge = false;            // the render height two rows short: makes the game rebuild its targets when the size did not change
    std::atomic<int> nudge_frames{0};
};

Res& rs() {
    static Res r;
    return r;
}

// ---- the render-target pool's allocator ------------------------------------------------------------------------------------------
constexpr uint64_t kRtPoolSize = 0x3f000000;   // main 0x646f88
constexpr int kPoolCpuNoAccess = 0x1;           // NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS
constexpr uint64_t kAllocatorAt = 0x108;        // in the pool object: the NumberLineAllocator ...
constexpr uint64_t kNvnPoolAt = 0x8;            // ... and the NVNmemoryPool it places into
constexpr uint64_t kUnit = 256;                 // the allocator's unit (0x5d8c0: size + 255 >> 8, the alignment >> 8)
constexpr int64_t kOverflowStart = (1ll << 30) / kUnit;   // the room past the pool starts at 1 GiB (16 MiB past the pool's end)
constexpr int64_t kOverflowMaxMb = 3072;        // up to 4 GiB of offsets in all

// An SDK function the hooks below stand in front of. Its address is looked up at the first call: the modules are not loaded at install.
struct SdkFn {
    const char* sym;
    std::atomic<uint64_t> addr{0};
};
SdkFn g_add_range{"_ZN2nn3mem19NumberLineAllocator8AddRangeEii"};          // (this, int first, int count)
SdkFn g_alloc3{"_ZN2nn3mem19NumberLineAllocator8AllocateEPiii"};           // bool (this, int* out, int count, int align)
SdkFn g_alloc2{"_ZN2nn3mem19NumberLineAllocator8AllocateEPii"};            // bool (this, int* out, int count)
SdkFn g_free{"_ZN2nn3mem19NumberLineAllocator4FreeEi"};                    // (this, int first)
SdkFn g_finalize{"_ZN2nn3mem19NumberLineAllocator8FinalizeEv"};            // (this)

void call_sdk(::Ctx* c, SdkFn& f) {
    uint64_t a = f.addr.load(std::memory_order_acquire);
    if (!a) {
        a = module_find_symbol(f.sym);
        if (!a || hle_is_thunk(a)) trap("nn::mem::NumberLineAllocator: the SDK's own function is not found", a);
        f.addr.store(a, std::memory_order_release);
    }
    const uint64_t lr = c->x[30], fn = c->fn;
    dispatch_call(c, a);
    c->x[30] = lr;
    c->fn = fn;
}

// The room past one render-target pool, in the allocator's units: what the pool's allocator cannot place goes here (first fit).
struct Room {
    std::map<int64_t, int64_t> free;   // start -> length
    std::map<int64_t, int64_t> used;   // start -> length
    int64_t in_use = 0;
};

struct RtPool {
    bool installed = false;
    int64_t room_units = 0;            // [fefw] rt_pool_overflow_mb in units (0: off)
    std::atomic<bool> extended{false};
    std::mutex mu;
    std::map<uint64_t, Room> rooms;    // the pool's NumberLineAllocator (guest address) -> its room
    uint64_t placed = 0;               // allocations placed in a room so far (logs)
};

RtPool& rp() {
    static RtPool p;
    return p;
}

int64_t room_take(Room& r, int64_t count, int64_t align) {
    for (auto it = r.free.begin(); it != r.free.end(); ++it) {
        const int64_t at = (it->first + align - 1) / align * align, end = it->first + it->second;
        if (at + count > end) continue;
        const int64_t s0 = it->first;
        r.free.erase(it);
        if (at > s0) r.free.emplace(s0, at - s0);
        if (at + count < end) r.free.emplace(at + count, end - (at + count));
        r.used.emplace(at, count);
        r.in_use += count;
        return at;
    }
    return -1;
}

bool room_give(Room& r, int64_t at) {
    auto u = r.used.find(at);
    if (u == r.used.end()) return false;
    int64_t s0 = at, len = u->second;
    r.in_use -= len;
    r.used.erase(u);
    auto next = r.free.lower_bound(s0);
    if (next != r.free.end() && next->first == s0 + len) { len += next->second; next = r.free.erase(next); }
    if (next != r.free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == s0) { s0 = prev->first; len += prev->second; r.free.erase(prev); }
    }
    r.free.emplace(s0, len);
    return true;
}

// AddRange(int first, int count): the SDK's. The render-target pool's allocator (the whole pool as its range, and the NVNmemoryPool 0x100
// bytes before it a live pool of that size the CPU cannot access) also gets a room past the pool.
void nla_add_range(::Ctx* c) {
    RtPool& p = rp();
    const uint64_t self = c->x[0];
    const int32_t first = static_cast<int32_t>(c->x[1]), count = static_cast<int32_t>(c->x[2]);
    call_sdk(c, g_add_range);
    if (!p.room_units || first != 0 || count <= 0 || static_cast<uint64_t>(count) * kUnit != kRtPoolSize || self < kAllocatorAt) return;
    const uint64_t guest_pool = self - kAllocatorAt + kNvnPoolAt;
    uint64_t size = 0;
    int flags = 0;
    if (!mem::is_mapped(guest_pool, 16) || !nvn::memory_pool_info(reinterpret_cast<const void*>(guest_pool), &size, &flags) || size != kRtPoolSize ||
        !(flags & kPoolCpuNoAccess)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            NXLOG_W(kTag, "an allocator over 0x%llx bytes is not the render-target pool's (pool 0x%llx, size 0x%llx, flags 0x%x): no room past it",
                    static_cast<unsigned long long>(kRtPoolSize), static_cast<unsigned long long>(guest_pool), static_cast<unsigned long long>(size),
                    static_cast<unsigned>(flags));
        return;
    }
    {
        std::lock_guard<std::mutex> lk(p.mu);
        Room& r = p.rooms[self];
        r = Room();
        r.free.emplace(kOverflowStart, p.room_units);
    }
    if (!p.extended.exchange(true))
        NXLOG_I(kTag, "render-target pool (%llu MiB, flags 0x%x): what its allocator cannot place goes to offsets %llu to %llu MiB, with GPU memory of its "
                      "own ([fefw] rt_pool_overflow_mb = %lld)",
                static_cast<unsigned long long>(kRtPoolSize >> 20), static_cast<unsigned>(flags), static_cast<unsigned long long>(kOverflowStart * kUnit >> 20),
                static_cast<unsigned long long>((kOverflowStart + p.room_units) * kUnit >> 20), static_cast<long long>(p.room_units * kUnit >> 20));
}

// Allocate(int* out, int count[, int align]): the SDK's. When it finds no room in the render-target pool, the room past the pool answers.
void nla_allocate(::Ctx* c, SdkFn& f, bool aligned) {
    RtPool& p = rp();
    const uint64_t self = c->x[0], out = c->x[1];
    const int64_t count = static_cast<int32_t>(c->x[2]), align = aligned ? std::max<int64_t>(1, static_cast<int32_t>(c->x[3])) : 1;
    call_sdk(c, f);
    if ((c->x[0] & 1) || count <= 0 || !out) return;
    std::lock_guard<std::mutex> lk(p.mu);
    auto it = p.rooms.find(self);
    if (it == p.rooms.end()) return;
    const int64_t at = room_take(it->second, count, align);
    if (at < 0 || !mem::is_mapped(out, 4)) {
        if (at >= 0) room_give(it->second, at);
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            NXLOG_W(kTag, "render-target pool: %lld KiB do not fit in the pool or the room past it (%lld of %lld MiB in use): the game's allocator fails, "
                          "as it did before",
                    static_cast<long long>(count * kUnit >> 10), static_cast<long long>(it->second.in_use * kUnit >> 20),
                    static_cast<long long>(p.room_units * kUnit >> 20));
        return;
    }
    const int32_t v = static_cast<int32_t>(at);
    std::memcpy(reinterpret_cast<void*>(out), &v, 4);
    c->x[0] = 1;
    if (p.placed++ < 24)
        NXLOG_I(kTag, "render-target pool full: %lld KiB placed past it at offset 0x%llx (%lld MiB past the pool in use)", static_cast<long long>(count * kUnit >> 10),
                static_cast<unsigned long long>(at * kUnit), static_cast<long long>(it->second.in_use * kUnit >> 20));
}
void nla_allocate3(::Ctx* c) { nla_allocate(c, g_alloc3, true); }
void nla_allocate2(::Ctx* c) { nla_allocate(c, g_alloc2, false); }

// Free(int first): a place in the room goes back to it; the rest to the SDK's.
void nla_free(::Ctx* c) {
    RtPool& p = rp();
    const uint64_t self = c->x[0];
    const int64_t at = static_cast<int32_t>(c->x[1]);
    if (at >= kOverflowStart) {
        std::lock_guard<std::mutex> lk(p.mu);
        auto it = p.rooms.find(self);
        if (it != p.rooms.end() && room_give(it->second, at)) return;
    }
    call_sdk(c, g_free);
}

void nla_finalize(::Ctx* c) {
    RtPool& p = rp();
    {
        std::lock_guard<std::mutex> lk(p.mu);
        p.rooms.erase(c->x[0]);
    }
    call_sdk(c, g_finalize);
}

void rt_pool_install() {
    RtPool& p = rp();
    if (p.installed) return;
    p.installed = true;
    const int64_t mb = std::clamp<int64_t>(config_get_int("fefw", "rt_pool_overflow_mb", kOverflowMaxMb), 0, kOverflowMaxMb);
    if (mb <= 0) {
        NXLOG_I(kTag, "render-target pool: no room past it ([fefw] rt_pool_overflow_mb = 0)");
        return;
    }
    p.room_units = (mb << 20) / static_cast<int64_t>(kUnit);
    const std::pair<SdkFn*, HleFn> hooks[] = {{&g_add_range, nla_add_range}, {&g_alloc3, nla_allocate3}, {&g_alloc2, nla_allocate2},
                                              {&g_free, nla_free}, {&g_finalize, nla_finalize}};
    for (const auto& h : hooks) {
        hle_register(h.first->sym, h.second);
        module_override_symbol(h.first->sym);
    }
}

bool parse_wxh(const std::string& v, int* w, int* h) {
    int a = 0, b = 0;
    if (std::sscanf(v.c_str(), "%dx%d", &a, &b) != 2 || a < 64 || b < 64) return false;
    *w = a;
    *h = b;
    return true;
}

// The window's client size at start, fitted to 16:9; with [fefw] ultrawide a wider window is taken as it is (ObeliskChamber_wide.cpp).
void window_output(int* w, int* h, bool wide) {
    int pw = 0, ph = 0;
    platform::pixel_size(&pw, &ph);
    if (pw <= 0 || ph <= 0) return;
    if (wide && wide_aspect(pw, ph)) {
        *w = pw & ~1;
        *h = ph & ~1;
        return;
    }
    int fw = std::min(pw, ph * 16 / 9);
    int fh = fw * 9 / 16;
    *w = fw & ~1;
    *h = fh & ~1;
}

// The options that can change while the game runs (res_live_apply): render size, shadow-map size and quality.
void read_live_options(Res& r) {
    double ratio = config_get_double("fefw", "render_ratio", 0.0);
    for (const double f : {1.0 / 3.0, 2.0 / 3.0})   // 0.33 / 0.67 mean thirds (1440p -> 1706x960, as the console's config 2)
        if (std::fabs(ratio - f) < 0.006) ratio = f;
    r.ratio = ratio > 0 ? std::clamp(ratio, 0.25, 1.0) : 0;
    r.render_w = r.render_h = 0;
    parse_wxh(config_get_str("fefw", "render", ""), &r.render_w, &r.render_h);
    const int64_t sh = config_get_int("fefw", "shadow_size", 0);
    r.shadow = sh > 0 ? static_cast<int>(std::clamp<int64_t>(sh, 512, 8192)) : 0;
    const std::string q = config_get_str("fefw", "quality", "console");
    r.docked_quality = q == "docked";
    r.handheld_quality = q == "handheld";
}

void read_options(Res& r) {
    std::string o = config_get_str("fefw", "output", "console");
    for (char& ch : o) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const bool wide = wide_mode() != "off";
    if (o == "720p") r.out_w = 1280, r.out_h = 720;
    else if (o == "1080p") r.out_w = 1920, r.out_h = 1080;
    else if (o == "1440p") r.out_w = 2560, r.out_h = 1440;
    else if (o == "2160p" || o == "4k") r.out_w = 3840, r.out_h = 2160;
    else if (o == "window") window_output(&r.out_w, &r.out_h, wide);
    else if (o != "console" && !o.empty() && !parse_wxh(o, &r.out_w, &r.out_h)) NXLOG_W(kTag, "[fefw] output = %s is not known: console", o.c_str());
    if (wide && r.out_w > 0 && wide_aspect(r.out_w, r.out_h)) {
        // [fefw] ultrawide: the shape is kept within the 3840x2160 window textures (5120x1440 -> 3840x1080; the window scales it up)
        const double s = std::min({1.0, static_cast<double>(kMaxW) / r.out_w, static_cast<double>(kMaxH) / r.out_h});
        if (s < 1.0) {
            const int w0 = r.out_w, h0 = r.out_h;
            r.out_w = 2 * static_cast<int>(std::lround(w0 * s / 2));
            r.out_h = 2 * static_cast<int>(std::lround(h0 * s / 2));
            NXLOG_W(kTag, "[fefw] output %dx%d is above the game's %dx%d window textures: %dx%d, the same shape", w0, h0, kMaxW, kMaxH, r.out_w, r.out_h);
        }
    } else if (wide && r.out_w > 0 && static_cast<double>(r.out_w) * 9.0 * 1.01 < static_cast<double>(r.out_h) * 16.0) {
        // narrower than 16:9 (16:10, 4:3): the game's 16:9 picture inside it, letterboxed by the window
        const int h0 = r.out_h;
        r.out_h = (r.out_w * 9 / 16) & ~1;
        NXLOG_I(kTag, "[fefw] output %dx%d is narrower than 16:9: %dx%d, letterboxed ([fefw] ultrawide = auto)", r.out_w, h0, r.out_w, r.out_h);
    }
    if (r.out_w > kMaxW || r.out_h > kMaxH) {
        NXLOG_W(kTag, "[fefw] output %dx%d is above the game's 3840x2160 window textures: 3840x2160", r.out_w, r.out_h);
        r.out_w = std::min(r.out_w, kMaxW);
        r.out_h = std::min(r.out_h, kMaxH);
    }
    read_live_options(r);
    // [fefw] config_set = 0x3c=f0.5, 0x38=b0, 0x78=i2 : a float, byte or 32-bit integer at an offset of every render config (0x10..0x7f;
    // the offsets and the game's values are in the "render config N quality fields" log lines). An experiment key.
    {
        const std::string spec = config_get_str("fefw", "config_set", "");
        size_t at = 0;
        while (at < spec.size()) {
            size_t end = spec.find(',', at);
            if (end == std::string::npos) end = spec.size();
            std::string item = spec.substr(at, end - at);
            at = end + 1;
            item.erase(std::remove(item.begin(), item.end(), ' '), item.end());
            const size_t eq = item.find('=');
            if (eq == std::string::npos || eq + 2 > item.size()) continue;
            Res::Set q{std::strtoull(item.substr(0, eq).c_str(), nullptr, 0), 0, 4};
            const char kind = item[eq + 1];
            const std::string val = item.substr(eq + 2);
            if (kind == 'f') { const float f = std::strtof(val.c_str(), nullptr); std::memcpy(&q.bits, &f, 4); }
            else if (kind == 'b') { q.bits = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 0)) & 0xff; q.size = 1; }
            else if (kind == 'i') q.bits = static_cast<uint32_t>(std::strtol(val.c_str(), nullptr, 0));
            else { NXLOG_W(kTag, "[fefw] config_set: '%s' is not offset=f<float>, b<byte> or i<int>", item.c_str()); continue; }
            r.sets.push_back(q);
            NXLOG_I(kTag, "[fefw] config_set: offset 0x%llx = %s (%u bytes) in every render config", static_cast<unsigned long long>(q.off), item.c_str() + eq + 1, q.size);
        }
    }
}

bool any_option(const Res& r) { return r.out_w > 0 || r.ratio > 0 || r.render_w > 0 || r.shadow > 0 || r.docked_quality || r.handheld_quality || !r.sets.empty(); }

uint64_t config_data(const Res& r, int i) {
    uint64_t p = 0;
    const uint64_t slot = r.base + kConfigs + 16ull * i;
    if (!mem::is_mapped(slot, 8)) return 0;
    std::memcpy(&p, reinterpret_cast<const void*>(slot), 8);
    return p && mem::is_mapped(p, kConfigBytes) ? p : 0;
}

// Rewrites the six configs from the game's own values (idempotent: every call with the same options gives the same values).
void apply(Res& r) {
    std::lock_guard<std::mutex> lk(r.mu);
    uint64_t cfg[kConfigCount];
    for (int i = 0; i < kConfigCount; ++i)
        if (!(cfg[i] = config_data(r, i))) return;   // not built yet
    const bool first = !r.logged.exchange(true);
    if (!r.have_orig) {
        for (int i = 0; i < kConfigCount; ++i) std::memcpy(r.orig[i], reinterpret_cast<const void*>(cfg[i]), kConfigBytes);
        r.have_orig = true;
    }
    if (r.docked_quality || r.handheld_quality || !r.sets.empty() || r.shadow > 0) r.quality_written = true;
    if (first) {   // the game's own quality fields, before anything is copied or set: what the handheld configs reduce
        for (int i = 0; i < kConfigCount; ++i) {
            std::string line;
            for (uint64_t off = kShadow; off < kConfigBytes; off += 4) {
                uint32_t w = 0;
                float f = 0;
                std::memcpy(&w, reinterpret_cast<const void*>(cfg[i] + off), 4);
                std::memcpy(&f, &w, 4);
                char b[48];
                if (w == 0) snprintf(b, sizeof b, " %02x:0", static_cast<unsigned>(off));
                else if (std::isfinite(f) && std::fabs(f) > 1e-4f && std::fabs(f) < 1e7f) snprintf(b, sizeof b, " %02x:%g", static_cast<unsigned>(off), static_cast<double>(f));
                else snprintf(b, sizeof b, " %02x:#%x", static_cast<unsigned>(off), w);
                line += b;
            }
            NXLOG_I(kTag, "render config %d quality fields (offset:value, #hex = an integer or bytes):%s", i, line.c_str());
        }
    }
    for (int i = 0; i < kConfigCount; ++i) {
        int32_t v[5];
        std::memcpy(v, r.orig[i], sizeof v);
        if (r.quality_written)   // the game's own fields first: an option that went back to "the config's own" leaves nothing behind
            std::memcpy(reinterpret_cast<void*>(cfg[i] + kShadow), r.orig[r.docked_quality ? 0 : r.handheld_quality ? kHandheldConfig : i] + kShadow, kConfigBytes - kShadow);
        for (const Res::Set& q : r.sets) {   // [fefw] config_set
            if (q.off < kShadow || q.off + q.size > kConfigBytes) continue;
            std::memcpy(reinterpret_cast<void*>(cfg[i] + q.off), &q.bits, q.size);
        }
        int32_t ow = r.out_w > 0 ? r.out_w : v[0], oh = r.out_h > 0 ? r.out_h : v[1];
        int32_t rw = v[2], rh = v[3];
        if (r.render_w > 0) rw = r.render_w, rh = r.render_h;
        else if (r.ratio > 0)   // even sizes: the SSAO pass works on 2x2 blocks
            rw = 2 * static_cast<int32_t>(std::lround(ow * r.ratio / 2)), rh = 2 * static_cast<int32_t>(std::lround(oh * r.ratio / 2));
        else if (r.out_w > 0) rw = (ow + 1) / 2, rh = (oh + 1) / 2;
        rw = std::clamp(rw, 64, ow);
        rh = std::clamp(rh, 64, oh);
        // The game's render targets live in one 0x3f000000-byte memory pool (made at 0x646f88). Above 1440p output it holds a render of at
        // most half the output per side: 3840x2160 from 2560x1440 runs out and the game's allocator gives the HUD and post targets
        // offset -1 (a black picture). 2560x1440 at full size fits. With room past the pool (rt_pool_install) the rest goes there.
        if (!rp().extended.load() && static_cast<int64_t>(ow) * oh > 2560 * 1440 && (rw > (ow + 1) / 2 || rh > (oh + 1) / 2)) {
            if (first)
                NXLOG_W(kTag, "render %dx%d for output %dx%d does not fit the game's render-target pool: %dx%d", rw, rh, ow, oh, (ow + 1) / 2,
                        (oh + 1) / 2);
            rw = (ow + 1) / 2;
            rh = (oh + 1) / 2;
        }
        if (const float dyn = r.dyn.load(std::memory_order_relaxed); dyn < 1.0f) {
            rw = std::max(64, 2 * static_cast<int32_t>(std::lround(rw * static_cast<double>(dyn) / 2)));
            rh = std::max(64, 2 * static_cast<int32_t>(std::lround(rh * static_cast<double>(dyn) / 2)));
        }
        if (r.nudge && rh >= 128) rh -= 2;
        const int32_t sizes[4] = {ow, oh, rw, rh};
        std::memcpy(reinterpret_cast<void*>(cfg[i]), sizes, sizeof sizes);
        if (r.shadow > 0) std::memcpy(reinterpret_cast<void*>(cfg[i] + kShadow), &r.shadow, 4);
        int32_t shadow = 0;
        std::memcpy(&shadow, reinterpret_cast<const void*>(cfg[i] + kShadow), 4);
        if (first)
            NXLOG_I(kTag, "render config %d: output %dx%d from %dx%d, shadow %d (was %dx%d from %dx%d, shadow %d)%s", i, ow, oh, rw, rh, shadow, v[0],
                    v[1], v[2], v[3], v[4], r.docked_quality && i != 0 ? ", config 0's quality" : r.handheld_quality && i != kHandheldConfig ? ", the handheld config's quality" : "");
    }
}

// [nvn] dynres (render thread): the game renders at `scale` of the configured render size from its next frame on.
void dynres_apply(float scale) {
    Res& r = rs();
    uint64_t got = 0, gfx = 0;
    int32_t cur[2] = {0, 0};
    if (mem::is_mapped(r.base + kGraphicsGot, 8)) std::memcpy(&got, reinterpret_cast<const void*>(r.base + kGraphicsGot), 8);
    if (got && mem::is_mapped(got, 8)) std::memcpy(&gfx, reinterpret_cast<const void*>(got), 8);
    if (gfx && mem::is_mapped(gfx + kCurRender, 8)) std::memcpy(cur, reinterpret_cast<const void*>(gfx + kCurRender), 8);
    r.dyn.store(std::clamp(scale, 0.25f, 1.0f));
    apply(r);
    uint64_t c0 = config_data(r, 0);
    int32_t want[4] = {0, 0, 0, 0};
    if (c0) std::memcpy(want, reinterpret_cast<const void*>(c0), sizeof want);
    if (!mem::is_mapped(r.base + kSettingsChanged, 1)) {
        NXLOG_W(kTag, "[dynres] the game's settings-changed byte is not mapped: the render size stays");
        return;
    }
    *reinterpret_cast<volatile uint8_t*>(r.base + kSettingsChanged) = 1;
    NXLOG_I(kTag, "[dynres] asked the game for %dx%d (x%.2f) of output %dx%d; it rendered %dx%d", want[2], want[3], static_cast<double>(scale), want[0], want[1],
            cur[0], cur[1]);
}

bool ask_rebuild(Res& r) {
    if (!mem::is_mapped(r.base + kSettingsChanged, 1)) return false;
    *reinterpret_cast<volatile uint8_t*>(r.base + kSettingsChanged) = 1;
    return true;
}

// nn::oe::GetCurrentGraphicsScale(): the SDK's own answer, after the configs are rewritten.
void get_current_graphics_scale(::Ctx* c) {
    Res& r = rs();
    apply(r);
    if (!r.sdk_fn) {
        const int64_t v = config_get_int("am", "graphics_scale", 10);
        c->x[0] = v == 20 ? 1 : v == 30 ? 2 : 0;
        return;
    }
    const uint64_t lr = c->x[30], fn = c->fn;
    dispatch_call(c, r.sdk_fn);
    c->x[30] = lr;
    c->fn = fn;
}

}  // namespace

bool res_rt_pool_extended() { return rp().extended.load(); }

// The menu changed [fefw] render_ratio, shadow_size or quality (main thread): the configs are rewritten and the game is asked to pick its
// config again, as dynres does. The game only rebuilds when the render size changed, so for shadows and quality (`force`) the height goes two
// rows short now and back a few frames later (res_frame): two rebuilds, each a short hitch.
void res_live_apply(bool force) {
    Res& r = rs();
    if (!r.base) return;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        read_live_options(r);
        r.nudge = force;
    }
    apply(r);
    uint64_t c0 = config_data(r, 0);
    int32_t want[5] = {};
    if (c0) std::memcpy(want, reinterpret_cast<const void*>(c0), sizeof want);
    const bool asked = c0 && ask_rebuild(r);
    if (force) r.nudge_frames.store(asked ? 6 : 0);
    if (!asked) {
        std::lock_guard<std::mutex> lk(r.mu);
        r.nudge = false;
    }
    NXLOG_I(kTag, "[live] render %dx%d of output %dx%d, shadow %d, %s quality%s%s", want[2], want[3], want[0], want[1], want[4],
            r.docked_quality ? "docked" : r.handheld_quality ? "handheld" : "console", force ? " (a rebuild forced: the height is two rows short for a few frames)" : "",
            asked ? "" : ": the game's render configs are not there yet, nothing changed");
}

void res_frame() {
    Res& r = rs();
    if (r.nudge_frames.load(std::memory_order_relaxed) <= 0 || r.nudge_frames.fetch_sub(1) != 1) return;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.nudge = false;
    }
    apply(r);
    ask_rebuild(r);
    NXLOG_I(kTag, "[live] the render height is back: the second rebuild");
}

// [nvn] dynres switched in the menu. It needs [fefw] output (the six render configs then share one size).
bool res_dynres_live(bool on) {
    Res& r = rs();
    if (!on) {
        nxrt::nvn::backend_dynres_on(false);
        NXLOG_I(kTag, "[live] dynamic resolution off: the render size goes back to the configured one");
        return true;
    }
    if (r.out_w <= 0) {
        NXLOG_W(kTag, "[live] dynamic resolution needs an Output Size other than the console's: off until one is set and the game restarted");
        return false;
    }
    nxrt::nvn::backend_dynres_enable(&dynres_apply);
    nxrt::nvn::backend_dynres_on(true);
    NXLOG_I(kTag, "[live] dynamic resolution on");
    return true;
}

void res_install(uint64_t base) {
    Res& r = rs();
    if (r.base) return;
    r.base = base;
    rt_pool_install();
    read_options(r);
    if (wide_mode() != "off") wide_install(base, r.out_w, r.out_h);   // only an output wider than 16:9 installs anything
    if (!any_option(r)) return;
    if (config_get_bool("nvn", "dynres", false)) {
        if (r.out_w > 0) nxrt::nvn::backend_dynres_enable(&dynres_apply);
        else NXLOG_W(kTag, "[nvn] dynres needs [fefw] output (the six render configs then share one size): off");
    }
    r.sdk_fn = module_find_symbol(kGetScale);
    hle_register(kGetScale, get_current_graphics_scale);
    module_override_symbol(kGetScale);
    NXLOG_I(kTag, "render size: output %s, render %s, shadow %s%s", r.out_w > 0 ? (std::to_string(r.out_w) + "x" + std::to_string(r.out_h)).c_str() : "console",
            r.render_w > 0 ? (std::to_string(r.render_w) + "x" + std::to_string(r.render_h)).c_str()
            : r.ratio > 0  ? ("x" + std::to_string(r.ratio)).c_str()
                           : "console (half)",
            r.shadow > 0 ? std::to_string(r.shadow).c_str() : "console", r.docked_quality ? ", docked quality" : r.handheld_quality ? ", handheld quality" : "");
}

}  // namespace nxrt::games::ObeliskChamber
