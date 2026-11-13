#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Packs the Linux player package from the Windows one: no game code or data in it. fefw.exe stays a Windows program; on Linux the
# launcher builds it with Linux Zig (same x86_64-windows-gnu target) and starts it through Proton (umu-run) or Wine.
#
#   game-build/scripts/make-release-linux.sh work/release/fefw-recomp-0.1.7-win64.zip
#
# Input: the zip game-build/scripts/make-release.ps1 made. Kept from it as they are: fefw_rt.dll and tools/build/sdk (fefw_rt.lib
# and what links against it), the lifter, dump_hashes.json, fonts, locales, licences. Replaced: the launcher (built here as an
# AppImage), the bundled Python (a relocatable CPython from python-build-standalone with the lifter's packages), Zig (the Linux
# build of the same version) and the shader translator (dotnet publish -r linux-x64). fefw_build.py and extract_corpus.py are
# taken from this checkout.
#
# Run it on x86_64 Linux (WSL2 works) with: node 20+ and npm, Rust (rustup), the .NET 10 SDK, git, curl, unzip, rsync, python3 and
# Tauri's Linux build packages. Debian/Ubuntu (tried on Ubuntu 26.04):
#   sudo apt install build-essential file curl git rsync unzip python3 nodejs npm dotnet-sdk-10.0 \
#       libwebkit2gtk-4.1-dev libxdo-dev libssl-dev libayatana-appindicator3-dev librsvg2-dev
#   curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
# The Ryujinx sources for the translator are cloned into work/refs/ryubing on first use (or RYUBING_DIR).
# Output: <out>/fefw-recomp-<version>-linux-x64.tar.gz (a tarball, so the executable bits survive).
set -euo pipefail

usage() { echo "usage: $0 <fefw-recomp-<version>-win64.zip> [--out DIR] [--version NAME]" >&2; exit 2; }
[ $# -ge 1 ] || usage
WIN_ZIP=$(realpath "$1"); shift
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$ROOT/work/release"
VERSION=$(basename "$WIN_ZIP" | sed -n 's/^fefw-recomp-\(.*\)-win64\.zip$/\1/p')
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT=$(realpath -m "$2"); shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        *) usage ;;
    esac
done
[ -n "$VERSION" ] || { echo "cannot tell the version from $(basename "$WIN_ZIP"): pass --version" >&2; exit 2; }
[ "$(uname -m)" = x86_64 ] || { echo "build on x86_64 Linux: the AppImage and the Python packages are for the machine they are built on" >&2; exit 2; }

# WSL appends the Windows PATH; linuxdeploy (the AppImage step) crashes on an unreadable entry among them (WindowsApps)
if grep -qi microsoft /proc/version 2>/dev/null; then
    PATH=$(echo "$PATH" | tr ':' '\n' | grep -v '^/mnt/' | paste -sd:)
    export PATH
fi

# keep these with make-release.ps1 (Zig) and launcher/src-tauri/src/setup.rs (Ryujinx revision)
ZIG_VERSION=0.17.0
ZIG_SHA256=1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026
PYTHON_MINOR=3.13
PYTHON_TAG=20261026   # python-build-standalone release
PYTHON_BUILD=cpython-3.13.18+$PYTHON_TAG-x86_64-unknown-linux-gnu-install_only.tar.gz
PYTHON_SHA256=126343c54daff4068efc1ac269518d0cc202d57857794948103dd1454e8881be
RYUBING_GIT=https://git.ryujinx.app/ryubing/ryujinx.git
RYUBING_COMMIT=a82350bb774f70fcbd41c9987bf67a3775409963

NAME="fefw-recomp-$VERSION"
CACHE="$OUT/cache"
STAGE="$OUT/stage-linux"
PKG="$STAGE/$NAME"
mkdir -p "$CACHE"
rm -rf "$STAGE"
mkdir -p "$STAGE"

echo "--- the Windows package"
unzip -q "$WIN_ZIP" -d "$STAGE/win"
top=$(find "$STAGE/win" -mindepth 1 -maxdepth 1 -type d | head -1)
[ -f "$top/fefw_rt.dll" ] && [ -f "$top/tools/build/sdk/fefw_rt.lib" ] || { echo "$WIN_ZIP is not a make-release.ps1 package" >&2; exit 1; }
mv "$top" "$PKG"
rm -rf "$STAGE/win" "$PKG/fefw-launcher.exe" "$PKG/tools/python" "$PKG/tools/build/zig" "$PKG/tools/spirv_harness"

echo "--- launcher (AppImage)"
(
    cd "$ROOT/launcher"
    # Rust keeps source paths for panic messages: map the home folder and the checkout to neutral names (as make-release.ps1)
    export RUSTFLAGS="--remap-path-prefix=$HOME=~ --remap-path-prefix=$ROOT=."
    export NO_STRIP=true APPIMAGE_EXTRACT_AND_RUN=1   # linuxdeploy: newer libraries trip its strip; no FUSE needed to build
    npm ci --no-audit --no-fund
    npm run tauri build -- --bundles appimage --config '{"bundle":{"active":true}}'
)
cp "$(ls -t "$ROOT"/launcher/src-tauri/target/release/bundle/appimage/*.AppImage | head -1)" "$PKG/fefw-launcher.AppImage"
chmod +x "$PKG/fefw-launcher.AppImage"

echo "--- shader translator (linux-x64)"
RYU="${RYUBING_DIR:-$ROOT/work/refs/ryubing}"
if [ ! -d "$RYU/src" ]; then
    mkdir -p "$(dirname "$RYU")"
    git clone -q --filter=blob:none --no-checkout "$RYUBING_GIT" "$RYU"
    git -C "$RYU" checkout -q "$RYUBING_COMMIT"
fi
dst="$ROOT/tools/shaders/third_party/ryubing"   # what tools/shaders/scripts/setup_ryujinx.ps1 stages
mkdir -p "$dst/src"
for p in Ryujinx.Common Spv.Generator Ryujinx.Graphics.Shader; do rsync -a --delete --exclude obj --exclude bin "$RYU/src/$p/" "$dst/src/$p/"; done
for f in Directory.Build.props Directory.Packages.props global.json nuget.config LICENSE.txt; do cp "$RYU/$f" "$dst/$f"; done
rm -rf "$OUT/harness-linux"   # always rebuilt, as make-release.ps1 does
dotnet publish "$ROOT/tools/shaders/harness/SpirvHarness.csproj" -c Release -r linux-x64 --self-contained true -o "$OUT/harness-linux" \
    -p:DebugType=none -p:DebugSymbols=false -p:Deterministic=true "-p:PathMap=$ROOT=."
cp -a "$OUT/harness-linux" "$PKG/tools/spirv_harness"
"$PKG/tools/spirv_harness/spirv_harness" --help >/dev/null 2>&1 || [ $? -le 2 ] || { echo "the shader translator does not start" >&2; exit 1; }

echo "--- from this checkout"
cp "$ROOT/game-build/release/fefw_build.py" "$PKG/tools/build/fefw_build.py"
cp "$ROOT/tools/shaders/ampere/extract_corpus.py" "$PKG/tools/shaders/extract_corpus.py"
sed "s/@VERSION@/$VERSION/" "$ROOT/game-build/release/README-linux.txt" > "$PKG/README.txt"

echo "--- Zig $ZIG_VERSION"
zig="$CACHE/zig-x86_64-linux-$ZIG_VERSION.tar.xz"
[ -f "$zig" ] || curl -fsSL -o "$zig.part" "https://ziglang.org/download/$ZIG_VERSION/zig-x86_64-linux-$ZIG_VERSION.tar.xz"
[ -f "$zig" ] || mv "$zig.part" "$zig"
echo "$ZIG_SHA256  $zig" | sha256sum -c --quiet || { rm -f "$zig"; echo "sha256 mismatch: $zig" >&2; exit 1; }
mkdir -p "$PKG/tools/build/zig"
tar xf "$zig" -C "$PKG/tools/build/zig" --strip-components=1 --exclude='*/doc/*'

echo "--- Python ($PYTHON_BUILD)"
py="$CACHE/$PYTHON_BUILD"
[ -f "$py" ] || curl -fsSL -o "$py.part" "https://github.com/astral-sh/python-build-standalone/releases/download/$PYTHON_TAG/${PYTHON_BUILD/+/%2B}"
[ -f "$py" ] || mv "$py.part" "$py"
echo "$PYTHON_SHA256  $py" | sha256sum -c --quiet || { rm -f "$py"; echo "sha256 mismatch: $py" >&2; exit 1; }
tar xf "$py" -C "$PKG/tools"   # unpacks as python/
"$PKG/tools/python/bin/python3" -m pip install --quiet --no-cache-dir --disable-pip-version-check --only-binary=:all: \
    capstone==5.0.9 lz4==4.4.5 zbic==1.0.1.post1 zstandard==0.25.0   # pinned: the same packages in every build
"$PKG/tools/python/bin/python3" -I -c "import zbic, lz4, capstone, zstandard"
# the launcher runs bin/python3 -I only: pip's console scripts carry this machine's absolute path in their shebangs
find "$PKG/tools/python/bin" -maxdepth 1 -type f \( -name 'pip*' -o -name 'idle*' -o -name 'pydoc*' -o -name '2to3*' \) -delete
find "$PKG/tools/python" -name __pycache__ -prune -exec rm -rf {} +
# no symbolic links in the package (#111): they fail on exFAT and NTFS drives (SD cards shared with Windows) and in Windows extractors
# (WinRAR, 7-Zip). Python's are the only ones: terminfo, man pages, pkg-config files and the -config scripts are not used, the linker's
# libpython name and the idle/pydoc links go, and python3.13 (an 81 MB static program: not copied) is renamed to bin/python3, the links
# python3 and python pointed to it and the launcher runs bin/python3 only. Any other link becomes a copy of what it names.
rm -rf "$PKG/tools/python/share/terminfo" "$PKG/tools/python/share/man" "$PKG/tools/python/lib/pkgconfig" "$PKG/tools/python/lib/libpython$PYTHON_MINOR.so"
find "$PKG/tools/python/bin" -maxdepth 1 \( -name 'python*-config' -o -name 'idle*' -o -name 'pydoc*' -o -name '2to3*' \) -delete
rm -f "$PKG/tools/python/bin/python" "$PKG/tools/python/bin/python3"
mv "$PKG/tools/python/bin/python$PYTHON_MINOR" "$PKG/tools/python/bin/python3"
find "$PKG/tools/python" -type l -print0 | while IFS= read -r -d '' l; do
    t=$(readlink -f "$l") || t=""
    rm "$l"
    [ -n "$t" ] && [ -e "$t" ] && cp -a --dereference "$t" "$l"
done
links=$(find "$PKG" -type l)
[ -z "$links" ] || { echo "symbolic links in the package:"; echo "$links"; exit 1; } >&2
"$PKG/tools/python/bin/python3" -I -B -c "import zbic, lz4, capstone, zstandard"   # -B: no __pycache__ (it would carry this machine's paths)
cp "$PKG/tools/python/lib/python$PYTHON_MINOR/LICENSE.txt" "$PKG/licenses/LICENSE.Python.PSF.txt"

echo "--- checks"
# never ship anything from a game dump, a shader cache built from the game, saves or keys
bad=$(find "$PKG" \( -iname '*.nso' -o -iname '*.nro' -o -iname '*.nca' -o -iname '*.nsp' -o -iname '*.xci' -o -iname '*.nup' \
    -o -iname '*.nsz' -o -iname '*.xcz' -o -iname '*.spv' -o -iname '*.keys' -o -iname '*.sav' -o -name fefw.exe -o -name fefw.ini \
    -o -name controls.ini -o -name exefs -o -name romfs -o -name shader_cache -o -name save -o -name saves \) -print)
[ -z "$bad" ] || { echo "game-derived or private files in the package:"; echo "$bad"; exit 1; } >&2
# and nothing that names the machine it was built on: the home folder or the checkout, also inside the AppImage (compressed)
check="$STAGE/appimage-check"
mkdir -p "$check"
(cd "$check" && "$PKG/fefw-launcher.AppImage" --appimage-extract >/dev/null)
leaks=""
for n in "$HOME" "$ROOT"; do
    [ ${#n} -ge 8 ] || { echo "(the path check skips $n: too short to search for)"; continue; }
    leaks+="$(grep -rl --binary-files=text -F "$n" "$PKG" "$check" || true)"$'\n'
done
rm -rf "$check"
[ -z "${leaks//$'\n'/}" ] || { echo "build-machine path ($HOME or $ROOT) in:"; echo "$leaks" | sort -u; exit 1; } >&2

# the same bytes for the same inputs: sorted, owned by root, dated at the commit, no gzip timestamp; nothing writable by others (a stage on
# a Windows drive under WSL reads as mode 777)
tarball="$OUT/$NAME-linux-x64.tar.gz"
when=$(git -C "$ROOT" log -1 --format=%ct 2>/dev/null || echo 0)
tar -C "$STAGE" --sort=name --owner=0 --group=0 --numeric-owner --mode=go-w --mtime="@$when" -cf - "$NAME" | gzip -n -9 > "$tarball"
echo "wrote $tarball ($(( $(stat -c %s "$tarball") / 1048576 )) MiB)"
