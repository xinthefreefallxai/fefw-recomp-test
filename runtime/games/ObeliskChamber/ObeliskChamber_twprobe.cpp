// SPDX-License-Identifier: GPL-3.0-or-later
// Fire Emblem: Fortune's Weave, a debug probe for the dialogue typewriter (notes/fps120.md). [fefw] tw_probe = <seconds after play mode
// starts> (0 = off): at that time the guest heap is searched for nn::ui2d::TextBox and ui::control::TextBoxEx objects (by vtable), then
// their first 0x400 bytes are sampled every 2 ms for [fefw] tw_probe_ms (default 4000). Words that change step by step in one direction
// while a line types are logged with their object, offset, values and change times.
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <utility>
#include <cstdlib>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/hooks.h"
#include "nxrt/abi.h"
#include "nxrt/symbols.h"
#include "nxrt/kernel.h"

namespace nxrt::nvn {
void debug_text_draw(uint64_t* textbox, uint64_t* buffer, int* instances);   // gfx/nvn/api_cmd_work.cpp ([nvn] draw_bt)
}

namespace nxrt::games::ObeliskChamber {

namespace {

constexpr const char* kTag = "fefw.twprobe";
constexpr size_t kObjBytes = 0x800;

double now_ms() {
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(q.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

bool readable(DWORD p) {
    return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) && !(p & PAGE_GUARD);
}

struct Obj {
    uint64_t addr, vt;
    size_t words = 0;
    size_t parent = ~size_t(0);
    std::vector<uint32_t> last;
    std::vector<uint32_t> first;
    std::vector<int> ups, downs, changes;
    std::vector<double> t_first, t_last;
};


// Watch mode ([fefw] tw_probe_watch = 1): the TextBox whose display-string buffer ([TextBox+0x148]) changes its character count
// (+0x98, what the text draw's instance count is) most often is the dialogue line; then its page is made read-only every 30 ms and each
// write to the count is recorded by a vectored handler: the host instruction, the guest function being run (ctx->fn) and x30.
struct Hit {
    uint64_t rip, addr, fn, lr;
};
constexpr int kMaxHits = 256;
Hit g_hits[kMaxHits];
std::atomic<int> g_nhits{0};
constexpr int kPages = 4;
std::atomic<uint64_t> g_watch_page[kPages], g_watch_at[kPages];

thread_local uint64_t t_rearm = 0;   // page to make read-only again after the single step of the faulting write
std::atomic<bool> g_armed{false};

LONG CALLBACK watch_veh(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && t_rearm) {
        DWORD old;
        if (g_armed.load()) VirtualProtect(reinterpret_cast<void*>(t_rearm), 0x1000, PAGE_READONLY, &old);
        t_rearm = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t a = r->ExceptionInformation[1];
    for (int k = 0; k < kPages; ++k) {
        const uint64_t page = g_watch_page[k].load();
        if (!page || (a & ~0xfffull) != page) continue;
        const uint64_t at = g_watch_at[k].load();
        if (a >= at && a < at + 2) {
            const int i = g_nhits.fetch_add(1);
            if (i < kMaxHits) {
                ::Ctx* c = kernel::current_ctx();
                g_hits[i] = {e->ContextRecord->Rip, a, c ? c->fn : 0, c ? c->x[30] : 0};
            }
        }
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_READWRITE, &old);
        t_rearm = page;
        e->ContextRecord->EFlags |= 0x100;   // trap after this one instruction, then re-protect
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void watch(uint64_t base, const std::vector<uint64_t>& boxes) {
    const uint64_t lo = mem::kHeapBase, hi = mem::kHeapBase + mem::heap_reserved();
    std::vector<uint32_t> last(boxes.size(), 0);
    std::vector<int> changes(boxes.size(), 0);
    std::vector<std::set<uint64_t>> seen(boxes.size());
    const double s0 = now_ms();
    while (now_ms() - s0 < 10000) {
        for (size_t i = 0; i < boxes.size(); ++i) {
            uint64_t b = 0;
            std::memcpy(&b, reinterpret_cast<const void*>(boxes[i] + 0x148), 8);
            if (b < lo || b >= hi) continue;
            if (seen[i].size() < 16) seen[i].insert(b);
            uint32_t v;
            std::memcpy(&v, reinterpret_cast<const void*>(b + 0x98), 4);
            if (v != last[i]) { changes[i]++; last[i] = v; }
        }
        Sleep(2);
    }
    size_t best = 0;
    for (size_t i = 0; i < boxes.size(); ++i) if (changes[i] > changes[best]) best = i;
    if (boxes.empty() || changes[best] < 5) {
        NXLOG_I(kTag, "watch: no TextBox buffer count changed (best %d)", boxes.empty() ? 0 : changes[best]);
        return;
    }
    NXLOG_I(kTag, "watch: TextBox 0x%llx: count changed %d times, %zu buffers", static_cast<unsigned long long>(boxes[best]), changes[best],
            seen[best].size());
    int k = 0;
    for (uint64_t b : seen[best]) {
        if (k >= kPages) break;
        g_watch_at[k] = b + 0x98;
        g_watch_page[k] = (b + 0x98) & ~0xfffull;
        NXLOG_I(kTag, "watch: buffer 0x%llx, count at 0x%llx", static_cast<unsigned long long>(b), static_cast<unsigned long long>(b + 0x98));
        ++k;
    }
    void* h = AddVectoredExceptionHandler(1, &watch_veh);
    const double w0 = now_ms();
    int fails = 0, protects = 0;
    while (now_ms() - w0 < 8000 && g_nhits.load() < kMaxHits) {
        for (int j = 0; j < k; ++j) {
            DWORD old;
            if (VirtualProtect(reinterpret_cast<void*>(g_watch_page[j].load()), 0x1000, PAGE_READONLY, &old)) ++protects; else ++fails;
        }
        Sleep(30);
    }
    NXLOG_I(kTag, "watch: %d protects, %d failed (error %lu)", protects, fails, GetLastError());
    for (int j = 0; j < k; ++j) {
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(g_watch_page[j].load()), 0x1000, PAGE_READWRITE, &old);
        g_watch_page[j] = 0;
    }
    Sleep(50);
    RemoveVectoredExceptionHandler(h);
    const int n = std::min(g_nhits.load(), kMaxHits);
    for (int i = 0; i < n; ++i)
        NXLOG_I(kTag, "watch hit %d: write 0x%llx by %s, guest fn 0x%llx, x30 0x%llx", i, static_cast<unsigned long long>(g_hits[i].addr),
                symbols_describe(g_hits[i].rip).c_str(), static_cast<unsigned long long>(g_hits[i].fn), static_cast<unsigned long long>(g_hits[i].lr));
    NXLOG_I(kTag, "watch: %d writes", g_nhits.load());
}


// Watch mode 2 ([fefw] tw_probe_watch = 2, needs [nvn] draw_bt = 1): the TextBox and buffer come from the text draw itself.
void watch_from_draw(double at_s) {
    const double t0 = now_ms();
    while (now_ms() - t0 < at_s * 1000.0) Sleep(20);
    std::set<uint64_t> bufs;
    uint64_t tb = 0;
    int last = -1, changes = 0;
    const double s0 = now_ms();
    while (now_ms() - s0 < 20000 && (changes < 4 || bufs.size() < 2)) {
        uint64_t t1, b1;
        int n1;
        nvn::debug_text_draw(&t1, &b1, &n1);
        if (b1 && n1 != last) {
            last = n1;
            ++changes;
            tb = t1;
            if (bufs.size() < 4) bufs.insert(b1);
        }
        Sleep(1);
    }
    if (bufs.empty()) {
        NXLOG_I(kTag, "watch: no text draw seen");
        return;
    }
    uint64_t vt = 0;
    std::memcpy(&vt, reinterpret_cast<const void*>(tb), 8);
    NXLOG_I(kTag, "watch: TextBox 0x%llx (vtable 0x%llx), %zu buffers, %d count changes seen", static_cast<unsigned long long>(tb),
            static_cast<unsigned long long>(vt), bufs.size(), changes);
    int k = 0;
    const uint64_t off = static_cast<uint64_t>(config_get_int("fefw", "tw_probe_off", 0x112));   // in the TextBox: the text length (u16)
    g_watch_at[k] = tb + off;
    g_watch_page[k] = (tb + off) & ~0xfffull;
    NXLOG_I(kTag, "watch: TextBox 0x%llx + 0x%llx", static_cast<unsigned long long>(tb), static_cast<unsigned long long>(off));
    ++k;
    void* h = AddVectoredExceptionHandler(1, &watch_veh);
    const double w0 = now_ms();
    int fails = 0, protects = 0;
    g_armed = true;
    for (int j = 0; j < k; ++j) {
        DWORD old;
        if (VirtualProtect(reinterpret_cast<void*>(g_watch_page[j].load()), 0x1000, PAGE_READONLY, &old)) ++protects; else ++fails;
    }
    while (now_ms() - w0 < 3000 && g_nhits.load() < kMaxHits) Sleep(10);
    g_armed = false;
    NXLOG_I(kTag, "watch: %d protects, %d failed (error %lu)", protects, fails, GetLastError());
    for (int j = 0; j < k; ++j) {
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(g_watch_page[j].load()), 0x1000, PAGE_READWRITE, &old);
        g_watch_page[j] = 0;
    }
    Sleep(50);
    RemoveVectoredExceptionHandler(h);
    const int n = std::min(g_nhits.load(), kMaxHits);
    for (int i = 0; i < n; ++i)
        NXLOG_I(kTag, "watch hit %d: write 0x%llx by %s, guest fn 0x%llx, x30 0x%llx", i, static_cast<unsigned long long>(g_hits[i].addr),
                symbols_describe(g_hits[i].rip).c_str(), static_cast<unsigned long long>(g_hits[i].fn), static_cast<unsigned long long>(g_hits[i].lr));
    NXLOG_I(kTag, "watch: %d writes", g_nhits.load());
}

void probe(uint64_t base, double at_s, double dur_ms, std::vector<uint64_t> kVtables) {
    const double t0 = now_ms();
    while (now_ms() - t0 < at_s * 1000.0) Sleep(20);
    std::sort(kVtables.begin(), kVtables.end());
    const uint64_t vlo = kVtables.front(), vhi = kVtables.back();
    std::vector<Obj> objs;
    std::vector<std::pair<uint64_t, uint64_t>> regions;
    const uint64_t lo = mem::kHeapBase, hi = mem::kHeapBase + mem::heap_reserved();
    uint64_t a = lo, scanned = 0;
    while (a < hi) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
        const uint64_t rb = reinterpret_cast<uint64_t>(mbi.BaseAddress), re = rb + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && readable(mbi.Protect)) {
            const uint64_t* p = reinterpret_cast<const uint64_t*>(rb);
            const size_t n = mbi.RegionSize / 8;
            for (size_t i = 0; i < n; ++i) {
                const uint64_t v = p[i] - base;
                if (v < vlo || v > vhi || !std::binary_search(kVtables.begin(), kVtables.end(), v) || rb + i * 8 + kObjBytes > re) continue;
                objs.push_back({rb + i * 8, v, kObjBytes / 4, ~size_t(0), {}, {}, {}, {}, {}, {}, {}});
            }
            scanned += mbi.RegionSize;
            regions.push_back({rb, re});
        }
        a = re > a ? re : a + 0x1000;
    }
    NXLOG_I(kTag, "heap scanned (%.0f MiB in %.0f ms): %zu objects", static_cast<double>(scanned) / 1048576.0, now_ms() - t0 - at_s * 1000.0,
            objs.size());
    if (objs.size() > 4000) objs.resize(4000);
    if (config_get_int("fefw", "tw_probe_watch", 0) == 1) {
        std::vector<uint64_t> boxes;
        for (const Obj& o : objs) if (o.vt == 0x1fc9990) boxes.push_back(o.addr);
        watch(base, boxes);
        return;
    }
    std::set<uint64_t> seen;
    const size_t parents = objs.size();
    for (size_t k = 0; k < parents; ++k) {   // one level down: what the objects point to in the heap (0x100 bytes each)
        const uint64_t* q = reinterpret_cast<const uint64_t*>(objs[k].addr);
        for (size_t i = 1; i < kObjBytes / 8; ++i) {
            const uint64_t v = q[i];
            if (v < lo || v >= hi || (v & 7) || !seen.insert(v).second) continue;
            auto it = std::upper_bound(regions.begin(), regions.end(), std::make_pair(v, ~0ull));
            if (it == regions.begin() || v + 0x400 > std::prev(it)->second || v < std::prev(it)->first) continue;
            objs.push_back({v, objs[k].addr + i * 8, 0x400 / 4, k, {}, {}, {}, {}, {}, {}, {}});
        }
    }
    NXLOG_I(kTag, "%zu objects and %zu pointed-to blocks", parents, objs.size() - parents);
    for (Obj& o : objs) {
        const size_t W = o.words;
        o.last.assign(reinterpret_cast<const uint32_t*>(o.addr), reinterpret_cast<const uint32_t*>(o.addr) + W);
        o.first = o.last;
        o.ups.assign(W, 0); o.downs.assign(W, 0); o.changes.assign(W, 0); o.t_first.assign(W, 0); o.t_last.assign(W, 0);
    }
    const double s0 = now_ms();
    while (now_ms() - s0 < dur_ms) {
        const double t = now_ms() - s0;
        for (Obj& o : objs) {
            const uint32_t* cur = reinterpret_cast<const uint32_t*>(o.addr);
            for (size_t w = 0; w < o.words; ++w) {
                const uint32_t v = cur[w];
                if (v == o.last[w]) continue;
                const int32_t d = static_cast<int32_t>(v) - static_cast<int32_t>(o.last[w]);
                (d > 0 ? o.ups : o.downs)[w]++;
                if (!o.changes[w]++) o.t_first[w] = t;
                o.t_last[w] = t;
                o.last[w] = v;
            }
        }
        Sleep(2);
    }
    int logged = 0;
    for (const Obj& o : objs) {
        bool told = false;
        for (size_t w = 0; w < o.words; ++w) {
            const int ch = o.changes[w];
            if (ch < 3) continue;
            if (o.ups[w] && o.downs[w] && o.ups[w] < 3 * o.downs[w] && o.downs[w] < 3 * o.ups[w]) continue;
            float f0, f1;
            std::memcpy(&f0, &o.first[w], 4);
            std::memcpy(&f1, &o.last[w], 4);
            NXLOG_I(kTag, "obj 0x%llx (vt 0x%llx) +0x%zx: %d changes (%d up, %d down) %u -> %u (f %.3f -> %.3f) at %.0f..%.0f ms",
                    static_cast<unsigned long long>(o.addr), static_cast<unsigned long long>(o.vt), w * 4, ch, o.ups[w], o.downs[w],
                    o.first[w], o.last[w], static_cast<double>(f0), static_cast<double>(f1), o.t_first[w], o.t_last[w]);
            if (!told && o.parent != ~size_t(0)) {
                told = true;
                const Obj& par = objs[o.parent];
                NXLOG_I(kTag, "  (block 0x%llx is pointed to by parent 0x%llx vt 0x%llx at +0x%llx)", static_cast<unsigned long long>(o.addr),
                        static_cast<unsigned long long>(par.addr), static_cast<unsigned long long>(par.vt),
                        static_cast<unsigned long long>(o.vt - par.addr));
            }
            if (++logged > 3000) return;
        }
    }
    NXLOG_I(kTag, "done");
}


// [fefw] tw_hooks = 1 (debug, needs the entry sites 0x59c320 and 0x568880 in main and [nvn] draw_bt = 1): per call of the TextBox update
// for the dialogue TextBox (the one the text draw last published), the words of the TextBox (0..0x160) and of the two blocks at
// [[TextBox + 0x138]] that changed since its last call; per call of the 16-bit print loop from that update, its arguments.
struct HookState {
    uint64_t tb = 0;
    uint32_t snap[0x160 / 4] = {};
    uint32_t snap_a[0x40 / 4] = {}, snap_b[0x40 / 4] = {};
    uint64_t pa = 0, pb = 0;
    int lines = 0;
    uint64_t last_update_obj = 0;
};
HookState g_hs;

bool heap_ptr(uint64_t v) { return v >= mem::kHeapBase && v < mem::kHeapBase + mem::heap_reserved() && !(v & 3); }

int hook_textbox_update(::Ctx* c, uint64_t, void*) {
    HookState& h = g_hs;
    h.last_update_obj = c->x[0];
    uint64_t tb, buf;
    int n;
    nvn::debug_text_draw(&tb, &buf, &n);
    if (!tb || c->x[0] != tb || h.lines > 600) return 0;
    const uint32_t* w = reinterpret_cast<const uint32_t*>(tb);
    char line[1024];
    int len = std::snprintf(line, sizeof line, "%.1f upd:", now_ms());
    for (int i = 0; i < 0x160 / 4 && len < 900; ++i)
        if (w[i] != h.snap[i] || h.tb != tb) {
            if (h.tb == tb) len += std::snprintf(line + len, sizeof line - len, " +%x=%x", i * 4, w[i]);
            h.snap[i] = w[i];
        }
    uint64_t p = 0;
    std::memcpy(&p, reinterpret_cast<const void*>(tb + 0x138), 8);
    if (heap_ptr(p)) {
        uint64_t pa = 0, pb = 0;
        std::memcpy(&pa, reinterpret_cast<const void*>(p), 8);
        std::memcpy(&pb, reinterpret_cast<const void*>(p + 8), 8);
        const uint64_t ps[2] = {pa, pb};
        uint32_t* snaps[2] = {h.snap_a, h.snap_b};
        uint64_t* olds[2] = {&h.pa, &h.pb};
        for (int k = 0; k < 2; ++k) {
            if (!heap_ptr(ps[k])) continue;
            const uint32_t* q = reinterpret_cast<const uint32_t*>(ps[k]);
            for (int i = 0; i < 0x40 / 4 && len < 980; ++i)
                if (q[i] != snaps[k][i] || *olds[k] != ps[k]) {
                    if (*olds[k] == ps[k]) len += std::snprintf(line + len, sizeof line - len, " %c+%x=%x", 'a' + k, i * 4, q[i]);
                    snaps[k][i] = q[i];
                }
            *olds[k] = ps[k];
        }
    }
    h.tb = tb;
    NXLOG_I(kTag, "%s", line);
    ++h.lines;
    return 0;
}

int hook_print16(::Ctx* c, uint64_t, void*) {
    HookState& h = g_hs;
    if (!h.tb || h.last_update_obj != h.tb || h.lines > 600) return 0;
    float f4 = 0;
    if (heap_ptr(c->x[4])) std::memcpy(&f4, reinterpret_cast<const void*>(c->x[4]), 4);
    NXLOG_I(kTag, "%.1f print16: len %u w3 %u x4 %llx (%f) x5 %llx", now_ms(), static_cast<uint32_t>(c->x[2]), static_cast<uint32_t>(c->x[3]),
            static_cast<unsigned long long>(c->x[4]), static_cast<double>(f4), static_cast<unsigned long long>(c->x[5]));
    ++h.lines;
    return 0;
}

// Print-loop callers ([fefw] tw_hooks = 2, entry sites 0x568880 and 0x566e80): calls and characters per second by caller chain.
struct CallerStat {
    uint64_t l1, l2, l3;
    uint64_t calls, chars;
    uint32_t last_len;
};
std::mutex g_cs_mu;
std::vector<CallerStat> g_cs;

int hook_print_any(::Ctx* c, uint64_t, void*) {
    uint64_t l2 = 0, l3 = 0, fp = c->x[29];
    if (heap_ptr(fp) || (fp >> 32) == 0x7f || fp) {
        uint64_t next = 0;
        if (fp && !(fp & 7)) {
            std::memcpy(&l2, reinterpret_cast<const void*>(fp + 8), 8);
            std::memcpy(&next, reinterpret_cast<const void*>(fp), 8);
            if (next > fp && !(next & 7)) std::memcpy(&l3, reinterpret_cast<const void*>(next + 8), 8);
        }
    }
    static std::atomic<int> dumps{0};
    if (static_cast<uint32_t>(c->x[2]) >= 60 && dumps.load() < 150 && c->x[0]) {
        dumps.fetch_add(1);
        char line[900];
        int n = std::snprintf(line, sizeof line, "%.1f writer len %u x3 %u:", now_ms(), static_cast<uint32_t>(c->x[2]), static_cast<uint32_t>(c->x[3]));
        uint64_t tp = 0;
        std::memcpy(&tp, reinterpret_cast<const void*>(c->x[0] + 0x58), 8);
        if (heap_ptr(tp)) {
            const uint32_t* q = reinterpret_cast<const uint32_t*>(tp);
            n += std::snprintf(line + n, sizeof line - n, " | tagproc %llx:", static_cast<unsigned long long>(tp));
            for (int i = 0; i < 0x80 / 4 && n < 880; ++i) n += std::snprintf(line + n, sizeof line - n, " %x", q[i]);
        }
        NXLOG_I(kTag, "%s", line);
    }
    std::lock_guard<std::mutex> lk(g_cs_mu);
    for (CallerStat& s : g_cs)
        if (s.l1 == c->x[30] && s.l2 == l2 && s.l3 == l3) {
            s.calls++;
            s.chars += static_cast<uint32_t>(c->x[2]);
            s.last_len = static_cast<uint32_t>(c->x[2]);
            return 0;
        }
    if (g_cs.size() < 200) g_cs.push_back({c->x[30], l2, l3, 1, static_cast<uint32_t>(c->x[2]), static_cast<uint32_t>(c->x[2])});
    return 0;
}

void caller_report() {
    for (int r = 0; r < 120; ++r) {
        Sleep(1000);
        std::vector<CallerStat> v;
        {
            std::lock_guard<std::mutex> lk(g_cs_mu);
            v.swap(g_cs);
        }
        for (const CallerStat& s : v)
            NXLOG_I(kTag, "print callers: %llx < %llx < %llx: %llu calls, %llu chars, last len %u", static_cast<unsigned long long>(s.l1),
                    static_cast<unsigned long long>(s.l2), static_cast<unsigned long long>(s.l3), static_cast<unsigned long long>(s.calls),
                    static_cast<unsigned long long>(s.chars), s.last_len);
    }
}
}  // namespace

void twprobe_start(uint64_t base) {
    if (config_get_int("fefw", "tw_hooks", 0) == 2) {
        hook_add_entry(base + 0x568880, &hook_print_any);
        hook_add_entry(base + 0x566e80, &hook_print_any);
        std::thread([] { Sleep(55000); caller_report(); }).detach();
        NXLOG_I(kTag, "print-caller hooks on");
    } else if (config_get_int("fefw", "tw_hooks", 0) == 1) {
        hook_add_entry(base + 0x59c320, &hook_textbox_update);
        hook_add_entry(base + 0x568880, &hook_print16);
        NXLOG_I(kTag, "typewriter hooks on");
    }
    const double at = config_get_double("fefw", "tw_probe", 0.0);
    if (!(at > 0)) return;
    const double ms = config_get_double("fefw", "tw_probe_ms", 4000.0);
    std::vector<uint64_t> vts;
    const std::string list = config_get_str("fefw", "tw_probe_vt", "0x1fc9990,0x213d4f8");   // nn::ui2d::TextBox, ui::control::TextBoxEx
    for (size_t i = 0; i < list.size();) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        vts.push_back(std::strtoull(list.substr(i, j - i).c_str(), nullptr, 16));
        i = j + 1;
    }
    if (config_get_int("fefw", "tw_probe_watch", 0) == 2) std::thread(watch_from_draw, at).detach();
    else std::thread(probe, base, at, ms, vts).detach();
    NXLOG_I(kTag, "typewriter probe at %.1f s for %.0f ms", at, ms);
}

}  // namespace nxrt::games::ObeliskChamber
