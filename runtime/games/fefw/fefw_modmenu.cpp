// SPDX-License-Identifier: GPL-3.0-or-later
// Fire Emblem: Fortune's Weave: the title screen's DLC entry as a mod menu ([fefw] mod_menu, default on).
//
// The title's eShop entry (class vtable 0x20c46d8 in main of update 131072: GetMid 0x16d7320 = "MID_Title_eShop", ACall 0x16d7330 = a
// prompt, then the shop applet, which a PC has none of) reads "DLC / Mods" and opens a list of the mods folder made from the game's own
// Options screen class (fefw_optmenu.cpp's pick list), on two tabs:
//   RomFS  the mods that have romfs/ (files laid over the game's)
//   ExeFS  the mods that have exefs/ (code: this port's DLLs, and the .ips / .pchtxt patches Set up builds in)
// A row is a mod, named by its folder; its value is On or Off and A flips it. The switch is the mod's own mod.ini
// (services::mods_set: "romfs = on|off", "exefs = on|off"), read where mods are used: the RomFS overlay and the DLL loader at the next
// start, Set up's exefs patches at the next Set up.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fefw_game.h"
#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/services.h"

namespace nxrt::games::fefw {
namespace {
constexpr const char* kTag = "fefw.modmenu";
constexpr uint64_t kVtShop = 0x20c46d8, kGetText = 0x1718dc0, kShopA = 0x16d7330;
constexpr int kSlotText = 2, kSlotACall = 25;
constexpr uint32_t kOpened = 9, kRefused = 1, kRedraw = 13, kNothing = 0;
const char kLabel[] = "DLC / Mods";

uint64_t g_base = 0;
bool g_on = true, g_hooked = false, g_failed = false;

// The list being shown (main thread): the RomFS tab's rows, then the ExeFS tab's. A tab without mods has one row that says so.
struct Row {
    int mod = -1;        // index into g_mods, -1: the "no mods" row
    bool exefs = false;
};
std::vector<services::ModInfo> g_mods;
std::vector<Row> g_rows;

template <class T> bool rd(uint64_t a, T* out) {
    if (!a || !mem::is_mapped(a, sizeof(T))) return false;
    std::memcpy(out, reinterpret_cast<const void*>(a), sizeof(T));
    return true;
}
template <class T> void wr(uint64_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T)); }

const Row* row(int i) { return i >= 0 && i < static_cast<int>(g_rows.size()) ? &g_rows[static_cast<size_t>(i)] : nullptr; }
// The mod's own folder name (the folder that holds romfs/ or exefs/).
std::string leaf(const std::string& name) {
    const size_t at = name.find_last_of('/');
    return at == std::string::npos ? name : name.substr(at + 1);
}
int row_page(int i) {
    const Row* r = row(i);
    return r && r->exefs ? 1 : 0;
}
void row_label(int i, char* out, size_t n) {
    const Row* r = row(i);
    if (!r) return;
    if (r->mod < 0) std::snprintf(out, n, "No mods found");
    else std::snprintf(out, n, "%.60s", leaf(g_mods[static_cast<size_t>(r->mod)].name).c_str());
}
void row_value(int i, char* out, size_t n) {
    const Row* r = row(i);
    if (!r || r->mod < 0) return;
    const services::ModInfo& m = g_mods[static_cast<size_t>(r->mod)];
    std::snprintf(out, n, "%s", (r->exefs ? m.exefs_on : m.romfs_on) ? "On" : "Off");
}
void row_help(int i, char* out, size_t n) {
    const Row* r = row(i);
    if (!r) return;
    if (r->mod < 0 && r->exefs) std::snprintf(out, n, "Code mods go in mods/<name>/exefs: DLLs made for this port, or .ips / .pchtxt patches.");
    else if (r->mod < 0) std::snprintf(out, n, "File mods go in mods/<name>/romfs, next to the game.");
    else if (r->exefs) std::snprintf(out, n, "A: on / off. DLLs change at the next start; .ips and .pchtxt patches at the next Set up.");
    else std::snprintf(out, n, "A: on / off. The mod's files replace the game's. Applies at the next start.");
}
uint32_t row_pick(::Ctx*, int i) {
    const Row* r = row(i);
    if (!r || r->mod < 0) return kNothing;
    services::ModInfo& m = g_mods[static_cast<size_t>(r->mod)];
    bool romfs_on = m.romfs_on, exefs_on = m.exefs_on;
    (r->exefs ? exefs_on : romfs_on) = !(r->exefs ? exefs_on : romfs_on);
    std::string err;
    if (!services::mods_set(m.dir, romfs_on, exefs_on, &err)) {
        NXLOG_W(kTag, "%s: %s", m.name.c_str(), err.c_str());
        return kRefused;
    }
    m.romfs_on = romfs_on;
    m.exefs_on = exefs_on;
    NXLOG_I(kTag, "%s: %s %s (mod.ini)", m.name.c_str(), r->exefs ? "exefs" : "romfs", (r->exefs ? exefs_on : romfs_on) ? "on" : "off");
    return kRedraw;
}

void shop_text(::Ctx* c) { c->x[0] = reinterpret_cast<uint64_t>(kLabel); }
void shop_acall(::Ctx* c) {
    const uint64_t item = c->x[0];
    g_mods = services::mods_list();
    g_rows.clear();
    for (int pass = 0; pass < 2; ++pass) {
        const size_t before = g_rows.size();
        for (size_t i = 0; i < g_mods.size(); ++i)
            if (pass == 0 ? g_mods[i].has_romfs : g_mods[i].has_exefs) g_rows.push_back({static_cast<int>(i), pass == 1});
        if (g_rows.size() == before) g_rows.push_back({-1, pass == 1});
    }
    OptList list;
    list.pages = 2;
    list.titles[0] = "RomFS";
    list.titles[1] = "ExeFS";
    list.count = static_cast<int>(g_rows.size());
    list.page = row_page;
    list.label = row_label;
    list.value = row_value;
    list.help = row_help;
    list.pick = row_pick;
    const bool opened = optmenu_open_list(c, item, list);
    if (opened) NXLOG_I(kTag, "mod menu opened: %zu mod(s)", g_mods.size());
    else NXLOG_W(kTag, "the mod menu could not be opened (the Options screen hook is not available)");
    c->x[0] = opened ? kOpened : kRefused;
}
bool slot_is(int slot, uint64_t fn) {
    uint64_t v = 0;
    return rd(g_base + kVtShop + static_cast<uint64_t>(slot) * 8, &v) && v == g_base + fn;
}
void patch(int slot, uint64_t thunk) {
    const uint64_t at = g_base + kVtShop + static_cast<uint64_t>(slot) * 8;
    mem::protect(at & ~0xfffull, 0x1000, mem::Prot::RW);
    wr<uint64_t>(at, thunk);
}
}  // namespace

void modmenu_install(uint64_t base) {
    g_base = base;
    g_on = config_get_bool("fefw", "mod_menu", true);
}

// Main thread, once per frame: hooks the entry once the game's data is there and is the build the addresses are for.
void modmenu_frame() {
    if (g_hooked || g_failed || !g_on || !g_base) return;
    uint64_t probe = 0;
    if (!rd(g_base + kVtShop + kSlotACall * 8, &probe) || !probe) return;
    if (!slot_is(kSlotText, kGetText) || !slot_is(kSlotACall, kShopA)) {
        g_failed = true;
        NXLOG_W(kTag, "this build's title DLC entry is not the one this was written for: it is left as it is");
        return;
    }
    patch(kSlotText, hle_register("fefw.modmenu.text", shop_text));
    patch(kSlotACall, hle_register("fefw.modmenu.acall", shop_acall));
    g_hooked = true;
    NXLOG_I(kTag, "the title's DLC entry opens the mod menu ([fefw] mod_menu)");
}
}  // namespace nxrt::games::fefw
