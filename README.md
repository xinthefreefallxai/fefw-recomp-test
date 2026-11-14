# fefw-recomp

> This will probably be the final update.
>
> I started this project to play a game I really like in the best way possible, and it's grown into a lot more than that. To everyone who had a smooth experience: I'm glad you enjoyed it. To anyone who ran into crashes or other problems: every release tried to improve on that, this one most of all. But this is a heavy game to run, and complete compatibility and perfect optimization are an uphill battle.
>
> Thanks to everyone who filed issues, sent contributions and helped test.
>
> I'll be deleting this repo soon. Anyone is free to continue the work.

A native Windows PC port of **Fire Emblem: Fortune's Weave** (Nintendo Switch 2), made by static recompilation. The game's code is
translated to C++ and compiled into a regular Windows program, which runs on a small runtime that stands in for the console's system
software and draws with Vulkan. It is not an emulator.

> [!IMPORTANT]
> **No game files are included or downloadable here.** This project contains only its own tools and runtime. To play, you need your
> own legally dumped copy of the game; everything that reads the game happens on your PC. See [LEGAL.md](LEGAL.md).
>
> Not affiliated with or endorsed by Nintendo or Intelligent Systems.

## Features

- Runs natively on Windows, with no emulator in between
- 60 or 120 FPS modes, any output resolution, render scale and a temporal upscaler
- Ultrawide (21:9, 32:9 or any wider shape): a wider view with the menus and HUD kept at 16:9 in the centre
- Windowed, borderless or fullscreen
- Keyboard or any controller, with matching button prompts
- An in-game settings overlay (**F1**)
- A launcher that sets everything up from your dump, with nothing else to install

## Requirements

- Windows 10 or 11, 64-bit
- A 64-bit CPU with SSE4.2 and POPCNT: an Intel Core i3 / i5 / i7 (2008) or newer, or an AMD FX (2011) / Ryzen or newer. Setup builds the game for your processor: CPUs with FMA3 (Intel Haswell 2013, AMD Piledriver 2012 and newer) get the faster build, older ones such as Sandy Bridge, Ivy Bridge and Xeon E5 v1 / v2 a compatible build that plays the same, a little slower. Older processors (Core 2, AMD Phenom) are not supported; Setup and `fefw.exe` say so instead of crashing
- A GPU with Vulkan 1.3 and an up-to-date driver
- 16 GB of RAM and about 30 GB of free disk space
- Your own decrypted dump of the game at update **131072**, in one of these forms:
  - a folder with `exefs\` and `romfs\`
  - a dump folder with `base\` and `updates\<version>\`
  - a [`.nup`](https://github.com/a55uka/nup) package

## Getting started

1. Download `fefw-recomp-<version>-win64.zip` from the [Releases](../../releases) page and extract it to its own folder.
2. Run `fefw-launcher.exe`.
3. On **Setup**, choose your dump folder (or **.nup** file) and press **Set up**. The launcher checks your files, builds the game on
   your PC and prepares the shaders. The first setup takes a while; you can **Pause** it and continue later.
4. Press **Play**.

Your saves are in the `save\` folder next to the launcher.

### Linux and Steam Deck

Download `fefw-recomp-<version>-linux-x64.tar.gz` instead, extract it and run `fefw-launcher.AppImage`; the steps are the same. The
game runs through Proton: Play needs Steam with a Proton installed, [umu-launcher](https://github.com/Open-Wine-Components/umu-launcher) (`umu-run`) or Wine, or
press **Add to Steam** on the Play page and start it from Steam (also in the Steam Deck's Game Mode). After an update, run Set up in
the launcher (Desktop Mode on a Deck) before playing from Steam.

### Settings

The launcher's **Settings** page covers resolution, frame rate, upscaling, window mode, sound and controls. Press **F1** in game for
the overlay. Every option is described in [runtime/docs/CONFIG.md](runtime/docs/CONFIG.md).

### Game language

**Settings > Language** (or **F1 > Game**) picks the language of the game's text and voices: Japanese, English (US or UK), French,
German, Italian, Spanish, Korean and Chinese (Simplified or Traditional). *Automatic*, the default, follows your Windows display
language and uses English when the game has no translation for it. The change applies the next time the game starts.

### DLSS (NVIDIA RTX only)

NVIDIA's files cannot be shipped here, so DLSS stays off until you add them. Take them from NVIDIA's
[Streamline SDK](https://github.com/NVIDIAGameWorks/Streamline) (v2.x, the `bin/x64` folder of the release, not the development one) and
put these four files in a folder named `upscalers\streamline` next to `fefw-launcher.exe`:

```
sl.interposer.dll   sl.common.dll   sl.dlss.dll   nvngx_dlss.dll
```

Then choose **DLSS** (or **Automatic**, which picks DLSS when the files and an RTX GPU are there) as the Upscaler in **Settings**, and
restart the game. `fefw.log` says `Streamline ... started` and `DLSS (Streamline)` when it works. The folder can be somewhere else:
set it under **Settings > Picture > DLSS (Streamline) folder**.

For DLSS frame generation (RTX 40 or newer, with Windows' hardware-accelerated GPU scheduling on) add five more files from the same
folder: `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `nvngx_dlssg.dll` and `NvLowLatencyVk.dll`.

On an RTX 30 or older GPU, NVIDIA's own files refuse frame generation. Some community mods replace some of these files to run it on
older GPUs. We don't ship or support them. If you use one, put its files in the same `upscalers\streamline` folder, replacing the
originals as the mod says, and set `frame_gen_any_gpu = 1` under `[dlss]` in `fefw.ini` in the run folder (or the environment
variable `NXRT_DLSS_FRAME_GEN_ANY_GPU=1`). The game then no longer turns frame generation off just because Streamline says the GPU
is too old, and the mod's own answer decides. `fefw.log` shows the outcome on its `framegen` and `dlss` lines.

### FSR 3 (any GPU)

FSR 3 needs AMD's signed FidelityFX files, which you download from AMD's
[FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) repository. Put them in `upscalers\fsr` next to
`fefw-launcher.exe` (create the folder):

```
amd_fidelityfx_upscaler_vk.dll   amd_fidelityfx_loader_vk.dll   FSR 3 upscaling (Kits/FidelityFX/signedbin of tag v2.4.0)
amd_fidelityfx_vk.dll                                            FSR 3 frame generation (PrebuiltSignedDLL of tag v1.1.4)
```

Choose **AMD FSR 3 upscaling** as the Upscaler and/or **AMD FSR 3 frame generation** under **Settings > Frame rate**, then restart
the game. Frame generation adds one generated frame between two real ones; the HUD and text are taken from the real frames, so they
stay sharp. It works best with VSync off on a 120 Hz or faster display: with VSync on the game renders at most half the refresh
rate, and on a display slower than twice the game's frame rate generation pauses. **Settings > Picture > FSR 3 (AMD) folder** shows
whether the files are found, and can point to another folder. If a file is missing, `fefw.log` names it and the game carries on with TAAU (or without frame generation).

### XeSS (any GPU, best on Intel Arc)

XeSS needs Intel's `libxess.dll`, which you download from `bin/` of the [XeSS SDK 3.0.2](https://github.com/intel/xess/releases/tag/v3.0.2)
release. Put it in `upscalers\xess` next to `fefw-launcher.exe` (create the folder). It also needs the
[Microsoft Visual C++ 2015-2022 Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe). Then choose **Intel XeSS** as the
Upscaler in **Settings** and restart the game. If the file or the runtime is missing, `fefw.log` says so and the game uses TAAU.

### Keyboard and controller

Any controller works, and so does the keyboard, with no setup. The button prompts in the game follow the one you used last: press a
key and they show keys, touch the controller and they show its buttons. The keyboard drives the Switch buttons the game knows:

| Keys | Switch | |
|---|---|---|
| W A S D | Left stick | Move |
| Arrow keys, I J K L | Right stick | Camera |
| Enter, Space | A | Confirm |
| Esc, Backspace | B | Back |
| R | X | |
| F | Y | |
| Q / E | L / R | E: dash in town |
| Z / C | ZL / ZR | C: map in town |
| Tab | + | Menu |
| M | - | |
| 1 2 3 4, or numpad 8 2 4 6 | D-pad up, down, left, right | |
| V / B | Left / right stick click | |

The movement keys push the stick all the way. **F1** (settings), **F2** (frame rate counter) and **Alt+Enter** (full screen) are
the runtime's own keys. **F1 > Controls** lists the keys as they are now. To leave the game cleanly, use **Quit game** at the
bottom of the F1 menu (click it twice) or close the window (**Alt+F4**): both save the settings and end the game normally.

To change a key, close the game and edit `[bindings.all]` in `controls.ini` in the run folder (the game creates it on its first
start). Each line is `key = action`:

```
[bindings.all]
W = move_forward
Return = btn_a
G = btn_x
X = none
```

Left side: a key name (`Return`, `Space`, `Left Shift`, `Up`, `Keypad 8` ...). Right side: `move_*`, `look_*`, `dpad_*`, `btn_a`,
`btn_b`, `btn_x`, `btn_y`, `btn_l`, `btn_r`, `btn_zl`, `btn_zr`, `btn_plus`, `btn_minus`, `btn_lstick`, `btn_rstick`, or `none` to
switch a key off. An older `controls.ini` gets the keys above added at the next start (only keys you have not bound yourself; the
old file is kept as `controls.ini.bak`). To go back to the default keys, delete the `[bindings.*]` sections. Controller options (stick dead zones,
button mapping, prompts) are under **Settings** and **F1 > Controls**.

### Mouse

The game has its own mouse controls, made for the Switch 2's Joy-Con 2 used as a mouse, and the PC mouse drives them. Move the mouse
or click and the game switches to its mouse controls (its prompts show mouse buttons); press a key or touch the controller and it
switches straight back. Whichever you used last is in control.

- **Bases, dungeons:** the mouse turns the camera, left click talks, opens and attacks, right click cancels or opens the menu, the
  wheel zooms the camera while standing still. Walk with WASD or the stick.
- **Battle maps:** the mouse moves the map cursor (faster when you move it faster), left click selects and confirms, right click
  cancels.
- **Menus:** moving the mouse up or down moves the selection, left click confirms, right click goes back.

While the game is in its mouse controls the Windows pointer is hidden and kept in the window (it comes back where the game's pointer
was). A key, the controller, **Alt+Tab** or **F1** frees it. The game's own **System > Mouse Controls** page shows every button, and
its **System > Settings** set the speeds: the rows marked (Mouse) on the General, Battle and Bases/Dungeons tabs (camera
sensitivity, map cursor speed, menu cursor speed), and **Method for Switching to Mouse**, which should stay automatic. Leave the
game's angle calibration alone (a desk mouse needs none; **Reset Calibration** undoes it). **F1 > Controls > Mouse** has the PC side:
mouse controls automatic, always or off, and the mouse speed before the game's own (`[mouse] jc2_speed` in `controls.ini`).

On battle maps, hold **Y** (keyboard **F**) to move the cursor faster with the stick or keys.

### DLC

DLC is used when you dump it from your own copy, like the game. Put each add-on in `dlc\<title id>\romfs` of your dump folder (next
to `exefs\` and `romfs\`, or next to `base\` and `updates\`). A `.nup` with add-ons is unpacked that way on its own. The **Play**
page lists the DLC it found.

### Mods

RomFS mods go in the `mods\` folder next to the launcher (**Play** > **Open mods folder**), in the layout emulators use:
`mods\<mod name>\romfs\...`. Files in a mod replace the game's files at the same path. If two mods change the same file, the mod
whose name sorts last wins, so you can name them `01-...` and `02-...`. Rename a mod to start with `-` or `_` to turn it off.

Code mods (exefs patches) go in `mods\<mod name>\exefs\`: an `.ips` / IPS32 file named after main's build ID
(`DB93B0125F4850601D61C3EDDE012F8EB2ECFE77.ips`) or a `.pchtxt` with that `@nsobid`. Because the game code is compiled ahead of
time, they are applied when the game is built, not when it starts: after adding, changing or removing one, press **Set up** again
(the Play page says the build is out of date). Patches for the other modules (sdk, subsdk*) are not used, and a patch that touches
an instruction this port hooks makes Set up stop and name it.

Code mods made for this port can also be DLLs in the mod's `exefs\` folder (`mods\<mod name>\exefs\<mod>.dll`), so one mod folder can
carry an emulator's patches and this port's DLL side by side. They load when the game starts and can hook the game at the sites it was
built with. A DLL runs with the same rights as the game: only install mods you trust. `[fs] dll_mods = false` in `fefw.ini` turns them
off. Writing one: [runtime/include/nxrt/mod_api.h](runtime/include/nxrt/mod_api.h).

Mods can be switched on and off in the game: the title screen's DLC entry reads **DLC / Mods** and lists every mod folder on a RomFS
tab and an ExeFS tab. The switch is the file `mod.ini` in the mod's folder (`romfs = on` / `off`, `exefs = on` / `off`; no file means
on), which you can also edit by hand. RomFS files and DLLs follow it at the next start, exefs patches at the next **Set up**.

### Updating to a new release

Extract the new zip to a new folder, copy your `save\` folder (and `mods\`, if you use mods) into it, and run **Set up** again.

## Troubleshooting

- **"These game files are a different version"**: only update 131072 is supported. Check that you dumped the update, not only the
  base game.
- **Setup stopped or failed**: press **Set up** again to continue where it stopped. **Open full log** on the Setup page shows what
  went wrong.
- **The game closes or freezes**: look at `fefw.log` in the launcher's folder, and update your GPU driver.

## Reporting problems

Open an [issue](../../issues) with `fefw.log`, your GPU, driver and Windows version, and the release you used. Please do not attach
game files or anything made from them.

## Building from source

See [docs/BUILDING.md](docs/BUILDING.md). Contributions are welcome.

## Credits

This project stands on the work of others. Thank you.

- [nxrecomp](https://github.com/drakolordx7/nxrecomp) by drakolordx7: the recompiler this is built on.
- [pokopia-recomp](https://github.com/drakolordx7/pokopia-recomp) (drakolordx7) and [featureJosh/pokopia-recomp](https://github.com/featureJosh/pokopia-recomp)
  (featureJosh): the runtime, tools and launcher this project started from (GPL-3.0).
- [drakolordx7/switch2-recomp](https://github.com/drakolordx7/switch2-recomp): the launcher design this launcher follows.
- [nxnative-penis6](https://git.eden-emu.dev/penis6/nxnative-penis6) by seedds (MIT): the movie decoder front end, its dav1d copy and
  several hardware facts (notices in `runtime/THIRD_PARTY_NOTICES.md`).
- [Kompas9's Switch-Game-Patches](https://github.com/Kompas9/Switch-Game-Patches) (MIT): the Fortune's Weave ultrawide patch, whose sites and
  findings `[fefw] ultrawide` re-implements as runtime hooks.
- [Ryujinx / Ryubing](https://git.ryujinx.app/ryubing/ryujinx) (MIT): the shader translator and tiling code; [Mesa](https://gitlab.freedesktop.org/mesa/mesa) NAK (MIT).
- SuperMarioWonderRecomp (MIT); smo-recomp, ScarletNX and ssbu-recomp (GPL-3.0); [yuzu and Eden](https://git.eden-emu.dev/eden-emu/eden)
  (GPL); libnx (ISC); LLVM libc++abi (Apache-2.0 with LLVM exceptions).
- [Hookshot](https://github.com/NotRexed/Hookshot) (GPL-3.0) and SkyboundTotk: the RomFS mod folder layout and rules followed here, so
  mods work the same in both.
- [zbic](https://github.com/kinnay/zbic) by Yannik Marchand (GPLv2; installed by pip at setup, not included here): ZBIC decompression.
- [dav1d](https://code.videolan.org/videolan/dav1d) (BSD-2-Clause), [libvpx](https://chromium.googlesource.com/webm/libvpx) (BSD-3-Clause),
  [Zstandard](https://github.com/facebook/zstd) (BSD), [SDL3](https://github.com/libsdl-org/SDL) (zlib), volk, VulkanMemoryAllocator, Dear ImGui (MIT),
  Vulkan-Headers (Apache-2.0 or MIT).
- [astc-encoder](https://github.com/ARM-software/astc-encoder) (Apache-2.0), [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo)
  (MIT or public domain), ISPC (BSD-3-Clause).
- AMD FidelityFX / FSR SDK headers (MIT), NVIDIA Image Scaling (MIT), NVIDIA Streamline headers (MIT). No AMD or NVIDIA binaries
  are included; FSR and DLSS files are not distributed.
- [Sascha Willems' Vulkan samples](https://github.com/SaschaWillems/Vulkan) (MIT): the stand-in shader test corpus.
- [Kenney input prompts](https://kenney.nl/assets/input-prompts) (CC0), DejaVu fonts, [Tauri](https://tauri.app), [Lucide](https://lucide.dev) icons (ISC).

Licence texts: `runtime/THIRD_PARTY_NOTICES.md`, `recomp/THIRD_PARTY.md`, `tools/*/licenses/`, `tools/textures/THIRD_PARTY.md` and the
licence files inside `runtime/third_party/`. All of these licences are compatible with the GPL-3.0 this project is released under.

## License

GPL-3.0 ([LICENSE](LICENSE)). Third-party code keeps its own licence.
