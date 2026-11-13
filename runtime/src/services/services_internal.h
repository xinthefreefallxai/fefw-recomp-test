// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to src/services/: the per-service registration hooks that services::register_all() / reset_all() call.
// Each service file defines its own pair; a service that has nothing to reset defines an empty reset.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "nxrt/input_source.h"
#include "nxrt/ipc.h"
#include "nxrt/services.h"

namespace nxrt::services {

// Registers `name` in the ipc registry and records it for registered_names().
void add(const std::string& name, ipc::ServiceFactory f);

// Convenience for "a service made of a command table": builds a Service named `name`, lets `setup` add its commands.
std::shared_ptr<ipc::Service> make_iface(const char* name, const std::function<void(ipc::Service&)>& setup);

// Forwards one stored vibration value to the sink registered with nxrt::hid::set_vibration_sink (input_source.cpp). npad_slot is the input
// slot (0..9), hand 0 = left or the only device, 1 = right. Does nothing when no sink is registered.
void vibration_dispatch(uint32_t npad_slot, int hand, const nxrt::hid::VibrationValue& v);

void register_fs();          void fs_reset();          // fs.cpp          fsp-srv (+ fsp-ldr, fsp-pr stubs)
void register_settings();    void settings_reset();    // settings.cpp    set, set:sys
void register_time();        void time_reset();        // time.cpp        time:u, time:a, time:s, time:r
void register_power();       void power_reset();       // power.cpp       apm, apm:p, apm:am, psm
void register_logfault();    void logfault_reset();    // logfault.cpp    lm, fatal:u, fatal:p, err:u
void register_am();          void am_reset();          // am.cpp          appletOE (appletAE)
void register_acc();         void acc_reset();         // acc.cpp         acc:u0, acc:u1
void register_hid();         void hid_reset();         // hid.cpp         hid
void register_pl();          void pl_reset();          // pl.cpp          pl:u (pl:s)
void register_network();     void network_reset();     // network.cpp     nifm, bsd, ssl, nim, friend, prepo, pctl, bcat, ldn, nsd, ...
void register_misc();        void misc_reset();        // misc.cpp        csrng, ldr:ro, mm:u, hwopus, aoc:u, caps, mii, irs, nfc, nfp, ...
void register_audio();       void audio_reset();       // audio.cpp       audren:u (calls the two below)
void register_audio_out();   void audio_out_reset();   // audio_out.cpp   audout:u, audin:u
std::shared_ptr<ipc::Service> audio_out_manager();   std::shared_ptr<ipc::Service> audio_in_manager();   // for aud:u (Switch 2)
void register_vi_nv();       void vi_nv_reset();       // vi_nv.cpp       vi:u (vi:m, vi:s), nvdrv (nvdrv:a, nvdrv:s)

// common.cpp: add-on content (DLC) the player supplied from their own dump, under [aoc] dir (default "game/dlc"): one folder per add-on
// named by its 16-hex-digit data id (application id + 0x1000 + index), holding romfs/ directly or in one subfolder ("<id>/romfs" or
// "<id>/<n>/romfs"; with several subfolders the highest numbered one is used). Only ids of this title with index 1..0xFFF count.
// Rescanned on every call, sorted by index. aoc:u lists these and fsp-srv OpenDataStorageByDataId serves their RomFS.
struct AddOnContent {
    uint64_t data_id = 0;
    uint32_t index = 0;
    std::string romfs;   // host folder
};
std::string aoc_dir();
std::vector<AddOnContent> installed_aoc();

// common.cpp: RomFS mods under [fs] mods (default "mods"), the layout emulators use, so mods made for them work as they are: any folder
// below it (up to 4 levels) holding romfs/ or exefs/ is a mod ("mods/<name>/romfs", "mods/contents/<title id>/<name>/romfs"). A folder
// whose name starts with '-', '_' or '.' is off, and so is a folder named by another title's id. Sorted by their path below the mods
// folder, ignoring case: when two mods have the same file, the one sorted last wins (01-..., 02-...). exefs/ (code patches) is not
// applied, since the code is compiled ahead of time; such mods are reported with has_exefs (exefs/*.dll code mods are loaded by
// mods_dll.cpp instead, nxrt/mod_api.h). The layout and rules follow Hookshot
// host/mods.c (NotRexed/Hookshot, GPL-3.0-or-later; from SkyboundTotk's loader), so a mods folder works the same in both.
struct RomfsMod {
    std::string name;    // path below the mods folder, '/'-separated
    std::string romfs;   // host folder, empty when the mod has only exefs/
    bool has_exefs = false;
    std::string exefs;   // host folder of exefs/, empty when there is none
};
std::string mods_dir();
std::vector<RomfsMod> installed_mods();

// album.cpp: caps:su (photos saved as PNG into the album folder), registered by register_misc().
std::shared_ptr<ipc::Service> make_screenshot_app();
// The album folder: [album] dir (default "album"), a relative path taken from the exe's folder.
std::filesystem::path album_dir();
// Writes `width` x `height` linear RGBA8 pixels as a 24-bit PNG (alpha dropped). False, with the failing step in `err`, on error.
bool album_write_png(const std::filesystem::path& file, const uint8_t* rgba, uint32_t width, uint32_t height, std::string* err);

}  // namespace nxrt::services
