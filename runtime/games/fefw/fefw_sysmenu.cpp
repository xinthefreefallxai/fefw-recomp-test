// SPDX-License-Identifier: GPL-3.0-or-later
// Fire Emblem: Fortune's Weave: a "Recomp Options" entry in the game's own + menu, under System, that opens the F1 settings menu
// ([fefw] system_menu_entry, default on). No lifted-code site is needed: everything goes through the game's vtables.
//
// The game's menus (main of update 131072; names after the community decompilation's, addresses are this build's):
//   game::menu::BasicMenuItem      vtable slots: 0/1 destructors, 2 GetText() -> const char16_t*, 3 GetMid(), 25 ACall() -> int, 41 PlayDecideSe()
//   SystemMenuItem_System::ACall   0x188b930  makes the "SubMenu" (0x171fa20) under the menu's root, then for each entry allocates the item
//                                             (HeapManager::Allocate 0x652310, heap = *[GOT 0x2169fa8]), fills it and calls
//                                             BasicMenu::AddItem (0x1714cc0): Config, MouseGuide, Restart, RestartBeforeBattle, ExitTitle.
//                                             Returns 10 when it opened the sub menu.
//   vtables                        System 0x2127818, Config 0x2128c88, MouseGuide 0x2128ee8, ExitTitle 0x2129828 (56 slots each)
//   an item of 0x90 bytes          +0 vtable, +8 its menu (set by AddItem), +0x10.. zero, +0x50 = 1, +0x51 = 1 (shown), +0x52 = 1 (ready)
//   process tree (0x657fd0)        a new process becomes its parent's +0x28 (the newest child); BasicMenu's items: vectors at +0xe8 and +0x100
//   ACall results (PlayACallSe)    1 = refused (the "NG" sound), 2 = decided, 9 / 10 = opened something; 0 = nothing, no sound
//
// So: slot 25 of the System item's vtable points at a host thunk that runs the game's function and then adds one more item to the sub
// menu it made. That item is the MouseGuide item's shape with a vtable of its own (a copy with GetText and ACall replaced by thunks):
// its text is ours (no message file needed) and A opens the overlay, which takes the pad while it is up (gfx/overlay.cpp).
#include <atomic>
#include <cstdint>
#include <cstring>

#include "fefw_game.h"
#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/play.h"

namespace nxrt::games::fefw {
namespace {
constexpr const char* kTag = "fefw.sysmenu";
constexpr uint64_t kSystemACall = 0x188b930, kAllocate = 0x652310, kAddItem = 0x1714cc0, kHeapGot = 0x2169fa8;
constexpr uint64_t kVtSystem = 0x2127818, kVtMouseGuide = 0x2128ee8, kVtExitTitle = 0x2129828;
constexpr int kSlots = 56, kSlotGetText = 2, kSlotACall = 25, kSlotDecideSe = 41;
constexpr uint64_t kItemBytes = 0x90, kItemMenu = 0x8, kItemReady = 0x52;
constexpr uint64_t kMenuRoot = 0x98, kProcChild = 0x28, kMenuItems = 0xe8, kMenuItems2 = 0x100;
constexpr uint32_t kOpenedSubMenu = 10;

uint64_t g_base = 0;
bool g_on = true, g_done = false, g_failed = false;
uint64_t g_vt[2 + kSlots] = {};          // the entry's vtable: offset-to-top, type info, then the slots (a copy of MouseGuide's)
uint64_t g_thunk_system = 0;
std::atomic<uint64_t> g_added{0}, g_opened{0};
const char16_t kLabel[] = u"Recomp Options";

template <class T> bool rd(uint64_t a, T* out) {
    if (!a || !mem::is_mapped(a, sizeof(T))) return false;
    std::memcpy(out, reinterpret_cast<const void*>(a), sizeof(T));
    return true;
}
template <class T> void wr(uint64_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T)); }

// fn(a0..a3) on the guest thread; the registers are put back, the result is returned.
uint64_t call(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0) {
    const ::Ctx saved = *c;
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[3] = a3;
    dispatch_call(c, fn);
    const uint64_t r = c->x[0];
    *c = saved;
    return r;
}

// Moves the last element of the pointer vector at `vec` (begin, end) to index `to`, when it is `item`.
void move_last(uint64_t vec, uint64_t item, uint64_t to) {
    uint64_t b = 0, e = 0, last = 0;
    if (!rd(vec, &b) || !rd(vec + 8, &e) || e <= b || ((e - b) & 7)) return;
    const uint64_t n = (e - b) / 8;
    if (n < to + 2 || !mem::is_mapped(b, e - b) || !rd(e - 8, &last) || last != item) return;
    uint64_t* p = reinterpret_cast<uint64_t*>(b);
    std::memmove(p + to + 1, p + to, (n - 1 - to) * 8);
    p[to] = item;
}

void add_entry(::Ctx* c, uint64_t system_item) {
    uint64_t menu = 0, root = 0, sub = 0, heap_var = 0, heap = 0, ib = 0, ie = 0, last = 0, last_vt = 0;
    if (!rd(system_item + kItemMenu, &menu) || !menu) return;
    if (rd(menu + kMenuRoot, &root) && root) menu = root;
    // the sub menu just made is the root's newest child; its last item is the game's last entry
    if (!rd(menu + kProcChild, &sub) || !sub || !rd(sub + kMenuItems, &ib) || !rd(sub + kMenuItems + 8, &ie) || ie <= ib || !rd(ie - 8, &last) ||
        !rd(last, &last_vt) || last_vt != g_base + kVtExitTitle) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) NXLOG_W(kTag, "the System sub menu is not where it was expected: no Recomp Options entry");
        return;
    }
    if (!rd(g_base + kHeapGot, &heap_var) || !rd(heap_var, &heap) || !heap) return;
    const uint64_t item = call(c, g_base + kAllocate, heap, kItemBytes, 16, 0);
    if (!item || !mem::is_mapped(item, kItemBytes)) return;
    std::memset(reinterpret_cast<void*>(item), 0, kItemBytes);
    wr<uint64_t>(item, reinterpret_cast<uint64_t>(&g_vt[2]));
    wr<uint16_t>(item + 0x50, 0x0101);
    wr<uint8_t>(item + kItemReady, 1);
    call(c, g_base + kAddItem, sub, item);
    // straight after the game's own options entry (the first one)
    move_last(sub + kMenuItems, item, 1);
    move_last(sub + kMenuItems2, item, 1);
    if (g_added.fetch_add(1) == 0) NXLOG_I(kTag, "Recomp Options added to the System menu");
}

// SystemMenuItem_System::ACall: the game's, then our entry
void system_acall(::Ctx* c) {
    const uint64_t item = c->x[0];
    const uint64_t lr = c->x[30], fn = c->fn;
    dispatch_call(c, g_base + kSystemACall);
    c->x[30] = lr;
    c->fn = fn;
    const uint64_t ret = c->x[0];
    if (static_cast<uint32_t>(ret) == kOpenedSubMenu && g_on) add_entry(c, item);
    c->x[0] = ret;
}

void entry_text(::Ctx* c) { c->x[0] = reinterpret_cast<uint64_t>(kLabel); }

void entry_acall(::Ctx* c) {
    const uint64_t item = c->x[0];
    uint8_t ready = 0;
    if (!rd(item + kItemReady, &ready) || ready != 1) {
        c->x[0] = 1;
        return;
    }
    call(c, g_vt[2 + kSlotDecideSe], item);
    play::open_settings_menu();
    g_opened.fetch_add(1);
    c->x[0] = 0;      // nothing for the game's menu to do: it stays as it is under the overlay
}
}  // namespace

void sysmenu_install(uint64_t base) {
    g_base = base;
    g_on = config_get_bool("fefw", "system_menu_entry", true);
}

// Main thread, once per frame: patches the vtable once the game's data is there (the modules are not loaded at install).
void sysmenu_frame() {
    if (g_done || g_failed || !g_on || !g_base) return;
    const uint64_t slot = g_base + kVtSystem + kSlotACall * 8;
    uint64_t cur = 0, text = 0;
    if (!rd(slot, &cur) || !cur) return;
    if (cur != g_base + kSystemACall || !mem::is_mapped(g_base + kVtMouseGuide - 16, sizeof g_vt) ||
        !rd(g_base + kVtMouseGuide + kSlotGetText * 8, &text) || !fn_lookup(g_base + kSystemACall)) {
        g_failed = true;
        NXLOG_W(kTag, "this build's System menu is not the one this was written for (slot 0x%llx): no Recomp Options entry", static_cast<unsigned long long>(cur));
        return;
    }
    std::memcpy(g_vt, reinterpret_cast<const void*>(g_base + kVtMouseGuide - 16), sizeof g_vt);
    g_vt[2 + kSlotGetText] = hle_register("fefw.sysmenu.entry_text", entry_text);
    g_vt[2 + kSlotACall] = hle_register("fefw.sysmenu.entry_acall", entry_acall);
    g_thunk_system = hle_register("fefw.sysmenu.system_acall", system_acall);
    mem::protect(slot & ~0xfffull, 0x1000, mem::Prot::RW);
    wr<uint64_t>(slot, g_thunk_system);
    g_done = true;
    NXLOG_I(kTag, "System menu hooked: + > System gets a Recomp Options entry ([fefw] system_menu_entry)");
}
}  // namespace nxrt::games::fefw
