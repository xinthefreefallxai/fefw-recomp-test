# Profile-guided build (PGO)

A profile-guided build compiles the lifted game code twice. The first, instrumented build counts which branches and calls the game takes while you
play. The second build uses those counts to lay out and inline the code for the paths that matter. Only the lifted game code is instrumented (the
CPU-heavy part: the game's main thread and job workers); the runtime is compiled as usual.

This needs the **developer build** (docs/BUILDING.md). The release package builds `fefw.exe` with the bundled Zig, which has no profile runtime and
no `llvm-profdata`, so Set up cannot do it.

## What you need

- A working developer build first: the toolchain of docs/BUILDING.md (Visual Studio 2022 Build Tools, LLVM 23 or newer in
  `C:\Program Files\LLVM`, CMake, Ninja, uv, .NET 10), the steps of "By hand, from a clean checkout" done once, and the game running from it.
- LLVM's `llvm-profdata.exe` and its profile runtime (`clang_rt.profile*.lib`). Both come with the LLVM installer. `game-build/fefw/CMakeLists.txt`
  finds the runtime's folder next to the compiler on its own.
- A folder for the profiles **without spaces in its path**, e.g. `C:\pgo`.

Keep everything else the same between the two builds: `recomp/games/fefw/main.sites`, the lifter, and any other flags. A function whose lifted
code changed gets no profile data. The developer build does not apply exefs patches (that is the release package's Set up): put patches you want in
both builds into `main.sites` as `patch` lines.

## 1. Instrumented build

From the repository root, in a normal command prompt:

```
set NX_EXTRA_FLAGS=-fprofile-instr-generate
recomp\scripts\build_game.cmd fefw
pwsh -File game-build\scripts\build_fefw.ps1
```

The changed flags recompile every lifted module (the long step).

## 2. Play to record the profile

Set where the counters go, as a Windows user environment variable (Settings > System > About > Advanced system settings > Environment Variables)
or in the command prompt that starts the game:

```
LLVM_PROFILE_FILE = C:\pgo\fefw-%p.profraw
```

`%p` is the process id, so every session writes its own file. Then play the parts you care about. Battle maps, unit and class menus, the hub,
level-ups and a few area loads give a good mix. Several shorter sessions are as good as one long one. The instrumented game runs slower than
a normal build; that does not spoil the profile.

**End each session by closing the window.** The counters are written when the process ends (the log says
`pgo: profile counters written`). A crash or a killed process writes nothing for that session.

## 3. Merge the profiles

```
"C:\Program Files\LLVM\bin\llvm-profdata.exe" merge -o C:\pgo\fefw.profdata C:\pgo\*.profraw
```

Use the `llvm-profdata` of the same LLVM version that compiled the instrumented build.

## 4. Optimised build

```
set NX_EXTRA_FLAGS=-fprofile-instr-use=C:\pgo\fefw.profdata
recomp\scripts\build_game.cmd fefw
pwsh -File game-build\scripts\build_fefw.ps1
```

Unset `LLVM_PROFILE_FILE` afterwards if you set it for your user (the optimised build writes no profiles; the variable just does nothing).

## Back to a normal build

```
set NX_EXTRA_FLAGS=
recomp\scripts\build_game.cmd fefw
pwsh -File game-build\scripts\build_fefw.ps1
```

`build_game.cmd` passes `NX_EXTRA_FLAGS` on every configure, so an empty value really removes the flags.

## How the counters get written

LLVM's profile runtime writes its counters from an `atexit` handler. This process ends with `TerminateProcess` (closing the window, the self-test,
`[boot] max_seconds`) or `ExitProcess` (the game's own exit), which skip those handlers. The runtime therefore calls `nxrt::profile_flush()`
(`runtime/src/core/pgo.cpp`) just before it ends the process. In a build without the profile runtime that function does nothing:
`__llvm_profile_write_file` resolves to a stub through the linker's `/alternatename`.
