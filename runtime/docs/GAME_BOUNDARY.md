# The game boundary

The runtime, the recompiler and the offline tools were written for one game, Metroid Prime Remastered 1.0.0. This file lists every
place in them that knows about that game, says what kind of knowledge it is, and says where it lives now. The goal is that a second
Switch title can reuse the base unchanged and bring its own module.

Three kinds of game knowledge:

- (a) A generic thing with a Metroid value baked in. The code works for any game; only a number or a name was the game's.
  The value now comes from the game (a field of `nxrt::game::Info`, or a per-game data file in the tools).
- (b) A reusable mechanism with Metroid data. The code is a general technique (patching the frame step at a list of sites,
  matching button icons in the game's textures, factory settings with revisions); the table it works from was the game's. The
  mechanism stays in the base and the table moved to the game module.
- (c) Purely game-specific. Hooks on Metroid functions, Metroid camera and aim behaviour, its menus. Moved whole into the game
  module.

Scope: `runtime/` (base and launcher), `tools/` (setup tool, release builder) and `recomp/` outside `recomp/games/mpr`, which was
already the recompiler's game folder. The game exe (`game-build/`) is the game's own program and stays there.

## The interface

`include/nxrt/game.h` holds one struct, `nxrt::game::Info`, and a registry with one slot: `set_current(const Info*)`, `current()`
and `info()` (an empty Info when nothing is registered, so the base runs without a game and the hooks that need one are skipped with
a log line). It is header-only, so the launcher can use it without linking the runtime.

`Info` has:

| Field | Kind | What the base does with it |
| --- | --- | --- |
| `id`, `title`, `product`, `short_title`, `program_id`, `version` | identity | logs, window titles, the controls.ini header, the launcher's branding and ini comment, the title profile |
| `main_base` | (a) | the default guest base of play mode, timestep and test tooling (`[play]`, `[time]`, `[test] game_base` still override) |
| `nvn_api_minor` | (a) | Switch 2: the version of the NVN2 driver in the game's sdk (231.<minor>); `nvnDeviceGetInteger` answers that driver's constants (shader binary minor 17 or 20, the NVN2-only queries) |
| `movie_fail_at_open` | (a) | Switch 2 without a host video decoder: whether `nn::movie::Initialize` fails (default) or each movie fails to open (`nn::movie::MediaExtractor::SetUriMedia`), for a game whose player aborts on the former; the game then moves on as after a skip |
| `movie_video_decoder` | (a) | Switch 2: the game's player takes `movie::VideoDecoder` from its movie module, and the host one (`src/media/movie_vdec.cpp`, dav1d) decodes AV1 into the player's CPU frames. `[media] movies = skip` falls back to the line above. The game module puts its player on the CPU-frame path (Fortune's Weave: two inline sites, `games/ObeliskChamber/ObeliskChamber_play.cpp`) |
| `present_interval_hz` | (a) | a game that paces itself by the console's vsync and steps its logic by a fixed amount per frame (Fortune's Weave: 60): the frame limiter holds it to this rate / the NVN window's present interval (`gfx/nvn/nvn_present.cpp`, `[nvn] present_interval_hz`); 0 = the interval paces nothing |
| `exe_name`, `setup_exe_name`, `launcher_ini`, `game_ini`, `shader_pack`, `save_folder` | file names | the launcher finds and starts the programs; the game exe reads `game_ini`; services put saves under `%APPDATA%\<save_folder>` |
| `title_defaults(TitleProfile&)` | (a) | the services' title profile (program id, name, version, languages, save sizes, main thread, nickname, user id) |
| `timestep` (`timestep::Sites`) | (b) | the frame-step patch: frame update entry, mid-function sites with their load kind, render timing counter and its wrap, movie update, per-tick sampler |
| `probes` (`tt::GameProbes`) | (c) behind a generic API | test tooling: state readout, warp, grant, item, guest calls, object dump, ray casts, watches |
| `play_defaults` (`play::GameDefaults`) | (b) | factory `controls.ini`, the defaults revision and the bindings added by each revision |
| `play` (`play::GamePlay`) | (c) behind a generic API | play mode's game half: input and camera hooks, display hooks, menus, status, the F1 page's Tweaks tab |
| `prompts` (`play::PromptGame`) | (b) | button prompt icons: the table of icons, how to scan the game's textures, icon size, orientation |

A game module registers its Info before `nxrt::init()`. The Metroid module is `runtime/games/mpr` and comes as three libraries, each
a superset of the one before, so each program links only what it needs:

| Library | Adds | Registered by | Linked by |
| --- | --- | --- | --- |
| `nxrt_game_mpr_data` | identity, names, factory settings (pure data) | `nxrt_register_game_mpr_data()` | the launcher and its tests |
| `nxrt_game_mpr_core` | title profile, timestep sites, test probes (links nxrt) | `nxrt_register_game_mpr_core()` | `nxrt_tests` |
| `nxrt_game_mpr` | play-mode hooks, prompts (links nxrt_gfx and the pak reader) | `nxrt_register_game_mpr()` | `mpr.exe`, `nxrt_gfx_tests` |

The play-mode half talks to the base through two tables in `src/gfx/play/play_game.h`. `GamePlay` is what the game gives the base
(function pointers: `install_render`, `install`, `remove`, `settings_changed`, `frame_begin`, `frame`, `display_live`, `menu_*`,
`status`, `status_text`, `mouse_tab`, `tweaks_tab`, plus the guest address of the frame update hook). `GameHost` is what the base gives the game
(a virtual class implemented in `play.cpp`: current settings, action state, the right stick, pad look, the per-input-tick mouse
handling, aim feedback, pitch limit, log status). Hooks keep their registration order: the base installs the game's render hooks
before its own frame hook and the game's input hooks after it, as before.

CMake: `NXRT_BUILD_GAMES` (on) adds every module listed in `NXRT_GAMES` (default `mpr`), so a second game needs no edit of the base. `NXRT_TEST_GAME` (mpr) picks the module the test programs register.
Its tests run in the base test programs: `games/<id>/tests/core/*.cpp` in `nxrt_tests`, `games/<id>/tests/*.cpp` in `nxrt_gfx_tests`; without a test game they are left out. `NXRT_LAUNCHER_GAME` (mpr) picks the launcher's branding and its `games/<id>/launcher.rc`; when that module is not built, the launcher builds unbranded. The setup tool has `MPRS_GAME` (mpr) and
the release builder reads `tools/release/games/<id>.json` (`MPR_RELEASE_GAME`, default mpr).

## The audit

### runtime: services

| Where (before) | Kind | Now |
| --- | --- | --- |
| `include/nxrt/services.h` TitleProfile defaults: program id 0x010012101468C000, "Metroid Prime Remastered", "1.0.0", owner, account save 4 MiB / journal 1 MiB, nickname "Samus", user id "MPRRecom" | (a) | neutral defaults in the base (0, "", "Player", "nxrtuser"); the Metroid values in `games/mpr/mpr_core.cpp` `title_defaults` |
| `src/services/common.cpp` default language mask (13 languages) | (a) | `title_defaults`; the base falls back to American English |
| main thread stack 0x100000, priority 44, core 0 | (a), also the SDK's defaults | `title_defaults` sets them for Metroid; the base `TitleProfile` keeps the same values as its defaults (the usual Horizon main thread), so they were not removed from `services.h` |
| `src/services/common.cpp` save folder `%APPDATA%\MPRRecomp` | (a) | `Info::save_folder` (fallback "nxrt") |

### runtime: frame step (timestep)

| Where (before) | Kind | Now |
| --- | --- | --- |
| `include/nxrt/timestep.h` `namespace site`: frame update, the dt clamp/loop/floor sites, four budget sites, friction, two pairs of stream sites, render timings, movie update | (b) | mechanism in `src/timestep/timestep.cpp`, which walks a `Sites` table of `MidSite{offset, load, reg, value}`; the Metroid table in `games/mpr/mpr_core.cpp`, the constants in `games/mpr/mpr_sites.h` |
| `kGameBudget` 0.014, friction 3.5 per tick | (a) | `MidSite::value` of those sites |
| `kRenderCounterWrap` 53000 | (a) | `Sites::render_counter_wrap` |

### runtime: play mode

| Where (before) | Kind | Now |
| --- | --- | --- |
| `src/gfx/play/play.cpp` ProcessInput, SetTransform, first-person and ball camera think hooks, player pointer, free look, ball look, beam hints | (c) | `games/mpr/mpr_play.cpp` behind `GamePlay`; the mouse sampling logic that was inside the input hook stays in the base as `GameHost::input_tick` |
| `src/gfx/play/play_aim.*`, `play_lockcam.*`, `play_lookrate.*` (Samus's aim, lock-on camera, look rate) | (c) | `games/mpr/` |
| `src/gfx/play/play_scenefix.*` (S42: the guard on CModConSceneNode render queries, `[game] modcon_query_guard`) and its test | (c) | `games/mpr/`; installed and removed by `games/mpr/mpr_play.cpp` next to the lock-on camera |
| `src/gfx/play/play_glow.*`, `play_glow_ui.cpp` (S44: the glow colour on the morph ball tables, particle generators and incandescence draws) and its test | (c) | `games/mpr/`; installed, set, ticked and removed by `games/mpr/mpr_play.cpp` (after the lock-on camera; the ball's inner glow from the ProcessInput hook); the F1 Tweaks tab through `GamePlay::tweaks_tab` |
| `games/mpr/mpr_fastboot.*` (S46: skip the opening cutscene, the title's intro movie, faster front end) | (c) | was already in `games/mpr`, but compiled into nxrt_gfx and called from play.cpp; now in the `nxrt_game_mpr` library and called from `mpr_play.cpp` |
| `games/mpr/mpr_tint.*` (S45: suit and arm cannon paint, recolouring the game's textures) and its test | (c) | was in `games/mpr`, called from play.cpp; now in `nxrt_game_mpr`, installed, set, ticked and removed by `games/mpr/mpr_play.cpp`. The texture edit mechanism it uses (`include/nxrt/tex_edit.h`, `src/gfx/nvn/nvn_tex_edit.cpp`) is the base's (b) |
| `src/gfx/play/play_fastboot.*`, `play_ui_fastboot.cpp`, `play_tweaks.h`, launcher `page_fastboot.cpp` and `page_tweaks.cpp` (the `[tweaks]` keys, their F1 rows and launcher pages) | (b) | stay in the base: they are the settings file's format and its editors, like `[aim]` and `[camera]`; the keys name Metroid's features (gap below) |
| `src/gfx/play/play_display.cpp` the hooks on the game's resolution, FOV and HUD code | (c) | `games/mpr/display_hooks.cpp`; the display math and the texture filtering settings (`display_tuning`) stay in the base |
| `src/gfx/play/menu_mouse.cpp` the game's menu hooks | (c) | `games/mpr/menu_mouse_game.cpp`; the pointer state and the math stay in the base (`menu_mouse_math.cpp`) |
| `src/gfx/extras/tonemap_hook.*` | (c) | `games/mpr/mpr_tonemap.*` |
| `src/gfx/play/play_ui.cpp` F1 mouse tab: morph ball sliders, wheel cycles beams, lock-on settings | (c) | `games/mpr/mpr_ui.cpp`, called through `GamePlay::mouse_tab` at the same three places |
| `src/gfx/play/play_defaults.cpp` factory controls.ini, defaults revision 2, the BtnX map binding added in revision 2 | (b) | revision mechanism in `play_config.cpp`; the data in `games/mpr/mpr_defaults.cpp` (`GameDefaults`) |
| `src/gfx/play/prompts.cpp`, `prompt_resolve.cpp` the icon table and the Retro pak/TXTR scan | (b) | matching and drawing stay in the base; `games/mpr/mpr_prompts.cpp` gives the table, the scan, the icon size (36) and orientation |
| `cmake/gfx.cmake` `nxrt_mprpak` (the setup tool's pak reader) linked into nxrt_gfx | (c) | built and linked by `games/mpr/CMakeLists.txt` only |

### runtime: test tooling

| Where (before) | Kind | Now |
| --- | --- | --- |
| `src/testtools/tt_probe.cpp` state readout, warp, grant, give item, guest calls, object dump, probe positions, ray casts, watches | (c) | `games/mpr/mpr_tt_probe.cpp` (namespace `nxrt::tt::mpr`), registered as `tt::GameProbes`; `src/testtools/tt_game.cpp` keeps the base address, memory access, `@peek`/`@poke` and forwarders |
| S42's resource loader read (`loader_state`, test warps wait for an idle loader) | (c) | `games/mpr/mpr_tt_probe.cpp`, reached through `GameProbes::loader_state` (`tt::LoaderState` in `nxrt/tt.h`); without it the warp gate opens at once |

### runtime: launcher

| Where (before) | Kind | Now |
| --- | --- | --- |
| "Metroid Prime Remastered PC" window title and dialog captions | (a) | `Info::product` |
| `mpr.exe`, `mpr_setup.exe`, `mpr_launcher.ini`, `shader_cache.mprshc` | (a) | `Info` file names |
| `apps/launcher/launcher.rc`, `launcher.manifest` (version info, icon, manifest) | (c) | `games/mpr/`, picked by `NXRT_LAUNCHER_GAME` |

### game exe

`game-build/src/mpr_main.cpp` is the game's own main. It now calls `nxrt_register_game_mpr()` first and takes its window and
message box titles from `info().title`. Its own names (`mpr.log`, `mpr.ini`, `--version` text) stay: they belong to the game exe.

### tools: setup

| Where (before) | Kind | Now |
| --- | --- | --- |
| `src/pipeline.h` expected title id and build id | (a) | `src/game.h` `GameProfile`, defined in `src/games/mpr.cpp` (`MPRS_GAME`); the options still default to them and the CLI can still override |
| `src/pipeline.cpp` "v1.0.0" in messages and the display version, `mpr.ini`, `shader_cache.mprshc` | (a) | `game().version`, `game_ini`, `shader_pack` |
| `src/container.cpp` "Setup needs Metroid Prime Remastered", "original v1.0.0" | (a) | `game().title`, `game().version` |
| `gui/main_win32.cpp` window title, "Metroid Prime Remastered found" | (a) | `game().product`, `game().title` |
| `gui/setup.rc`, `setup.manifest` | (c) | not moved (gap) |
| texture cache, program scan (`texcache.cpp`, `programs.cpp`, `pak.cpp`, `txtr.cpp`): Retro pak/TXTR formats | (c) | not moved (gap) |

### tools: release builder

| Where (before) | Kind | Now |
| --- | --- | --- |
| `make_release.py` `mpr.exe`, setup exes, launcher, `mpr.ini.example`, `shader_cache.mprshc`, magic `MPRSHC01`, `mpr-<version>-windows-x64` | (a) | `tools/release/games/mpr.json`; the module reads it at import |
| `templates/README.txt`, `HELP.txt` | (c) | not moved (gap) |

### recomp

The recompiler (`nxrecomp/`) has no game knowledge. `0x7100000000` there is the console's load address for main, a platform fact.
`scripts/build_mpr.cmd` is now a wrapper around a generic `scripts/build_game.cmd <id>`, which builds `games/<id>` (target
`<id>_modules`, repository root passed as `NX_ROOT`).

## Gaps

These are still Metroid-shaped in the base. None of them blocks a second game from building; they limit what it can reuse.

- Play-mode vocabulary (done in part). The base's action and context slots are generic: `Ctx` is All, Play, Alt (the alternate body),
  Map, Menu, Inspect; the game actions are Primary, Target, Secondary, Jump, AltForm, LookHold, Map, Pause, Confirm, Back, the selects
  SetA1..4 / SetB1..4 and the cycle SetANext / SetAPrev; `GameFrame` carries `cycle_index` / `cycle_mask`; the engine cycles set A; the
  settings fields are `alt_yaw_rad`, `alt_pitch_rad`, `wheel_cycles`, `alt_orbit`. The game names them through
  `play::GameVocabulary` (in `GameDefaults`; Metroid: `games/mpr/mpr_vocabulary.cpp`): the settings-file name and factory output of every
  action, the context names, the launcher's Keyboard rows and the keys and labels of the alternate-body and cycle settings. Metroid's
  `controls.ini` is unchanged (same names, same keys; the play_config and launcher tests read and write it). Without a game the slots keep
  generic names and the game actions have no factory output. What is still fixed in the base: the number and kinds of slots (four-member
  sets A and B, one alternate body, one inspect context), the `[aim]` (lock-on) and `[camera]` sections, and one launcher sentence on the
  Keyboard page that lists Metroid's contexts.
- `[tweaks]` names Metroid's features. `skip_intro`, `skip_boot_logos`, `fast_menus`, `glow_color` and the per-part glow keys are base
  settings code (`play_fastboot.h`, `play_tweaks.h`) and launcher pages; a second game would want its own tweak list from `GameDefaults`.
- Moved code keeps namespace `nxrt::play`. Done to keep the move mechanical; rename to `nxrt::games::mpr` later.
- Test tooling vocabulary. `tt::GameState` fields (morph, beam, visor, missiles), the hash over them (`tt_core.cpp`) and the item
  names in `tt_script.cpp`.
- Launcher names. Target `mpr_launcher`, library `mpr_launcher_core`, namespace `mprl`, window class `MprLauncherWindow`.
- Base tests register the Metroid module. The service, tt and play tests of the base run with `NXRT_TEST_GAME=mpr`;
  the tests that use Metroid sites or hooks directly (timestep, play aim, lock-on, look rate, display hooks, play hook, camera look) are
  in `games/mpr/tests` and `games/mpr/tests/core`.
- Setup tool. The texture and program stages read Retro's formats; the version resources and the window class name are Metroid's.
- Release templates (README, HELP) and the release's licence notice name the game.
- Docs. Older docs (S*.md) and CONFIG.md still give the old paths of the moved files.
- Shader program list magic `MPRPRG01` (`nvn_gpl.cpp`, setup `programs.cpp`) is a file format name shared by the base and the
  setup tool; harmless, but named after the game.

## Deterministic mode and the New Game stall

A deterministic test boot sat on the planet loading screen after New Game for as long as it ran (25+ minutes). The game boundary was not
the cause: a build from before the boundary work stalls the same way with the same input frames (a frame-pinned script, 16,700 frames at
about 50 fps: still loading), and the boundary build loads and reaches the hangar when deterministic mode is off.

The cause was the test tooling's virtual clock. In deterministic mode `kernel::system_tick` reads a virtual clock that should move 1/60 s per
frame. But every sleep of every guest thread also moved it forward by the sleep's length, and a sleep only yields, so a thread that sleeps
in a loop spun at host speed and pushed the shared clock forward: 150 to 2,000 virtual seconds for every 60 frames. (The profile of a stalled
run points at the game's pad vibration thread, `CNXJoyCon_NPad::StaticVibrationThread`, busy in sleeps and hid requests; which thread
contributed most was not measured.) Under that clock the New Game load never finished. The one earlier run that had
passed was on a busy machine (12 fps): the vibration thread got less CPU, the clock ran at 20 to 50 seconds per 60 frames early on, and the load got
through. The trace (`out/trace.jsonl`, field `t`) shows the rate in every run.

The fix (`src/testtools/tt_core.cpp`, `virtual_sleep`): only the thread that runs the frame boundary moves the clock with its sleeps. Any other
thread waits until the frames move the clock past its wake time, for at most its sleep's length of host time, and does not move it. The clock
now runs at 60 frames per virtual second (frame 600 at 110.0 s, frame 9,000 at 250.0 s), and a deterministic boot reaches the hangar at about
frame 11,400. Deterministic mode is still not bit-exact across runs: movies and loader threads run on host time, so the same frame number can
show different moments in two runs (docs/S5.md, known limits). Compare runs at a game-time point (`@run` after `@area`), not at a frame number.

## Adding a game

1. `runtime/games/<id>/` with a CMakeLists that builds `nxrt_game_<id>_data` (and `_core`, and the full library if it has play
   mode), an `Info` and `nxrt_register_game_<id>*()` functions; `launcher.rc` if it ships the launcher.
2. A game exe that calls the register function before `nxrt::init()`.
3. `recomp/games/<id>/` with a `<id>_modules` target; build it with `scripts\build_game.cmd <id>`.
4. `tools/setup/src/games/<id>.cpp` and `tools/release/games/<id>.json`.
