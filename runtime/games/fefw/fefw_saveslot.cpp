// SPDX-License-Identifier: GPL-3.0-or-later
// Fire Emblem: Fortune's Weave: save slots ([fefw] save_slots, default on; the slot in use is [fefw] save_slot).
//
// The game has one save: every path it builds is "000/<file>" (Auto, Global, Chapter_*, Route_*; 33 places in main), under the account
// save data. Here the save folder holds several such folders, "000", "001", ..., and the runtime's save path hook (services.h) hands the
// game the one in use whenever it asks for "000". The game's save and load code is untouched.
//
// The slot is chosen in the game's own entries (main of update 131072; names after the community decompilation's, addresses this build's):
//   title: Continue      vtable 0x20c2d78, ACall 0x16d4ae0 (asks, then loads "000/Auto")      left / right: the slot in use
//   title: New Game      vtable 0x20c2b08, ACall 0x16d4810                                    starts in the next free folder
//                        The title menu's builder (0x16d2d60) makes New Game selectable only without a save (item+0x51 = !exists,
//                        read through slot 6 alone) and Continue only with one. With slots a new game overwrites nothing, so
//                        New Game's slot 6 answers yes always.
//   title: Delete Save   vtable 0x20c41e8, ACall 0x16d6910 (Extra)                            left / right: the slot in use
//   + menu: Save         vtables 0x212ab80, 0x20144e8 (hub), 0x214c8c0; ACall 0x188b880 (the save prompt, 0x16da940(root))
//                        A opens a list (fefw_optmenu.cpp's pick list) with a Save tab (New Slot, then the slots: A on a row
//                        brings the game's own save prompt, and the save goes to that row's slot) and a Load tab (the slots: A on
//                        a row brings the game's own Continue prompt for that slot's save, and Yes loads it)
// The title's menus are single-column: left / right reach the current item's slots 39 / 40 (BasicMenuItem: (item, wrap) -> changed), and
// slot 55 redraws its text from slot 2 (GetText). The + menu is a grid (left / right move its cursor), hence the list there.
//
// Each row shows what the save's own header says (below): the chapter and the play time.
//
// Picking a slot for Save copies nothing: the change waits for the game's first write to a file of the save other than Global (the save
// it then makes), so a prompt that is cancelled changes nothing. Saving to another slot copies the folder in use over it (the old
// contents are kept as "<slot>.bak"). New Game moves to an empty folder at once, with only the Global file of the one in use.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <initializer_list>
#include <mutex>
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
namespace stdfs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr const char* kTag = "fefw.saveslot";
constexpr uint64_t kVtContinue = 0x20c2d78, kVtNewGame = 0x20c2b08, kVtDelete = 0x20c41e8, kVtContinueNew = 0x20c3168;
constexpr uint64_t kVtSave[] = {0x212ab80, 0x20144e8, 0x214c8c0};
constexpr uint64_t kGetText = 0x1718dc0, kContinueA = 0x16d4ae0, kNewGameA = 0x16d4810, kDeleteA = 0x16d6910, kSaveA = 0x188b880, kContinueNewA = 0x16d4ea0;
constexpr uint64_t kBaseNext = 0x874dc0, kBasePrev = 0x874dd0, kBaseDecidable = 0x875210;
constexpr int kSlotText = 2, kSlotDecidable = 6, kSlotACall = 25, kSlotNext = 39, kSlotPrev = 40, kSlotRedraw = 55;
constexpr uint32_t kOpened = 9;                       // what an entry answers when it opened a screen
constexpr auto kPrompt = std::chrono::minutes(30);    // and, once it was, for the save behind the game's prompt
enum Kind { kNone, kContinue, kNewGame, kDelete, kSave };
enum Pending { kPendNone, kPendSave };

uint64_t g_base = 0;
bool g_on = true, g_hooked = false, g_failed = false;

std::mutex g_mu;                 // everything below: the game's threads (the path hook) and its main thread (the menus)
std::string g_root;              // the account save's host folder (UTF-8), known from the first path the game asks for
std::string g_active = "000";    // the folder the game's "000" means
std::string g_before_new;        // the slot in use before New Game moved to an empty one (Continue goes back to it)
bool g_had_save = false;         // the slot in use was seen with a save: when it has none any more, it was deleted
Pending g_pending = kPendNone;   // what the game's next write to the save does first
std::string g_target;
Clock::time_point g_deadline;
Clock::time_point g_written_at{};       // the game's last write to the save
std::string g_load_slot;               // the slot whose save the game's Continue prompt is showing for the Load list
std::vector<std::string> g_cache;      // the slots, for the labels
Clock::time_point g_cache_at;
bool g_cache_ok = false;

template <class T> bool rd(uint64_t a, T* out) {
    if (!a || !mem::is_mapped(a, sizeof(T))) return false;
    std::memcpy(out, reinterpret_cast<const void*>(a), sizeof(T));
    return true;
}
template <class T> void wr(uint64_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T)); }
uint64_t call(::Ctx* c, uint64_t fn, std::initializer_list<uint64_t> args) {
    const ::Ctx saved = *c;
    int i = 0;
    for (uint64_t a : args) c->x[i++] = a;
    dispatch_call(c, fn);
    const uint64_t r = c->x[0];
    *c = saved;
    return r;
}
void pass(::Ctx* c, uint64_t fn) {
    const uint64_t lr = c->x[30], self = c->fn;
    dispatch_call(c, fn);
    c->x[30] = lr;
    c->fn = self;
}

std::string describe_by_files(const std::string& slot);

// ---- the folders (g_mu held) -------------------------------------------------------------------------------------------------------------
bool slot_name(const std::string& s) { return s.size() == 3 && std::all_of(s.begin(), s.end(), [](char ch) { return ch >= '0' && ch <= '9'; }); }
stdfs::path dir_of(const std::string& slot) { return stdfs::u8path(g_root) / slot; }
bool has_save(const std::string& slot) {
    std::error_code ec;
    return stdfs::is_regular_file(dir_of(slot) / "Auto", ec);
}
// The folders that hold a save (an Auto file), in order.
std::vector<std::string> scan() {
    std::vector<std::string> out;
    if (g_root.empty()) return out;
    std::error_code ec;
    for (stdfs::directory_iterator it(stdfs::u8path(g_root), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string name = it->path().filename().string();
        if (slot_name(name) && has_save(name)) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}
const std::vector<std::string>& slots() {
    const Clock::time_point now = Clock::now();
    if (!g_cache_ok || now - g_cache_at > std::chrono::milliseconds(500)) {
        g_cache = scan();
        g_cache_at = now;
        g_cache_ok = true;
    }
    return g_cache;
}
// The lowest folder number that holds no save.
std::string next_free() {
    for (int i = 0; i < 1000; ++i) {
        char b[8];
        std::snprintf(b, sizeof b, "%03d", i);
        if (!has_save(b)) return b;
    }
    return "999";
}
void set_active(const std::string& slot) {
    if (slot == g_active) return;
    g_active = slot;
    g_had_save = !g_root.empty() && has_save(slot);
    g_cache_ok = false;
    std::string err;
    if (!config_write("fefw", "save_slot", slot, &err)) NXLOG_W(kTag, "cannot write [fefw] save_slot: %s", err.c_str());
}
// A save file's header (not compressed; the same in Auto, Chapter_* and Route_*, read from the user's files): the magic 0xCDEFBCDD at 0,
// then at 0x28 the route's number, the part and the chapter (Chapter_Kai_1_6: 1, 1, 6; Chapter_Ism_0_2: 9, 0, 2), at 0x54 the play time
// (hours as 16 bits, minutes, seconds) and at 0x58 when it was saved (year as 16 bits, month, day, hour, minute, second).
struct SaveInfo {
    bool ok = false;
    int route = 0, part = 0, chapter = 0, hours = 0, minutes = 0, seconds = 0, year = 0, month = 0, day = 0, hour = 0, minute = 0;
};
SaveInfo read_info(const stdfs::path& file) {
    SaveInfo v;
    uint8_t b[0x60] = {};
    FILE* f =
#ifdef _WIN32
        _wfopen(file.wstring().c_str(), L"rb");
#else
        std::fopen(file.string().c_str(), "rb");
#endif
    if (!f) return v;
    const size_t n = std::fread(b, 1, sizeof b, f);
    std::fclose(f);
    if (n != sizeof b || b[0] != 0xdd || b[1] != 0xbc || b[2] != 0xef || b[3] != 0xcd) return v;
    v.route = b[0x28], v.part = b[0x29], v.chapter = b[0x2a];
    v.hours = b[0x54] | b[0x55] << 8, v.minutes = b[0x56], v.seconds = b[0x57];
    v.year = b[0x58] | b[0x59] << 8, v.month = b[0x5a], v.day = b[0x5b], v.hour = b[0x5c], v.minute = b[0x5d];
    v.ok = v.minutes < 60 && v.seconds < 60 && v.month >= 1 && v.month <= 12 && v.day >= 1 && v.day <= 31 && v.hour < 24 && v.minute < 60;
    return v;
}
// "Kai 1-6": the route's name is the one in the slot's chapter file for that part and chapter ("Chapter_Kai_1_6"); without one, "Ch. 1-6".
std::string chapter_name(const std::string& slot, const SaveInfo& v) {
    char want[24], buf[64];
    std::snprintf(want, sizeof want, "_%d_%d", v.part, v.chapter);
    std::error_code ec;
    for (stdfs::directory_iterator it(dir_of(slot), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind("Chapter_", 0) != 0 || name.size() <= 8 + std::strlen(want) || name.compare(name.size() - std::strlen(want), std::string::npos, want) != 0)
            continue;
        const std::string who = name.substr(8, name.size() - 8 - std::strlen(want));
        if (who.empty() || who.find('_') != std::string::npos) continue;
        std::snprintf(buf, sizeof buf, "%.20s %d-%d", who.c_str(), v.part, v.chapter);
        return buf;
    }
    std::snprintf(buf, sizeof buf, "Ch. %d-%d", v.part, v.chapter);
    return buf;
}
// A row's value, "Kai 1-6  10:19:23" (chapter, play time), and for the help line when it was saved, "2026-11-11 18:18".
std::string describe(const std::string& slot, std::string* saved = nullptr) {
    const SaveInfo v = read_info(dir_of(slot) / "Auto");
    if (v.ok) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s  %d:%02d:%02d", chapter_name(slot, v).c_str(), v.hours, v.minutes, v.seconds);
        if (saved) {
            char when[40];
            std::snprintf(when, sizeof when, "%04d-%02d-%02d %02d:%02d", v.year, v.month, v.day, v.hour, v.minute);
            *saved = when;
        }
        return buf;
    }
    return describe_by_files(slot);
}
// The same from the files' names and times, for a save whose header does not read as expected.
std::string describe_by_files(const std::string& slot) {
    std::error_code ec;
    std::string chapter;
    stdfs::file_time_type best{};
    for (stdfs::directory_iterator it(dir_of(slot), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind("Chapter_", 0) != 0) continue;
        const stdfs::file_time_type t = it->last_write_time(ec);
        if (!chapter.empty() && t <= best) continue;
        best = t;
        char who[24] = {};
        int a = 0, b = 0;
        if (std::sscanf(name.c_str(), "Chapter_%23[A-Za-z]_%d_%d", who, &a, &b) == 3) {
            char buf[48];
            std::snprintf(buf, sizeof buf, "%s %d-%d", who, a, b);
            chapter = buf;
        } else chapter = name.substr(8);
    }
    std::string out = chapter.empty() ? std::string("Saved") : chapter;
    const stdfs::file_time_type ft = stdfs::last_write_time(dir_of(slot) / "Auto", ec);
    if (!ec) {
        const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - stdfs::file_time_type::clock::now() + std::chrono::system_clock::now());
        const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &tt);
#else
        localtime_r(&tt, &tmv);
#endif
        char buf[32];
        std::snprintf(buf, sizeof buf, "  %d/%d %02d:%02d", tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
        out += buf;
    }
    return out;
}

// A slot without a save cannot be continued or deleted: after a New Game that was backed out of, the slot before it is in use again.
void back_to_saved() {
    if (g_root.empty() || has_save(g_active)) return;
    if (!g_before_new.empty() && has_save(g_before_new)) set_active(g_before_new);
    else if (const std::vector<std::string> all = scan(); !all.empty()) set_active(all.front());
}
// The slot Continue and Delete Save stand for.
std::string loadable() {
    if (has_save(g_active)) return g_active;
    if (!g_before_new.empty() && has_save(g_before_new)) return g_before_new;
    const std::vector<std::string>& all = slots();
    return all.empty() ? g_active : all.front();
}

// What a slot picked for Save waits for: the game is about to write its save.
void apply_pending() {
    const std::string target = g_target;
    g_pending = kPendNone;
    if (Clock::now() > g_deadline) {
        NXLOG_I(kTag, "the slot picked for Save (%s) was not used in time: this save stays in %s", target.c_str(), g_active.c_str());
        return;
    }
    if (!slot_name(target) || target == g_active || g_root.empty()) return;
    const stdfs::path src = dir_of(g_active), dst = dir_of(target);
    std::error_code ec;
    if (stdfs::exists(dst, ec)) {      // the slot saved over: its old contents stay beside it
        const stdfs::path bak = stdfs::u8path(g_root) / (target + ".bak");
        stdfs::remove_all(bak, ec);
        ec.clear();
        stdfs::rename(dst, bak, ec);
        if (ec) {
            NXLOG_W(kTag, "cannot set aside the old slot %s (%s): this save stays in %s", target.c_str(), ec.message().c_str(), g_active.c_str());
            return;
        }
    }
    ec.clear();
    if (stdfs::exists(src, ec)) stdfs::copy(src, dst, stdfs::copy_options::recursive, ec);
    else stdfs::create_directories(dst, ec);
    if (ec) {
        NXLOG_W(kTag, "cannot copy slot %s to %s (%s): this save stays in %s", g_active.c_str(), target.c_str(), ec.message().c_str(), g_active.c_str());
        return;
    }
    NXLOG_I(kTag, "saving to slot %s (a copy of slot %s): %s", target.c_str(), g_active.c_str(), dst.string().c_str());
    set_active(target);
}

// services::SavePathHook: "/000/..." is the slot in use. Any guest thread.
void path_hook(const std::string& host_root, std::string* path, bool write) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_root.empty()) {
        g_root = host_root;
        // the slot of the last run, when it still holds a save; else the first that does
        if (!has_save(g_active)) {
            const std::vector<std::string> all = scan();
            if (!all.empty()) set_active(all.front());
            else if (g_active != "000") set_active("000");
        }
        g_had_save = has_save(g_active);
        NXLOG_I(kTag, "save slots: %zu in %s, slot %s in use", scan().size(), g_root.c_str(), g_active.c_str());
    }
    // "/vNNN/...": slot NNN itself, whatever slot is in use (the Load list asks the game to show another slot's save)
    if (path->size() >= 5 && (*path)[1] == 'v' && slot_name(path->substr(2, 3)) && (path->size() == 5 || (*path)[5] == '/')) {
        path->erase(1, 1);
        return;
    }
    if (path->size() < 4 || path->compare(0, 4, "/000") != 0 || (path->size() > 4 && (*path)[4] != '/')) return;
    if (write && path->size() > 4 && path->compare(4, std::string::npos, "/Global") != 0) {      // the game is writing its save
        if (g_pending != kPendNone) apply_pending();
        g_before_new.clear();        // the slot in use is the one to stay with
    }
    if (write) {
        g_cache_ok = false;
        g_written_at = Clock::now();      // the list's rows follow (rows_dirty)
    }
    // The game asks for its save (does it exist, open it). When the slot in use had one and has none now, Delete Save removed it: after
    // that the game goes back into its title sequence and, finding no save, starts a new game. Another slot with a save takes over, so the
    // title comes up with Continue. (A slot New Game just moved to never had a save: it stays in use.)
    if (!write && *path == "/000/Auto") {
        if (has_save(g_active)) g_had_save = true;
        else if (g_had_save) {
            g_had_save = false;
            const std::vector<std::string> all = scan();
            if (!all.empty()) {
                NXLOG_I(kTag, "slot %s was deleted: slot %s in use", g_active.c_str(), all.front().c_str());
                g_pending = kPendNone;
                g_before_new.clear();
                set_active(all.front());
            } else NXLOG_I(kTag, "slot %s was deleted and no other slot holds a save", g_active.c_str());
        }
    }
    if (g_active != "000") path->replace(1, 3, g_active);
}

// ---- the game's entries -------------------------------------------------------------------------------------------------------------------
Kind kind_of(uint64_t item) {
    uint64_t vt = 0;
    if (!rd(item, &vt)) return kNone;
    if (vt == g_base + kVtContinue) return kContinue;
    if (vt == g_base + kVtNewGame) return kNewGame;
    if (vt == g_base + kVtDelete) return kDelete;
    for (uint64_t v : kVtSave)
        if (vt == g_base + v) return kSave;
    return kNone;
}
void redraw(::Ctx* c, uint64_t item) {
    uint64_t vt = 0, fn = 0;
    if (rd(item, &vt) && rd(vt + kSlotRedraw * 8, &fn) && fn_lookup(fn)) call(c, fn, {item});
}

// slot 2: the game's label, with the slot it stands for
char g_text[5][160];
void get_text(::Ctx* c) {
    const uint64_t item = c->x[0];
    const uint64_t text = call(c, g_base + kGetText, {item});
    const Kind k = kind_of(item);
    c->x[0] = text;
    if (!text || k == kNone || !mem::is_mapped(text, 1)) return;
    const char* own = reinterpret_cast<const char*>(text);
    // The game's text is used as the start of the label only when it is plain: a message may begin with a control sequence (its bytes
    // can include zeros, which would end the copy early and leave the slot alone on the label). Then the entry's plain name stands in.
    static const char* const kPlain[] = {"", "Continue", "New Game", "Delete Save", "Save"};
    static bool shown[5] = {};
    bool plain = own[0] != 0;
    for (size_t i = 0; plain && i < 100 && mem::is_mapped(text + i, 1) && own[i]; ++i) plain = static_cast<unsigned char>(own[i]) >= 0x20;
    if (!shown[k]) {
        shown[k] = true;
        char hex[3 * 32 + 1] = {};
        for (size_t i = 0; i < 32 && mem::is_mapped(text + i, 1); ++i) std::snprintf(hex + 3 * i, 4, "%02x ", static_cast<unsigned char>(own[i]));
        NXLOG_I(kTag, "%s: the game's label text is %s; first bytes: %s", kPlain[k], plain ? "plain" : "not plain (the plain name is used)", hex);
    }
    if (!plain) own = kPlain[k];
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_root.empty()) return;
    const size_t n = slots().size();
    char* out = g_text[k];
    static size_t told[5] = {~size_t{0}, ~size_t{0}, ~size_t{0}, ~size_t{0}, ~size_t{0}};
    if (told[k] != n) {      // once per entry and slot count: shows that the entry draws through here and what it saw
        told[k] = n;
        static const char* const names[] = {"", "Continue", "New Game", "Delete Save", "Save"};
        NXLOG_I(kTag, "%s drawn: %zu slot(s) with a save, slot %s in use", names[k], n, g_active.c_str());
    }
    if ((k == kContinue || k == kDelete) && n >= 2) std::snprintf(out, sizeof g_text[0], "%.100s [Slot %s]", own, loadable().c_str());
    else if (k == kNewGame && n >= 1) std::snprintf(out, sizeof g_text[0], "%.100s [Slot %s]", own, next_free().c_str());
    else if (k == kSave && g_pending == kPendSave && Clock::now() <= g_deadline)
        std::snprintf(out, sizeof g_text[0], "%.100s [%sSlot %s]", own, has_save(g_target) ? "" : "New ", g_target.c_str());
    else return;
    c->x[0] = reinterpret_cast<uint64_t>(out);
}

// slots 39 / 40 of Continue and Delete Save: the next / previous slot becomes the one in use
void step(::Ctx* c, int dir) {
    const uint64_t item = c->x[0];
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const std::vector<std::string> all = scan();
        if (all.size() < 2) NXLOG_I(kTag, "left / right on a slot entry: %zu slot(s) with a save in %s, nothing to change to", all.size(), g_root.c_str());
        if (kind_of(item) != kNone && all.size() >= 2) {
            back_to_saved();
            const auto it = std::find(all.begin(), all.end(), g_active);
            const size_t n = all.size(), at = it == all.end() ? 0 : static_cast<size_t>(it - all.begin());
            g_pending = kPendNone;
            set_active(all[(at + n + (dir > 0 ? 1 : n - 1)) % n]);
            changed = true;
            NXLOG_I(kTag, "slot %s in use", g_active.c_str());
        }
    }
    if (changed) redraw(c, item);
    c->x[0] = changed ? 1 : 0;
}
void step_next(::Ctx* c) { step(c, 1); }
void step_prev(::Ctx* c) { step(c, -1); }

void on_saved_slot() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_pending = kPendNone;
    g_load_slot.clear();
    back_to_saved();
}
void continue_acall(::Ctx* c) {
    on_saved_slot();
    pass(c, g_base + kContinueA);
}
void continue_new_acall(::Ctx* c) {
    on_saved_slot();
    pass(c, g_base + kContinueNewA);
}
void delete_acall(::Ctx* c) {
    on_saved_slot();
    pass(c, g_base + kDeleteA);
}
// New Game: the next free folder, with the Global file of the slot in use, is in use from here on. Backing out leaves an empty folder
// that the next New Game takes again; Continue goes back to the slot before it.
void newgame_acall(::Ctx* c) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pending = kPendNone;
        const std::string target = g_root.empty() ? g_active : next_free();
        if (target != g_active) {
            const stdfs::path src = dir_of(g_active), dst = dir_of(target);
            std::error_code ec;
            stdfs::create_directories(dst, ec);
            if (ec) NXLOG_W(kTag, "cannot make slot %s (%s): the new game uses slot %s", target.c_str(), ec.message().c_str(), g_active.c_str());
            else {
                if (stdfs::exists(src / "Global", ec) && !stdfs::exists(dst / "Global", ec)) stdfs::copy_file(src / "Global", dst / "Global", ec);
                if (has_save(g_active)) g_before_new = g_active;
                set_active(target);
                NXLOG_I(kTag, "New Game: slot %s in use", target.c_str());
            }
        }
    }
    pass(c, g_base + kNewGameA);
}

// slot 6 of New Game: selectable with a save too (it goes to a folder of its own)
void newgame_decidable(::Ctx* c) {
    const uint64_t item = c->x[0];
    if (kind_of(item) == kNewGame && mem::is_mapped(item + 0x51, 1)) wr<uint8_t>(item + 0x51, 1);
    c->x[0] = 1;
}

// Save: a list first (fefw_optmenu.cpp's pick list), with a Save tab and a Load tab.
//
// Save tab: the first free folder ("New Slot") and the slots. A on a row makes it where the next save goes and calls the game's own
// Save entry (0x188b880), whose prompt (game::menu::ReportDialog, 0x16da940(root)) comes up over the list. Yes saves there; No leaves
// everything as it was, and B on the list forgets the row.
//
// Load tab: the slots. A on a row calls what the title's Continue calls, 0x881540(root, &path, &function), with that slot's Auto file as
// the path ("vNNN/Auto", which the path hook reads as slot NNN whatever slot is in use) and the game's own function object (vtable
// 0x20c30e8, no state; its invoke makes the Continue prompt, 0x16daea0). The prompt's Yes and No are std::function objects with the
// vtables 0x20c7050 and 0x20c70d0: slot 6 of both (the invoke) points at a thunk here, so Yes first makes the slot the one in use and
// then runs the game's own Yes (0x16dd880: the pending load becomes the Auto file, 0x87e7e0(2, -1, 0); the menu ends, 0x65a7a0; the
// main sequence loads), and No only forgets the slot. The title's own Continue goes through the same two thunks with no slot set.
// When this build's prompt is not the one expected, a row loads on a second A instead, as before (load_direct).
constexpr uint64_t kPendingLoad = 0x87e7e0, kProcEnd = 0x65a7a0, kMainSeq = 0x2209640;
constexpr uint64_t kContinuePrompt = 0x881540, kVtContinueFn = 0x20c30e8, kContinueFnInvoke = 0x16d4e00;
constexpr uint64_t kVtPromptYes = 0x20c7050, kVtPromptNo = 0x20c70d0, kPromptYes = 0x16dd880;
constexpr int kFnInvoke = 6;
constexpr uint32_t kSeqLoad = 32, kPickRefused = 1, kPickDialog = 12, kPickRedraw = 13, kPickNothing = 0;
constexpr auto kLoadConfirm = std::chrono::seconds(6);
std::vector<std::string> g_rows;     // the Save tab's rows when the list was opened (main thread): the first free folder, then the slots
int g_load_rows = 0;                 // the Load tab's: the slots (g_rows without its first)
std::string g_row_active;
uint64_t g_save_item = 0;
int g_load_armed = -1;               // load_direct: the Load row that was pressed once
Clock::time_point g_load_armed_at;
bool g_load_ok = false;              // the functions a load needs are there
bool g_prompt_ok = false;            // and the game's Continue prompt can be used for it
uint64_t g_prompt_no = 0;            // the game's own "No"

int save_rows() { return static_cast<int>(g_rows.size()); }
bool is_load_row(int i) { return i >= save_rows(); }
// The slot behind a row: empty for a row that is not there.
std::string row_slot(int i) {
    const int at = is_load_row(i) ? i - save_rows() + 1 : i;      // the Load tab has no row for the free folder
    return at >= 0 && at < save_rows() && i < save_rows() + g_load_rows ? g_rows[static_cast<size_t>(at)] : std::string();
}
int row_page(int i) { return is_load_row(i) ? 1 : 0; }
void row_label(int i, char* out, size_t n) {
    const std::string slot = row_slot(i);
    if (slot.empty()) return;
    bool saved = true;
    if (i == 0) {
        std::lock_guard<std::mutex> lk(g_mu);
        saved = has_save(slot);
    }
    if (!saved) std::snprintf(out, n, "New Slot");
    else std::snprintf(out, n, "Slot %s%s", slot.c_str(), slot == g_row_active ? " (in use)" : "");
}
void row_value(int i, char* out, size_t n) {
    const std::string slot = row_slot(i);
    if (slot.empty()) return;
    if (is_load_row(i) && i == g_load_armed && Clock::now() - g_load_armed_at <= kLoadConfirm) {
        std::snprintf(out, n, "Press A again to load");
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (has_save(slot)) std::snprintf(out, n, "%s", describe(slot).c_str());
    else std::snprintf(out, n, "Slot %s", slot.c_str());
}
void row_help(int i, char* out, size_t n) {
    const std::string slot = row_slot(i);
    if (slot.empty()) return;
    std::string when;
    bool saved = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        saved = has_save(slot);
        if (saved) describe(slot, &when);
    }
    const std::string at = when.empty() ? std::string() : "Saved " + when + ". ";
    if (is_load_row(i)) std::snprintf(out, n, "%sLoads this slot. Progress since your last save is lost.", at.c_str());
    else if (!saved) std::snprintf(out, n, "Saves to a new slot and keeps the slot in use as it is.");
    else if (slot == g_row_active) std::snprintf(out, n, "%sSaves to the slot in use.", at.c_str());
    else std::snprintf(out, n, "%sSaves over this slot. Its old contents are kept in a .bak folder.", at.c_str());
}
// The rows' values are read from the files: they are drawn again for a moment after the game wrote to the save.
bool rows_dirty() {
    std::lock_guard<std::mutex> lk(g_mu);
    return Clock::now() - g_written_at < std::chrono::seconds(3);
}
// B on the list: a row picked for Save or Load is forgotten.
void rows_back() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_pending == kPendSave) NXLOG_I(kTag, "Save: the list was closed, slot %s is not saved to", g_target.c_str());
    g_pending = kPendNone;
    g_load_slot.clear();
}

// The + menu's root (the process the game's prompts are made under) from its Save entry.
uint64_t save_root() {
    uint64_t item_menu = 0, root = 0;
    if (!rd(g_save_item + 0x8, &item_menu) || !item_menu) return 0;
    return rd(item_menu + 0x98, &root) && root ? root : item_menu;
}
// The slot becomes the one in use and the game is asked to load its Auto file (what the Continue prompt's Yes does).
void load_now(::Ctx* c, const std::string& slot, uint64_t root, uint64_t seq) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pending = kPendNone;
        g_before_new.clear();
        set_active(slot);
        NXLOG_I(kTag, "Load: slot %s in use, asking the main sequence for a load", slot.c_str());
    }
    call(c, g_base + kPendingLoad, {2, 0xffffffffull, 0});
    call(c, g_base + kProcEnd, {root});
    wr<uint32_t>(seq + 0x98, kSeqLoad);
}
// A on a Load row without the game's prompt: once to arm it, again within a few seconds to load.
uint32_t load_direct(::Ctx* c, int i, const std::string& slot) {
    const uint64_t root = save_root();
    uint64_t seq = 0;
    if (!root) return kPickRefused;
    if (g_load_armed != i || Clock::now() - g_load_armed_at > kLoadConfirm) {
        g_load_armed = i;
        g_load_armed_at = Clock::now();
        return kPickRedraw;
    }
    g_load_armed = -1;
    if (!rd(g_base + kMainSeq, &seq) || !seq || !mem::is_mapped(seq + 0x98, 4)) return kPickRefused;
    load_now(c, slot, root, seq);
    return kPickNothing;
}
// A on a Load row: the game's Continue prompt for that slot's save.
uint32_t load_pick(::Ctx* c, int i) {
    const std::string slot = row_slot(i);
    if (slot.empty() || !g_load_ok) return kPickRefused;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!has_save(slot)) return kPickRefused;
    }
    if (!g_prompt_ok) return load_direct(c, i, slot);
    const uint64_t root = save_root();
    if (!root) return kPickRefused;
    static char path[16];                 // static: the prompt may keep the pointers past this call
    static uint64_t view[2], fn[6];
    std::snprintf(path, sizeof path, "v%s/Auto", slot.c_str());
    view[0] = reinterpret_cast<uint64_t>(path), view[1] = std::strlen(path);
    fn[0] = g_base + kVtContinueFn, fn[4] = reinterpret_cast<uint64_t>(fn);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pending = kPendNone;
        g_load_slot = slot;
        NXLOG_I(kTag, "Load: the game's Continue prompt for slot %s", slot.c_str());
    }
    call(c, g_base + kContinuePrompt, {root, reinterpret_cast<uint64_t>(view), reinterpret_cast<uint64_t>(fn)});
    return kPickDialog;
}
// Slot 6 of the Continue prompt's Yes: the slot the Load list asked about becomes the one in use, then the game's own Yes loads it.
void prompt_yes(::Ctx* c) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_load_slot.empty() && has_save(g_load_slot)) {
            g_pending = kPendNone;
            g_before_new.clear();
            set_active(g_load_slot);
            NXLOG_I(kTag, "Load: yes, slot %s in use", g_load_slot.c_str());
        }
        g_load_slot.clear();
    }
    pass(c, g_base + kPromptYes);
}
void prompt_no(::Ctx* c) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_load_slot.empty()) NXLOG_I(kTag, "Load: no, slot %s stays as it is", g_load_slot.c_str());
        g_load_slot.clear();
    }
    pass(c, g_prompt_no);
}
// A on a Save row: the next save goes to that slot, and the game's own Save entry brings its prompt.
uint32_t row_pick(::Ctx* c, int i) {
    if (is_load_row(i)) return load_pick(c, i);
    const std::string slot = row_slot(i);
    if (slot.empty() || !g_save_item || kind_of(g_save_item) != kSave) return kPickRefused;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_load_slot.clear();
        g_target = slot;
        g_pending = kPendSave;
        g_deadline = Clock::now() + kPrompt;      // the game's prompt may stay up for a while
        NXLOG_I(kTag, "Save: slot %s picked, the game's save prompt follows", g_target.c_str());
    }
    const uint32_t answer = static_cast<uint32_t>(call(c, g_base + kSaveA, {g_save_item}));
    if (answer != kPickDialog) {      // the game did not ask (it answered something else): nothing is armed
        std::lock_guard<std::mutex> lk(g_mu);
        g_pending = kPendNone;
        NXLOG_I(kTag, "Save: the game's Save entry answered %u, no prompt", answer);
    }
    return answer;
}
void save_acall(::Ctx* c) {
    const uint64_t item = c->x[0];
    uint8_t ready = 0;
    bool known = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        known = !g_root.empty();
        g_pending = kPendNone;
        g_load_slot.clear();
        if (known) {
            g_rows = scan();
            g_load_rows = static_cast<int>(g_rows.size());
            g_rows.insert(g_rows.begin(), next_free());
            g_row_active = g_active;
        }
    }
    if (known && rd(item + 0x52, &ready) && ready == 1) {
        OptList list;
        list.pages = g_load_ok && g_load_rows > 0 ? 2 : 1;
        list.titles[0] = "Save";
        list.titles[1] = "Load";
        list.count = save_rows() + (list.pages == 2 ? g_load_rows : 0);
        if (list.pages == 1) g_load_rows = 0;
        list.page = row_page;
        list.label = row_label;
        list.value = row_value;
        list.help = row_help;
        list.pick = row_pick;
        list.back = rows_back;
        list.dirty = rows_dirty;
        g_save_item = item;
        g_load_armed = -1;
        if (optmenu_open_list(c, item, list)) {
            c->x[0] = kOpened;
            return;
        }
    }
    pass(c, g_base + kSaveA);
}

bool slot_is(uint64_t vt, int slot, uint64_t fn) {
    uint64_t v = 0;
    return rd(g_base + vt + static_cast<uint64_t>(slot) * 8, &v) && v == g_base + fn;
}
void patch(uint64_t vt, int slot, uint64_t thunk) {
    const uint64_t at = g_base + vt + static_cast<uint64_t>(slot) * 8;
    mem::protect(at & ~0xfffull, 0x1000, mem::Prot::RW);
    wr<uint64_t>(at, thunk);
}
}  // namespace

void saveslot_install(uint64_t base) {
    g_base = base;
    g_on = config_get_bool("fefw", "save_slots", true);
    if (!g_on) return;
    const std::string slot = config_get_str("fefw", "save_slot", "000");
    if (slot_name(slot)) g_active = slot;
    services::set_save_path_hook(&path_hook);      // before the game mounts its save data
}

// Main thread, once per frame: hooks the entries once the game's data is there and is the build the addresses are for.
void saveslot_frame() {
    if (g_hooked || g_failed || !g_on || !g_base) return;
    uint64_t probe = 0;
    if (!rd(g_base + kVtContinue + kSlotACall * 8, &probe) || !probe) return;
    bool ok = slot_is(kVtContinue, kSlotACall, kContinueA) && slot_is(kVtNewGame, kSlotACall, kNewGameA) && slot_is(kVtDelete, kSlotACall, kDeleteA) &&
              slot_is(kVtContinueNew, kSlotACall, kContinueNewA) && slot_is(kVtNewGame, kSlotDecidable, kBaseDecidable);
    for (uint64_t vt : {kVtContinue, kVtDelete}) ok = ok && slot_is(vt, kSlotNext, kBaseNext) && slot_is(vt, kSlotPrev, kBasePrev);
    for (uint64_t vt : {kVtContinue, kVtNewGame, kVtDelete, kVtSave[0], kVtSave[1], kVtSave[2]}) {
        uint64_t redraw_fn = 0;
        ok = ok && slot_is(vt, kSlotText, kGetText) && rd(g_base + vt + kSlotRedraw * 8, &redraw_fn) && fn_lookup(redraw_fn);
    }
    for (uint64_t vt : kVtSave) ok = ok && slot_is(vt, kSlotACall, kSaveA);
    for (uint64_t f : {kGetText, kContinueA, kNewGameA, kDeleteA, kSaveA, kContinueNewA}) ok = ok && fn_lookup(g_base + f);
    if (!ok) {
        g_failed = true;
        NXLOG_W(kTag, "this build's title and Save entries are not the ones this was written for: the slot is [fefw] save_slot only");
        return;
    }
    const uint64_t text = hle_register("fefw.saveslot.text", get_text);
    for (uint64_t vt : {kVtContinue, kVtNewGame, kVtDelete, kVtSave[0], kVtSave[1], kVtSave[2]}) patch(vt, kSlotText, text);
    const uint64_t next = hle_register("fefw.saveslot.next", step_next), prev = hle_register("fefw.saveslot.prev", step_prev);
    for (uint64_t vt : {kVtContinue, kVtDelete}) {
        patch(vt, kSlotNext, next);
        patch(vt, kSlotPrev, prev);
    }
    patch(kVtContinue, kSlotACall, hle_register("fefw.saveslot.continue", continue_acall));
    patch(kVtNewGame, kSlotACall, hle_register("fefw.saveslot.newgame", newgame_acall));
    patch(kVtNewGame, kSlotDecidable, hle_register("fefw.saveslot.newgame_decidable", newgame_decidable));
    patch(kVtDelete, kSlotACall, hle_register("fefw.saveslot.delete", delete_acall));
    patch(kVtContinueNew, kSlotACall, hle_register("fefw.saveslot.continue_new", continue_new_acall));
    const uint64_t save = hle_register("fefw.saveslot.save", save_acall);
    for (uint64_t vt : kVtSave) patch(vt, kSlotACall, save);
    uint64_t seq_slot = 0;
    g_load_ok = fn_lookup(g_base + kPendingLoad) && fn_lookup(g_base + kProcEnd) && rd(g_base + kMainSeq, &seq_slot);
    if (!g_load_ok) NXLOG_W(kTag, "the functions a load needs are not where they were expected: the Save list has no Load tab");
    // the game's Continue prompt for the Load tab: its Yes and No go through here
    g_prompt_ok = g_load_ok && fn_lookup(g_base + kContinuePrompt) && slot_is(kVtContinueFn, kFnInvoke, kContinueFnInvoke) &&
                  slot_is(kVtPromptYes, kFnInvoke, kPromptYes) && rd(g_base + kVtPromptNo + kFnInvoke * 8, &g_prompt_no) && fn_lookup(g_prompt_no);
    if (g_prompt_ok) {
        patch(kVtPromptYes, kFnInvoke, hle_register("fefw.saveslot.prompt_yes", prompt_yes));
        patch(kVtPromptNo, kFnInvoke, hle_register("fefw.saveslot.prompt_no", prompt_no));
    } else if (g_load_ok) NXLOG_W(kTag, "the game's Continue prompt is not where it was expected: a Load row loads on a second A");
    g_hooked = true;
    NXLOG_I(kTag, "save slots hooked: Continue, New Game, Delete Save and Save choose the folder ([fefw] save_slots)");
}
}  // namespace nxrt::games::fefw
