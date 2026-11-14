// SPDX-License-Identifier: GPL-3.0-or-later
// Fire Emblem: Fortune's Weave: who the game is and what its files are called (nxrt/game.h Info). Pure data, no runtime: the launcher
// links this part alone.
#include "ObeliskChamber_game.h"

namespace nxrt::games::ObeliskChamber {

const game::Info& data_info() {
    static const game::Info info = [] {
        game::Info g;
        g.id = "fefw";
        g.title = "Fire Emblem: Fortune's Weave";
        g.product = "Fire Emblem: Fortune's Weave PC";
        g.short_title = "Fortune's Weave";
        g.program_id = 0x04007EF01F8AE000ull;
        g.version = "131072";     // the update's title version; its control.nacp (display version) is not in the dump
        g.main_base = 0x7100000000ull;
        // control.nacp is not in the dump: the romfs Msg folders instead (bits of services::Language): JPja USen EUfr EUde EUit EUes KRko
        // EUen USfr USes CNzh TWzh
        g.languages =(1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 7) | (1u << 12) | (1u << 13) |
                      (1u << 14) | (1u << 15) | (1u << 16);
        g.platform = game::Platform::Switch2;
        g.nvn_api_minor = 40;     // NintendoSDK 22.2.0's NVN2 driver: 231.40, shader binaries up to 201.20
        g.movie_fail_at_open = true;      // its movie player's constructor (main 0x1905920) aborts unless nn::movie::Initialize succeeds
        g.movie_video_decoder = true;     // AV1 movies through the host movie::VideoDecoder (subsdk2's), on the player's CPU-frame path
        g.vi_hdr_status_sdr = true;       // the SDK's answer (format 1, HDR off) gives it 0 nits: NaN tone map and bloom (core/switch2.cpp)
        g.present_interval_hz = 60;       // logic steps 60 / target per frame; the present interval is 60 / target (notes/pacing.md)
        g.exe_name = "fefw.exe";
        g.setup_exe_name = "fefw_setup.exe";
        g.launcher_ini = "fefw_launcher.ini";
        g.game_ini = "fefw.ini";
        g.shader_pack = "shader_cache.fefwshc";
        g.save_folder = "FortunesWeaveRecompPC";
        g.jc2_mouse_default = "auto";  // the PC mouse is the game's Joy-Con 2 mouse, the last device used wins (hid.cpp, ObeliskChamber_mouse.cpp)
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::ObeliskChamber

void nxrt_register_game_ObeliskChamber_data() { nxrt::game::set_current(&nxrt::games::ObeliskChamber::data_info()); }
