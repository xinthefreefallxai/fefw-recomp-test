# Building from source

For development. Players should use the zip on the Releases page instead. You need:

- Visual Studio 2022 Build Tools or newer, with "Desktop development with C++" (found with vswhere)
- LLVM 23 or newer (`clang-cl`), in `C:\Program Files\LLVM\bin`, or named by `NX_LLVM` (its bin folder) or `NX_TOOLCHAIN` (a folder with `llvm\bin`)
- CMake 3.25+, Ninja, Git, [uv](https://docs.astral.sh/uv/) (it fetches Python 3.13), .NET 10 SDK, PowerShell 7 (or Windows PowerShell)
- for the launcher: Rust (stable) and Node.js 20+
- optional: NASM 2.14+ (faster movie decoding; on PATH, or `NX_TOOLCHAIN\bin`), sccache

## With the launcher

```
git clone <this repository> fefw-recomp
cd fefw-recomp\launcher
npm install
npm run tauri build -- --no-bundle
```

Start `launcher\src-tauri\target\release\fefw-launcher.exe`. Built inside a checkout, it works on that checkout: the Setup page lists
which build tools it found and runs, in order, with the log on screen:

1. the Python environment (`tools\.venv`)
2. the game code from your dump: `game\dump\exefs` (the console's `main` is ZBIC-compressed and is written uncompressed)
3. the recompile: lift and compile the six modules into `work\build\fefw` (the long step)
4. the texture transcoder sources (astc-encoder, bc7enc_rdo)
5. `fefw.exe` in `game-build\fefw\build\release`
6. the shader translator (fetches Ryujinx's shader code, MIT, at a pinned revision, and builds it)
7. the shaders: found in your romfs and translated to SPIR-V in `work\shaders\spirv`
8. `fefw.ini` in the run folder (`work\play` unless you choose another)

## By hand, from a clean checkout

From the repository root, in a normal command prompt (the scripts set up the Visual Studio environment themselves):

```
:: 1. Python environment
uv venv tools\.venv --python 3.13
uv pip install --python tools\.venv -r recomp\requirements.txt

:: 2. your dump's ExeFS into game\dump\exefs (main written uncompressed; the dump itself is only read)
tools\.venv\Scripts\python -I tools\nso\prepare_exefs.py --exefs D:\dumps\fefw\updates\131072\exefs

:: 3. lift and compile the six modules into work\build\fefw (NX_JOBS = compile processes, default 20)
recomp\scripts\build_game.cmd fefw

:: 4. texture transcoder sources (optional, recommended), then fefw.exe
pwsh -File tools\textures\scripts\fetch_third_party.ps1
pwsh -File game-build\scripts\build_fefw.ps1

:: 5. the shader translator: Ryujinx's sources at the pinned revision, then the harness
git clone --filter=blob:none --no-checkout https://git.ryujinx.app/ryubing/ryujinx.git work\refs\ryubing
git -C work\refs\ryubing checkout a82350bb774f70fcbd41c9987bf67a3775409963
pwsh -File tools\shaders\scripts\setup_ryujinx.ps1 -Ryubing work\refs\ryubing
dotnet build tools\shaders\harness\SpirvHarness.csproj -c Release

:: 6. the shaders of your romfs, translated to SPIR-V
tools\.venv\Scripts\python -I tools\shaders\ampere\extract_corpus.py --romfs D:\dumps\fefw\updates\131072\romfs ^
    --main game\dump\exefs\main --exefs game\dump\exefs --out work\shaders\corpus --jobs 8
dotnet tools\shaders\build\bin\Release\net10.0\spirv_harness.dll --batch work\shaders\corpus --out work\shaders\spirv --jobs 8
```

Then make a run folder with a `fefw.ini` and start `fefw.exe` there:

```ini
[log]
path = fefw.log
[fs]
exefs = C:/src/fefw-recomp/game/dump/exefs
romfs = D:/dumps/fefw/updates/131072/romfs
save_dir = save
[nvn]
shader_cache = C:/src/fefw-recomp/work/shaders/spirv
bc7_cache = C:/src/fefw-recomp/work/bc7_cache
[dlss]
upscaler = taau
[fefw]
fps60 = 1
output = 1440p
render_ratio = 1.0
```

The toolchain can live anywhere: `NX_LLVM` (clang-cl's folder) for the lift, `NX_LLVM_BIN`, `NX_CMAKE_BIN`, `NX_NINJA_BIN`, `NX_VCVARS`
or one `NX_TOOLCHAIN` folder for the runtime (see `runtime\scripts\env.cmd`), and `-DNXRT_NASM=<nasm.exe>` for NASM.

Runtime tests: `pwsh -File runtime\scripts\build.ps1 -Test`.

FSR 3 in a source build: put AMD's DLLs in `upscalers\fsr` next to `fefw.exe` or in the run folder (or name the folder in
`[upscale] fsr_dll`): `amd_fidelityfx_upscaler_vk.dll` and `amd_fidelityfx_loader_vk.dll` from `Kits/FidelityFX/signedbin` of the
[FSR SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) tag v2.4.0 for upscaling, and `amd_fidelityfx_vk.dll` from
`PrebuiltSignedDLL` of tag v1.1.4 for frame generation. `game-build\scripts\make-release.ps1` downloads the same files (sha256 and AMD
signature checked) into the release package; they are never committed.

## Linux package

On Linux the game still runs as `fefw.exe` through Proton or Wine; the launcher, the first-run build and the shader translator run
natively. `fefw_build.py` builds the same `x86_64-windows-gnu` executable with the Linux build of Zig, and Play starts it through
the launcher's **Runner** setting (empty: Steam's own Proton, else `umu-run`, else `wine`). The launcher writes the `fefw.ini` paths as `Z:/...` so they work
under Wine and Proton, also when the game is started from Steam (**Add to Steam** on the Play page adds it as a non-Steam game).
Updating from the launcher works the same way, with the release's `-linux-x64.tar.gz` asset in place of the `-win64.zip`.
A fork can build a launcher that follows its own releases: `FEFW_UPDATE_REPO=<owner>/<repo>` at build time, and `FEFW_BUILD_ID=<tag>`
when its release tags are not version numbers (any other latest tag then counts as an update).

The Linux package is made from the Windows one, after `make-release.ps1`, on x86_64 Linux or WSL2:

```
game-build/scripts/make-release-linux.sh work/release/fefw-recomp-<version>-win64.zip
```

It keeps the runtime (`fefw_rt.dll`, `tools/build/sdk`), the lifter and the rest of the zip, and replaces the launcher (an AppImage
built there), Python (a relocatable CPython from python-build-standalone with the same packages), Zig (same version) and the shader
translator (`dotnet publish -r linux-x64`). It writes `work/release/fefw-recomp-<version>-linux-x64.tar.gz`.

It needs Node 20+, Rust, the .NET 10 SDK and Tauri's Linux packages. On Debian/Ubuntu (tried on Ubuntu 26.04, in WSL2):

```
sudo apt install build-essential file curl git rsync unzip python3 nodejs npm dotnet-sdk-10.0 \
    libwebkit2gtk-4.1-dev libxdo-dev libssl-dev libayatana-appindicator3-dev librsvg2-dev
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

Other distributions need the equivalent packages. In WSL2 the Windows `PATH` is appended to the Linux one; the script drops those
`/mnt/...` entries, because `linuxdeploy` (the AppImage step) crashes on an unreadable one. Build from a checkout on the Linux
filesystem (`~/fefw-recomp`), not `/mnt/c/...`: it is much faster.

## Repository layout

| Folder | What |
|---|---|
| `recomp/` | nxrecomp, the AArch64 to C++ lifter (Python), its CMake glue and the per-game config (`recomp/games/fefw`) |
| `runtime/` | the host runtime: kernel and services, NVN2 on Vulkan, audio, input, movies (`runtime/games/ObeliskChamber`: this game's hooks) |
| `game-build/` | builds `fefw.exe` from the runtime and the lifted libraries |
| `tools/` | `nso/` (dump preparation), `shaders/` (Maxwell/Ampere shader translator), `textures/` (ASTC to BC7), `tiling/` |
| `launcher/` | the Tauri launcher; `launcher/icon/make_icon.py` draws the app icon |

Generated files stay in `game/` and `work/`, which are git-ignored and must never be committed.
