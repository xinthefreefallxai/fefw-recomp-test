# Configure + build fefw.exe into game-build\fefw\build\<config> (runtime sources are compiled in; the six lifted libraries are imported
# from work\build\fefw). -Target fefw_obj compiles fefw_main.cpp only (a check that needs no lifted libraries). -BuildDir puts the build
# elsewhere (a path relative to game-build\fefw, or absolute), so that several builds can run side by side.
param([string]$Config = "Release", [switch]$Clean, [string]$LibDir = $env:FEFW_LIB_DIR, [string]$Target = "fefw",
      [int]$Jobs = $(if ($env:NX_JOBS) { [int]$env:NX_JOBS } else { 10 }), [string]$CMakeArgs = "", [string]$BuildDir = "")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root "fefw"
$bd = if ($BuildDir) { if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $src $BuildDir } }
      else { Join-Path $src ("build\" + $Config.ToLower()) }
if ($Clean -and (Test-Path $bd)) { Remove-Item -Recurse -Force $bd }
$env_cmd = Join-Path $root "..\runtime\scripts\env.cmd"
$cfg = "cmake -S `"$src`" -B `"$bd`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl"
$sccache = (Get-Command sccache -ErrorAction SilentlyContinue).Source
if (-not $sccache -and $env:NX_TOOLCHAIN -and (Test-Path (Join-Path $env:NX_TOOLCHAIN 'bin\sccache.exe'))) { $sccache = Join-Path $env:NX_TOOLCHAIN 'bin\sccache.exe' }
if ($sccache) {
    # one cache for every build folder, kept small (the disk is nearly full)
    if (-not $env:SCCACHE_DIR) { $env:SCCACHE_DIR = Join-Path $root "..\work\build\sccache" }
    if (-not $env:SCCACHE_CACHE_SIZE) { $env:SCCACHE_CACHE_SIZE = '4G' }
    $sccache = $sccache.Replace('\', '/')
    $cfg += " -DCMAKE_C_COMPILER_LAUNCHER=`"$sccache`" -DCMAKE_CXX_COMPILER_LAUNCHER=`"$sccache`""
}
if ($LibDir) { $cfg += " -DFEFW_LIB_DIR=`"$LibDir`"" }
# ThinLTO follows the flags the lifted libraries were built with (docs/PGO.md): -flto in NX_EXTRA_FLAGS turns on the LTO link options.
$lto = if ($env:NX_EXTRA_FLAGS -match '-flto') { 'ON' } else { 'OFF' }
$cfg += " -DFEFW_THINLTO=$lto -DFEFW_LTO_JOBS=`"$env:NX_LTO_JOBS`""
if ($CMakeArgs) { $cfg += " $CMakeArgs" }
$tgt = if ($Target -eq "fefw_obj") { "CMakeFiles/fefw.dir/src/fefw_main.cpp.obj" } else { $Target }
cmd /c "call `"$env_cmd`" && $cfg && cmake --build `"$bd`" --target $tgt -j $Jobs"
exit $LASTEXITCODE
