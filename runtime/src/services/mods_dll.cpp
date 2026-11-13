// SPDX-License-Identifier: GPL-3.0-or-later
// Code mods as DLLs (nxrt/mod_api.h): every *.dll in the exefs/ folder of an installed mod (installed_mods(), common.cpp) is loaded when
// play mode starts and its nxmod_init is called with a table of runtime functions. [fs] dll_mods = false turns loading off.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/mod_api.h"
#include "nxrt/services_api.h"
#include "services_internal.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace nxrt::services::modapi {
namespace {

constexpr const char* kTag = "mods";

struct Mod {
    std::string name;            // mod folder below [fs] mods, then the DLL's file name
    NxModApi api{};              // the mod's own copy (log() finds the mod by it)
    HMODULE dll = nullptr;
    std::vector<uint32_t> hooks; // added during nxmod_init, removed if the mod declines
};
struct FrameFn {
    NxModFrameFn fn;
    void* user;
};

struct State {
    std::mutex mu;
    std::vector<Mod*> mods;      // never freed: hooks and frame callbacks may still point into the DLLs
    std::vector<FrameFn> frame;
    Mod* in_init = nullptr;      // the mod whose nxmod_init is running (main thread)
    uint32_t frame_hook = 0;
    bool loaded = false;
};
State& st() {
    static State s;
    return s;
}

std::string mod_name(const NxModApi* api) {
    std::lock_guard<std::mutex> lk(st().mu);
    for (const Mod* m : st().mods)
        if (&m->api == api) return m->name;
    return "?";
}

void api_log(const NxModApi* api, int level, const char* text) {
    const std::string name = mod_name(api);
    const char* t = text ? text : "";
    switch (level) {
    case 0: NXLOG_E(kTag, "%s: %s", name.c_str(), t); break;
    case 1: NXLOG_W(kTag, "%s: %s", name.c_str(), t); break;
    case 2: NXLOG_I(kTag, "%s: %s", name.c_str(), t); break;
    default: NXLOG_D(kTag, "%s: %s", name.c_str(), t); break;
    }
}

uint32_t track(uint32_t id) {
    if (id && st().in_init) st().in_init->hooks.push_back(id);
    return id;
}
uint32_t api_hook_inline(uint64_t addr, NxModInlineFn fn, void* user) { return track(hook_add_inline(addr, fn, user)); }
uint32_t api_hook_mid(uint64_t addr, NxModSiteFn fn, void* user) { return track(hook_add_mid(addr, fn, user)); }
uint32_t api_hook_entry(uint64_t addr, NxModSiteFn fn, void* user) { return track(hook_add_entry(addr, fn, user)); }
int api_hook_remove(uint32_t id) { return hook_remove(id) ? 1 : 0; }

uint64_t api_call_guest(::Ctx* c, uint64_t addr, const uint64_t* args, int nargs) {
    if (!c || !addr) return 0;
    const ::Ctx saved = *c;
    for (int i = 0; i < 8; ++i) c->x[i] = (args && i < nargs) ? args[i] : 0;
    // below the caller's stack, 16-byte aligned (the stack the hook was given stays as it was)
    sp(c) = (sp(c) - 0x100) & ~uint64_t{0xF};
    call_indirect(c, addr);
    const uint64_t result = c->x[0];
    *c = saved;
    return result;
}

int api_is_mapped(uint64_t addr, uint64_t size) { return addr && mem::is_mapped(addr, size) ? 1 : 0; }

void api_on_frame(NxModFrameFn fn, void* user) {
    if (!fn) return;
    std::lock_guard<std::mutex> lk(st().mu);
    st().frame.push_back({fn, user});
}

// the game's frame boundary (GamePlay::frame_update, also play mode's own hook): never "handled", the game's function runs as usual
int frame_hook(::Ctx* c, uint64_t, void*) {
    std::vector<FrameFn> fns;
    {
        std::lock_guard<std::mutex> lk(st().mu);
        fns = st().frame;
    }
    for (const FrameFn& f : fns) f.fn(c, f.user);
    return 0;
}

std::string u8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

void load_one(const std::string& mod, const std::filesystem::path& mod_dir, const std::filesystem::path& file, uint64_t base) {
    const std::string name = mod + "/" + u8(file.filename());
    HMODULE dll = LoadLibraryExW(file.wstring().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!dll) {
        NXLOG_E(kTag, "%s: cannot be loaded (Windows error %lu)", name.c_str(), static_cast<unsigned long>(GetLastError()));
        return;
    }
    const auto init = reinterpret_cast<NxModInitFn>(GetProcAddress(dll, "nxmod_init"));
    if (!init) {
        NXLOG_W(kTag, "%s: no nxmod_init export, not a code mod: unloaded", name.c_str());
        FreeLibrary(dll);
        return;
    }
    Mod* m = new Mod;
    m->name = name;
    m->dll = dll;
    m->api.version = NXMOD_API_VERSION;
    m->api.size = sizeof(NxModApi);
    m->api.game_id = game::info().id;
    m->api.main_base = base;
    m->api.log = &api_log;
    m->api.hook_inline = &api_hook_inline;
    m->api.hook_mid = &api_hook_mid;
    m->api.hook_entry = &api_hook_entry;
    m->api.hook_remove = &api_hook_remove;
    m->api.call_guest = &api_call_guest;
    m->api.is_mapped = &api_is_mapped;
    m->api.on_frame = &api_on_frame;
    size_t frames_before;
    {
        std::lock_guard<std::mutex> lk(st().mu);
        st().mods.push_back(m);
        frames_before = st().frame.size();
    }
    st().in_init = m;
    const std::string dir = u8(mod_dir);
    const int rc = init(&m->api, dir.c_str());
    st().in_init = nullptr;
    if (rc != 0) {
        for (uint32_t id : m->hooks) hook_remove(id);
        std::lock_guard<std::mutex> lk(st().mu);
        st().frame.resize(frames_before);
        NXLOG_W(kTag, "%s: declined to start (nxmod_init returned %d); its hooks are removed", name.c_str(), rc);
        return;   // the DLL stays loaded: it may still own memory the runtime saw
    }
    NXLOG_I(kTag, "%s: code mod started (%zu hooks)", name.c_str(), m->hooks.size());
}

}  // namespace

void load_dll_mods(uint64_t base, uint64_t frame_update) {
    if (st().loaded) return;
    st().loaded = true;
    if (!config_get_bool("fs", "dll_mods", true)) {
        NXLOG_I(kTag, "code mods (exefs/*.dll) are off ([fs] dll_mods = false)");
        return;
    }
    namespace stdfs = std::filesystem;
    for (const RomfsMod& mod : installed_mods()) {
        if (mod.exefs.empty()) continue;
        std::error_code ec;
        std::vector<stdfs::path> dlls;
        for (const stdfs::directory_entry& e : stdfs::directory_iterator(stdfs::u8path(mod.exefs), ec)) {
            if (!e.is_regular_file(ec)) continue;
            std::wstring ext = e.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (ext == L".dll") dlls.push_back(e.path());
        }
        std::sort(dlls.begin(), dlls.end());
        for (const stdfs::path& p : dlls) load_one(mod.name, stdfs::u8path(mod.exefs).parent_path(), p, base);
    }
    if (frame_update) {
        std::lock_guard<std::mutex> lk(st().mu);
        if (!st().mods.empty()) st().frame_hook = hook_add_entry(base + frame_update, &frame_hook);
    }
}

}  // namespace nxrt::services::modapi
