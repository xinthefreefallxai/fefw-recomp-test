// SPDX-License-Identifier: GPL-3.0-or-later
// Fire Emblem: Fortune's Weave, the core part of the game module (ObeliskChamber_game.h): the data part plus the title facts. Needs the runtime
// library, not the graphics one.
#include "ObeliskChamber_game.h"

#include "nxrt/services.h"

namespace nxrt::games::ObeliskChamber {

namespace {

// main.npdm of update 131072: main thread 1 MiB / priority 44 / core 0, SystemResourceSize 64 MiB (the SDK's VAMM is on), ThreadInfo
// priorities 28..59 on cores 0..5. Save sizes are placeholders until control.nacp is known (the boot log shows what the game asks for).
void title_defaults(services::TitleProfile& p) {
    p.program_id = data_info().program_id;
    p.name = data_info().title;
    p.display_version = data_info().version;
    p.supported_languages = data_info().languages;
    p.account_save_size = 0x4000000;
    p.account_save_journal = 0x4000000;
    p.device_save_size = 0x4000000;
    p.device_save_journal = 0x4000000;
    p.main_thread_stack = 0x100000;
    p.main_thread_priority = 44;
    p.main_thread_core = 0;
    // main.npdm says 0x4000000, which turns on the SDK's VAMM: the game then reserves 36 GiB (after a 62 GiB probe that maps all free
    // memory once) in the alias region, 8 GiB here. 0 makes the SDK take its other, equally supported path: SetMemoryHeapSize(free memory)
    // + AllocateMemoryBlock (main 0x651EBC), i.e. the runtime's heap section, which NVN pools import from. notes/boot.md, VAMM.
    p.system_resource_size = 0;
    p.firmware = "23.0.0";                     // built with SDK 22.2.0; Eden-NS2, which runs the game, reports 23.0.0
    p.user_nickname = "Player";
    p.user_uid_hi = 0x0000000000000001ull;
    p.user_uid_lo = 0x4645465752636d70ull;     // "FEFWRcmp"
}

}  // namespace

const game::Info& core_info() {
    static const game::Info info = [] {
        game::Info g = data_info();
        g.title_defaults = &title_defaults;
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::ObeliskChamber

void nxrt_register_game_ObeliskChamber_core() { nxrt::game::set_current(&nxrt::games::ObeliskChamber::core_info()); }
