// SPDX-License-Identifier: GPL-3.0-or-later
// fefw.exe: Fire Emblem: Fortune's Weave (Switch 2, 04007EF01F8AE000, update 131072) as a native Windows process. A minimal pkp_main.cpp:
//   1. map the six NSOs (rtld, main, subsdk0 dlss, subsdk1 cuda, subsdk2 movie, sdk) at the bases the lifter baked into the generated code
//      (recomp/games/fefw/config.py; each library's NxModule carries its base),
//   2. register the lifted module tables with the runtime (each library carries the ABI version it was built with),
//   3. relocate: the game's own rtld does it ([boot] link = rtld, the default) or the runtime's linker ([boot] link = host),
//   4. turn the host main thread into the guest main thread,
//   5. enter rtld at offset 0 with x0 = 0, x1 = the main thread handle.
//
//   fefw [--exefs DIR] [--main FILE] [--romfs DIR] [--ini FILE] [--log FILE] [--max-seconds N] [--link rtld|host] [--no-nvn] [--headless|--window]
// The dump is read where it is, nothing is copied: [fs] exefs (the update's ExeFS folder), [fs] main (main as an uncompressed NSO: the
// console's main is ZBIC-compressed, which the runtime's NSO loader cannot read), [fs] romfs (the extracted RomFS folder). Their defaults
// are set by the build (fefw_paths.h). The ini is --ini, else NXRT_INI, else fefw.ini in the working directory when it exists, else nxrt.ini.
// The command line and the NXRT_FS_* variables win over the ini. [boot] keys as in pkp_main.cpp (docs/CONFIG.md).
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS      // fopen and getenv are used as standard C intends
#endif
#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "nxrt/pgo.h"
#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/gfx.h"
#include "nxrt/input_source.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/nvn.h"
#include "nxrt/nvn_backend.h"
#include "nxrt/nxmodule.h"
#include "nxrt/platform.h"
#include "nxrt/play.h"
#include "nxrt/runtime.h"

#include "ObeliskChamber/ObeliskChamber_game.h"   // the Fortune's Weave game module (runtime/games/ObeliskChamber)
#include "fefw_cpu.h"         // the CPU features the lifted code needs (per build: FMA3 + AVX or SSE4.2 + POPCNT; CX16), checked first
#include "fefw_paths.h"       // FEFW_EXEFS_DEFAULT, FEFW_MAIN_DEFAULT, FEFW_ROMFS_DEFAULT (game-build/fefw/CMakeLists.txt)

// Two builds of this file (game-build/fefw/CMakeLists.txt):
//   fefw.exe     (default): the lifted libraries are linked in, main() below names their module tables.
//   fefw_rt.dll  (FEFW_RT_DLL, the release package): the runtime alone, no game code. fefw.exe is then linked on the player's PC from the
//                libraries lifted out of their own dump and src/fefw_stub.cpp, which hands the module tables to fefw_rt_main()
//                (src/fefw_rt_api.h). Errors also show in a message box there (the exe is a GUI program).
#ifdef FEFW_RT_DLL
#include "fefw_rt_api.h"
#else
extern "C" {
extern const NxModule nxmod_main, nxmod_sdk, nxmod_subsdk0, nxmod_subsdk1, nxmod_subsdk2, nxmod_rtld;
extern const uint32_t nxmod_main_abi, nxmod_sdk_abi, nxmod_subsdk0_abi, nxmod_subsdk1_abi, nxmod_subsdk2_abi, nxmod_rtld_abi;
}
#endif

namespace {

struct GameModule {
    const char* file;            // name of the NSO in the ExeFS
    const NxModule* nx;          // nx->base: where the generated code expects the image
    const uint32_t* abi;
};

// Load order = the console loader's: rtld (the entry), main, subsdk0..2, sdk. Filled by the entry point of the build.
GameModule kModules[6] = {{"rtld"}, {"main"}, {"subsdk0"}, {"subsdk1"}, {"subsdk2"}, {"sdk"}};

std::unique_ptr<nxrt::gfx::Device> g_gfx_device;
std::unique_ptr<nxrt::gfx::Renderer> g_gfx_renderer;

[[noreturn]] void die(const char* fmt, const char* a = "", const char* b = "") {
    NXLOG_E("boot", fmt, a, b);
    nxrt::log_flush();
    std::fprintf(stderr, "fefw: ");
    std::fprintf(stderr, fmt, a, b);
    std::fprintf(stderr, "\n");
#ifdef FEFW_RT_DLL
    char msg[1024];
    std::snprintf(msg, sizeof msg, fmt, a, b);
    MessageBoxA(nullptr, msg, "fefw", MB_OK | MB_ICONERROR);
#endif
    TerminateProcess(GetCurrentProcess(), 1);
    for (;;) {}
}

std::wstring widen_utf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
bool file_exists(const std::string& p) {
    const DWORD a = GetFileAttributesW(widen_utf8(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool dir_exists(const std::string& p) {
    const DWORD a = GetFileAttributesW(widen_utf8(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// The game's own files cannot be read: says so (message box, stderr, the log and fefw_fatal.txt, which the launcher shows) and ends with
// code 1, before the game's first fs call turns it into a guest abort and a bare "code 4" crash report (issue 142). Under Wine / Proton
// the usual cause is Steam's container (the Steam Linux Runtime), which sees only the home folder, /mnt, /media, /run/media and the
// Steam libraries: a dump elsewhere does not exist inside it.
[[noreturn]] void die_romfs(const std::string& romfs) {
    const bool wine = GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") != nullptr;
    std::string shown = romfs, top;
    if (wine && shown.size() > 2 && (shown[0] == 'Z' || shown[0] == 'z') && shown[1] == ':') {
        shown.erase(0, 2);                                       // Z:/cracks/... is /cracks/... on the Linux side
        for (char& ch : shown) ch = ch == '\\' ? '/' : ch;
        top = shown.substr(0, shown.find('/', 1));
    }
    std::string msg = "The game's files (the RomFS folder of your dump) cannot be read:\n  " + shown +
                      "\n\nfefw.ini ([fs] romfs) points there. If the dump was moved or deleted, run Set up again in the launcher with "
                      "its new place, or set [fs] romfs to the dump's romfs folder.";
    if (wine)
        msg += "\n\nOn Linux, Steam runs the game in a container (the Steam Linux Runtime) that sees only your home folder, /mnt, "
               "/media, /run/media and Steam's library folders. A dump anywhere else is invisible to the game. Move the dump under "
               "one of those and run Set up again, or put STEAM_COMPAT_MOUNTS=" + (top.empty() ? std::string("<the dump's top folder>") : top) +
               " %command% in the game's launch options in Steam.";
    NXLOG_E("boot", "RomFS folder '%s' cannot be read (wine: %s)", romfs.c_str(), wine ? "yes" : "no");
    nxrt::log_flush();
    std::fprintf(stderr, "fefw: %s\n", msg.c_str());
    if (FILE* f = std::fopen("fefw_fatal.txt", "w")) {
        std::fprintf(f, "fefw cannot find the game's files.\n\n%s\n", msg.c_str());
        std::fclose(f);
    }
    MessageBoxW(nullptr, widen_utf8(msg).c_str(), L"fefw: the game's files cannot be read", MB_OK | MB_ICONERROR);
    TerminateProcess(GetCurrentProcess(), 1);
    for (;;) {}
}

// NSO header flags (0x0C): bits 0..2 LZ4 per segment, bit 7 ZBIC (Switch 2's patched zstd). 0xFFFFFFFF when unreadable.
uint32_t nso_flags(const std::string& path) {
    uint32_t flags = 0xFFFFFFFFu;
    if (FILE* f = _wfopen(widen_utf8(path).c_str(), L"rb")) {
        uint8_t h[16] = {};
        if (std::fread(h, 1, sizeof h, f) == sizeof h && !std::memcmp(h, "NSO0", 4)) std::memcpy(&flags, h + 12, 4);
        std::fclose(f);
    }
    return flags;
}

// [boot] press = "A@12,A@20:500,PLUS@30,A#1500": scripted controller presses on player 1 (pkp_main.cpp has the same syntax).
struct ScriptedPress { uint64_t bit; double at; int hold_ms; bool by_frame = false; };
std::shared_ptr<nxrt::hid::ManualInputSource> g_pad;
std::vector<ScriptedPress> g_presses;
DWORD WINAPI press_thread(LPVOID) {
    const ULONGLONG t0 = GetTickCount64();
    for (const ScriptedPress& p : g_presses) {
        if (p.by_frame) {
            while (!nxrt::nvn::backend_installed() || nxrt::nvn::backend_stats().presents < static_cast<uint64_t>(p.at)) Sleep(20);
        } else {
            const ULONGLONG due = t0 + static_cast<ULONGLONG>(p.at * 1000);
            for (ULONGLONG now = GetTickCount64(); now < due; now = GetTickCount64()) Sleep(static_cast<DWORD>(due - now > 50 ? 50 : due - now));
        }
        NXLOG_I("boot", "scripted press at %.1f s", (GetTickCount64() - t0) / 1000.0);
        g_pad->press(0, p.bit);
        Sleep(p.hold_ms);
        g_pad->release(0, p.bit);
    }
    return 0;
}
void parse_presses(const std::string& spec) {
    using namespace nxrt::hid;
    static const struct { const char* n; NpadButtonBit b; } names[] = {{"A", kBtnA}, {"B", kBtnB}, {"X", kBtnX}, {"Y", kBtnY}, {"PLUS", kBtnPlus},
        {"MINUS", kBtnMinus}, {"UP", kBtnUp}, {"DOWN", kBtnDown}, {"LEFT", kBtnLeft}, {"RIGHT", kBtnRight}, {"L", kBtnL}, {"R", kBtnR}, {"ZL", kBtnZL}, {"ZR", kBtnZR}};
    size_t i = 0;
    while (i < spec.size()) {
        size_t j = spec.find(',', i);
        if (j == std::string::npos) j = spec.size();
        const std::string item = spec.substr(i, j - i);
        i = j + 1;
        size_t at = item.find('@');
        const bool by_frame = at == std::string::npos && item.find('#') != std::string::npos;
        if (by_frame) at = item.find('#');
        if (at == std::string::npos) continue;
        const std::string name = item.substr(0, at);
        const size_t colon = item.find(':', at);
        ScriptedPress sp{0, std::atof(item.c_str() + at + 1), colon == std::string::npos ? 300 : std::atoi(item.c_str() + colon + 1), by_frame};
        for (const auto& e : names) if (name == e.n) sp.bit = btn(e.b);
        if (sp.bit) g_presses.push_back(sp);
    }
}

// [boot] screenshot_at / screenshot_every: shot_<n>.png of the presented frame in the working directory.
DWORD WINAPI shot_thread(LPVOID) {
    const int64_t first = nxrt::config_get_int("boot", "screenshot_at", 0), every = nxrt::config_get_int("boot", "screenshot_every", 0);
    int n = 0;
    Sleep(static_cast<DWORD>(first * 1000));
    for (;;) {
        if (g_gfx_renderer) {
            char path[64];
            std::snprintf(path, sizeof path, "shot_%d.png", n++);
            g_gfx_renderer->request_screenshot(path);
            NXLOG_I("boot", "screenshot requested: %s", path);
        }
        if (every <= 0) break;
        Sleep(static_cast<DWORD>(every * 1000));
    }
    return 0;
}

// [boot] max_seconds: log the state and end the process with exit code 124.
DWORD WINAPI deadline_thread(LPVOID p) {
    Sleep(static_cast<DWORD>(reinterpret_cast<uintptr_t>(p)));
    NXLOG_W("boot", "max_seconds reached: the guest is still running; ending the process");
    nxrt::log_flush();
    if (nxrt::nvn::backend_installed()) {
        const nxrt::nvn::BackendStats b = nxrt::nvn::backend_stats();
        NXLOG_I("boot", "nvn back end at the deadline: submits %llu lists %llu draws %llu (skipped %llu) presents %llu pipelines %llu textures %llu (failed %llu) unsupported %llu",
                (unsigned long long)b.submits, (unsigned long long)b.lists_replayed, (unsigned long long)b.draws, (unsigned long long)b.draws_skipped,
                (unsigned long long)b.presents, (unsigned long long)b.pipelines_created, (unsigned long long)b.textures_created,
                (unsigned long long)b.textures_failed, (unsigned long long)b.unsupported_ops);
        NXLOG_I("boot", "nvn unsupported operations: %s", nxrt::nvn::backend_unsupported_report().c_str());
    }
    NXLOG_I("boot", "state at the deadline:\n%s", nxrt::summary().c_str());
    nxrt::profile_flush();     // profile-guided builds: a timed run ([boot] max_seconds) keeps its counters
    nxrt::log_flush();
    TerminateProcess(GetCurrentProcess(), 124);
    return 0;
}

// Command line (or NXRT_FS_*), else the ini, else the build's default.
std::string fs_path(const char* key, const std::string& cli, const char* def) {
    if (!cli.empty()) nxrt::config_set("fs", key, cli.c_str());
    else if (!nxrt::config_has("fs", key)) nxrt::config_set("fs", key, def);
    return nxrt::config_get_str("fs", key, def);
}

int fefw_run(int argc, char** argv);

// The processor features the build of the game code needs (FEFW_CPU_*), logged at start-up.
uint32_t g_cpu_needs = FEFW_CPU_NEEDS_FMA_BUILD;

// Before the first lifted instruction: a CPU without what this build of the game code needs (`needs`: FMA3 / AVX for the default build,
// SSE4.2 / POPCNT for the compatible one, CX16 for both) would die on an illegal instruction in a blank window (issues 33, 5). Says which
// feature is missing in a message box, on stderr and in fefw_fatal.txt (the launcher offers it), then ends with code 4.
void check_cpu_or_exit(uint32_t needs) {
    g_cpu_needs = needs;
    const fefw_cpu::Report r = fefw_cpu::probe();
    if (!r.missing(needs)) return;
    const std::string msg = fefw_cpu::message(r, needs);
    std::fprintf(stderr, "fefw: %s\n", msg.c_str());
    if (FILE* f = std::fopen("fefw_fatal.txt", "w")) {
        auto yes = [&](uint32_t bit) { return (r.have & bit) ? "yes" : "NO"; };
        std::fprintf(f, "fefw cannot start on this processor.\n\n%s\n\nfefw.exe build: %s\nCPU features: FMA3 %s, AVX %s, SSE4.2 %s, POPCNT %s, CMPXCHG16B %s\n",
                     msg.c_str(), fefw_cpu::build_name(needs), yes(FEFW_CPU_FMA), yes(FEFW_CPU_AVX), yes(FEFW_CPU_SSE42), yes(FEFW_CPU_POPCNT),
                     yes(FEFW_CPU_CX16));
        std::fclose(f);
    }
    MessageBoxA(nullptr, msg.c_str(), "fefw: this processor is too old", MB_OK | MB_ICONERROR);
    TerminateProcess(GetCurrentProcess(), 4);
    for (;;) {}
}

}  // namespace

#ifdef FEFW_RT_DLL
extern "C" int fefw_rt_main(int argc, char** argv, const FefwRtModules* m) {
    // the build's processor features; a stub from before cpu_needs (0.1.6 and older) was always the FMA3 build
    const bool has_needs = m && m->size >= offsetof(FefwRtModules, cpu_needs) + sizeof(uint32_t);
    check_cpu_or_exit(has_needs ? m->cpu_needs : FEFW_CPU_NEEDS_FMA_BUILD);
    if (!m || m->size < offsetof(FefwRtModules, cpu_needs) || m->api != FEFW_RT_API) {
        MessageBoxA(nullptr, "fefw.exe and fefw_rt.dll are from different releases: run Set up again in the launcher.", "fefw", MB_OK | MB_ICONERROR);
        return 3;
    }
    const NxModule* nx[6] = {m->rtld, m->main, m->subsdk0, m->subsdk1, m->subsdk2, m->sdk};
    const uint32_t* abi[6] = {m->rtld_abi, m->main_abi, m->subsdk0_abi, m->subsdk1_abi, m->subsdk2_abi, m->sdk_abi};
    for (int i = 0; i < 6; i++) {
        kModules[i].nx = nx[i];
        kModules[i].abi = abi[i];
    }
    return fefw_run(argc, argv);
}
#else
int main(int argc, char** argv) {
    check_cpu_or_exit(FEFW_CPU_NEEDS_FMA_BUILD);     // the libraries of recomp/games/fefw are compiled with -mfma
    const NxModule* nx[6] = {&nxmod_rtld, &nxmod_main, &nxmod_subsdk0, &nxmod_subsdk1, &nxmod_subsdk2, &nxmod_sdk};
    const uint32_t* abi[6] = {&nxmod_rtld_abi, &nxmod_main_abi, &nxmod_subsdk0_abi, &nxmod_subsdk1_abi, &nxmod_subsdk2_abi, &nxmod_sdk_abi};
    for (int i = 0; i < 6; i++) {
        kModules[i].nx = nx[i];
        kModules[i].abi = abi[i];
    }
    return fefw_run(argc, argv);
}
#endif

namespace {
int fefw_run(int argc, char** argv) {
    nxrt_register_game_ObeliskChamber();           // before anything reads it (nxrt::init, the services): docs/GAME_BOUNDARY.md
    std::string exefs_arg, main_arg, romfs_arg;
    const char* ini = nullptr;
    const char* logp = nullptr;
    std::vector<std::pair<std::string, std::string>> boot_opts;   // [boot] keys from the command line, applied after nxrt::init() loads the ini
    for (int i = 1; i < argc; i++) {
        auto arg = [&](const char* n) { return !std::strcmp(argv[i], n) && i + 1 < argc; };
        if (arg("--exefs")) exefs_arg = argv[++i];
        else if (arg("--main")) main_arg = argv[++i];
        else if (arg("--romfs")) romfs_arg = argv[++i];
        else if (arg("--ini")) ini = argv[++i];
        else if (arg("--log")) logp = argv[++i];
        else if (arg("--max-seconds")) boot_opts.emplace_back("max_seconds", argv[++i]);
        else if (arg("--link")) boot_opts.emplace_back("link", argv[++i]);
        else if (!std::strcmp(argv[i], "--no-nvn")) boot_opts.emplace_back("nvn", "0");
        else if (!std::strcmp(argv[i], "--headless")) boot_opts.emplace_back("gfx", "headless");
        else if (!std::strcmp(argv[i], "--window")) boot_opts.emplace_back("gfx", "window");
        else { std::fprintf(stderr, "fefw: unknown argument %s\n", argv[i]); return 2; }
    }
    if (!ini && !std::getenv("NXRT_INI") && file_exists(nxrt::game::info().game_ini)) ini = nxrt::game::info().game_ini;

    nxrt::InitOptions opt;
    opt.ini_path = ini;
    opt.log_path = logp;
    if (!nxrt::init(opt)) die("runtime start-up failed (address-space layout)");
    for (const auto& kv : boot_opts) nxrt::config_set("boot", kv.first.c_str(), kv.second.c_str());
    const std::string exefs = fs_path("exefs", exefs_arg, FEFW_EXEFS_DEFAULT);
    const std::string main_nso = fs_path("main", main_arg, FEFW_MAIN_DEFAULT);
    const std::string romfs = fs_path("romfs", romfs_arg, FEFW_ROMFS_DEFAULT);
    nxrt::config_set("process", "program_id", "0x04007EF01F8AE000");     // GetInfo(ProgramId); an ini value or the environment still wins
    // the complete RomFS of update 131072 (base and update merged): 252,686 files. Dumps well short of it crash at boot (#64: 157,847 is the
    // base game's RomFS, 221,339 a partial copy); services/fs.cpp names the cause in the crash report. 0 turns the check off.
    if (!nxrt::config_has("fs", "romfs_files")) nxrt::config_set("fs", "romfs_files", "252686");
    NXLOG_I("boot", "fefw.exe build: %s", fefw_cpu::build_name(g_cpu_needs));
    NXLOG_I("boot", "%s (update %s): exefs '%s', main '%s', romfs '%s'", nxrt::game::info().title, nxrt::game::info().version, exefs.c_str(),
            main_nso.c_str(), romfs.c_str());
    // a folder, or a .nup package served as the RomFS image; without either the game aborts in its first fs call (issue 142)
    if (!dir_exists(romfs) && !file_exists(romfs)) die_romfs(romfs);

    bool play_started = false;
    if (nxrt::config_get_bool("boot", "nvn", true)) {
        nxrt::nvn::install();   // nvnBootstrapLoader and the NVN entry points (logging stubs for what no back end implements)
        // [boot] gfx = window (default) | headless (an offscreen image ring; also the default when [gfx] headless is set) | none (logging stubs only)
        const std::string gfxmode = nxrt::config_get_str("boot", "gfx", nxrt::config_get_bool("gfx", "headless", false) ? "headless" : "window");
        if (gfxmode != "none") {
            const bool headless = gfxmode != "window";
            nxrt::play::DisplayConfig disp;
            if (!headless) {
                disp = nxrt::play::display();       // controls.ini: window size, present mode, frame cap
                nxrt::platform::init();
                nxrt::platform::WindowDesc wd;
                wd.title = nxrt::game::info().title;
                wd.width = disp.width;
                wd.height = disp.height;
                if (!nxrt::platform::create_window(wd)) die("cannot create the game window (Vulkan 1.3 driver needed; the reason is in the log)");
            }
            nxrt::gfx::DeviceDesc dd;
            dd.headless = headless ? 1 : 0;
            std::string err;
            g_gfx_device = nxrt::gfx::Device::create(dd, &err);
            if (!g_gfx_device) die("graphics device: %s", err.c_str());
            nxrt::gfx::RendererDesc rd;
            rd.headless_width = static_cast<uint32_t>(nxrt::config_get_int("boot", "headless_width", 1280));
            rd.headless_height = static_cast<uint32_t>(nxrt::config_get_int("boot", "headless_height", 720));
            if (!headless) rd.present_mode = disp.present_mode;
            g_gfx_renderer = nxrt::gfx::Renderer::create(*g_gfx_device, rd, &err);
            if (!g_gfx_renderer) die("renderer: %s", err.c_str());
            nxrt::nvn::BackendDesc bd;
            bd.device = g_gfx_device.get();
            bd.renderer = g_gfx_renderer.get();
            if (!headless) {
                bd.limiter = nxrt::play::limiter();
                bd.keep_aspect = disp.keep_aspect;
            }
            if (!nxrt::nvn::install_backend(bd)) die("NVN back end failed to start (see the log)");
            NXLOG_I("boot", "NVN back end running on %s (%s)", g_gfx_device->gpu().name.c_str(), headless ? "headless" : "window");
            nxrt::play::StartOptions po;
            po.device = g_gfx_device.get();
            po.renderer = g_gfx_renderer.get();
            po.window = !headless;
            po.overlay = !headless;
            nxrt::play::start(po);
            play_started = true;
        }
    }
    if (!play_started) {
        nxrt::play::StartOptions po;           // no graphics: play mode still turns scripted keys into pad input
        po.overlay = false;
        nxrt::play::start(po);
    }

    // 1. map, 2. register
    for (const GameModule& g : kModules) {
        const bool is_main = !std::strcmp(g.file, "main");
        const std::string path = is_main && !main_nso.empty() ? main_nso : exefs + "\\" + g.file;
        if (!file_exists(path)) die("module file %s not found (set [fs] exefs, and [fs] main for the uncompressed main)", path.c_str());
        const uint32_t flags = nso_flags(path);
        if (flags != 0xFFFFFFFFu && (flags & 0x80u))
            die("%s is ZBIC-compressed, which the NSO loader cannot read: set [fs] main to the uncompressed NSO", path.c_str());
        nxrt::LoadOptions lo;
        lo.name = g.file;
        lo.base = g.nx->base;
        std::string err;
        nxrt::Module* m = nxrt::module_load_nso_file(path.c_str(), lo, &err);
        if (!m) die("cannot load %s: %s", path.c_str(), err.c_str());
        if (m->base() != g.nx->base) die("module %s is mapped at a base different from the one the library was lifted for", g.file);
        const nxrt::RegResult r = nxrt::nxmodule_register(m, g.nx, *g.abi);
        if (!r.ok()) die("lifted module %s refused: %s", g.file, r.message.c_str());
        NXLOG_I("boot", "module %s at 0x%llx (%s)", g.file, static_cast<unsigned long long>(m->base()), path.c_str());
    }

    // The game reaches NVN only through `nvnBootstrapLoader` (an import the sdk defines): forced to the runtime's NVN front end, and the
    // sdk's own definition replaced in the call table for indirect calls (pkp_main.cpp, debug note 120).
    if (nxrt::config_get_bool("boot", "nvn", true)) {
        nxrt::module_override_symbol("nvnBootstrapLoader");
        if (const uint64_t def = nxrt::nxmodule_find_export("nvnBootstrapLoader"))
            if (nxrt::HleFn fn = nxrt::hle_lookup("nvnBootstrapLoader")) nxrt::fn_register(def, fn);
    }

    // 3. link: [boot] link = rtld (default: the host maps and protects, the lifted rtld relocates every module) | host (the runtime's linker)
    const std::string link = nxrt::config_get_str("boot", "link", "rtld");
    if (link == "host") {
        if (!nxrt::module_link_all()) die("linking the modules failed (see the log)");
    } else {
        nxrt::module_protect_all();
    }

    if (const std::string spec = nxrt::config_get_str("boot", "press", ""); !spec.empty()) {
        parse_presses(spec);
        g_pad = std::make_shared<nxrt::hid::ManualInputSource>();
        g_pad->connect(0, nxrt::hid::NpadStyle::FullKey);
        nxrt::hid::set_input_source(g_pad);
        CreateThread(nullptr, 0, press_thread, nullptr, 0, nullptr);
    }
    if (nxrt::config_get_int("boot", "screenshot_at", 0) > 0) CreateThread(nullptr, 0, shot_thread, nullptr, 0, nullptr);
    if (const int64_t s = nxrt::config_get_int("boot", "max_seconds", 0); s > 0)
        CreateThread(nullptr, 0, deadline_thread, reinterpret_cast<void*>(static_cast<uintptr_t>(s * 1000)), 0, nullptr);

    // 4. the main thread: main.npdm says 1 MiB of stack, priority 44, core 0 (runtime/games/ObeliskChamber/ObeliskChamber_core.cpp)
    const uint64_t stack = static_cast<uint64_t>(nxrt::config_get_int("boot", "main_stack_kb", 1024)) << 10;
    const uint32_t h = nxrt::kernel::main_thread_adopt(stack, "main");
    if (!h) die("cannot create the main thread");
    ::Ctx* ctx = nxrt::kernel::current_ctx();

    const uint64_t entry = kModules[0].nx->base;
    if (!nxrt::fn_lookup(entry)) die("the lifted rtld has no function at its entry: rebuild the game libraries (config.py extra_entries)");

    // 5. the first instruction: rtld offset 0. x0 = 0 (the exception entry would be non-zero), x1 = the main thread's handle.
    ctx->x[0] = 0;
    ctx->x[1] = h;
    ctx->x[30] = 0;
    NXLOG_I("boot", "entering rtld at 0x%llx (x1 = main thread handle 0x%x)", static_cast<unsigned long long>(entry), h);
    nxrt::dispatch_call(ctx, entry);

    NXLOG_I("boot", "the entry function returned (x0 = 0x%llx): ending the process", static_cast<unsigned long long>(ctx->x[0]));
    nxrt::play::stop();
    nxrt::kernel::main_thread_release();
    nxrt::shutdown();
    return static_cast<int>(ctx->x[0] & 0xff);
}
}  // namespace
