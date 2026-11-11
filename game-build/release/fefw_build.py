# SPDX-License-Identifier: GPL-3.0-or-later
"""First-run game build of the release package: turns the player's own dump into fefw.exe on their PC.

    python -I tools/build/fefw_build.py --exefs <dump>/updates/<v>/exefs --game <install folder> [--jobs N] [--keep] [--cpu V]
    python -I tools/build/fefw_build.py --check --game <install folder> [--cpu V]

--cpu picks the build for the processor (CPU_VARIANTS): fma (FMA3 + AVX, the default and faster one) or compat (no FMA3: Sandy
Bridge, Ivy Bridge, Xeon E5 v1/v2, AMD Bulldozer, ...; issue 33). auto (the default; the launcher passes the one it detected) asks the
bundled Zig what this processor has. The variant is part of the build stamp, so a fefw.exe built for the other one is rebuilt.

The package ships no game code. This script
  1. verifies the dump's ExeFS against dump_hashes.json (the supported build: sha256 of every module + main's build ID) and writes
     <game>/exefs: the same NSOs with main uncompressed (the console's main is ZBIC-compressed; tools/build/nso_unpack.py),
  2. lifts the six NSOs with the bundled lifter (recomp/nxrecomp) into <game>/build/lift,
  3. compiles the generated C++ with the bundled Zig (zig c++, mingw target) into <game>/build/obj, in parallel,
  4. links fefw.exe = sdk/fefw_stub.cpp + the lifted objects + sdk/fefw_rt.lib (the runtime, fefw_rt.dll in the package),
  5. deletes the generated sources and objects (unless --keep) and records the result in <game>/build/done.json.

The build stamp (state.json, done.json) is the dump (main build ID, module hashes) plus tool_stamp(): everything in this
package that goes into fefw.exe (lifter sources, main.sites, compile flags and processor variant, the SDK files with fefw_rt.lib). A release that
changes any of them makes an older stamp stale: Set up then lifts, compiles and links again from scratch (the old fefw.exe
stays until the new one is linked over it). --check compares <game>/build/done.json with this package's tools only (no
dump check, well under a second) and prints one JSON line {"current": bool, "reason": text}; exit 0 current, 3 not.

Resumable: a module's lift counts once its marker file is written, an object once it is renamed into place, so a stopped
build continues where it stopped. A file named <game>/build/cancel stops it (the launcher's Pause).
Output for the launcher, one per line on stdout:  @progress <phase> <done> <total> <text>  /  @error <text>
Exit codes: 0 done, 1 error, 2 cancelled.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, FIRST_COMPLETED, wait

HERE = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.join(HERE, 'recomp')
SDK = os.path.join(HERE, 'sdk')
WINDOWS = os.name == 'nt'
# the same Windows target from either host: on Linux the launcher starts fefw.exe through Wine/Proton
ZIG = os.path.join(HERE, 'zig', 'zig.exe' if WINDOWS else 'zig')
FORMAT = 2          # bump when the build layout or the stamp changes: older builds (partial or done) are then started over
# the lifter as game-build/scripts/make-release.ps1 ships it: paths under RECOMP, the same in the zip (tools/build/recomp/...)
# and in the repo (recomp/...)
LIFTER = ['nxrecomp', 'include/nxrecomp', 'games/fefw/CMakeLists.txt', 'games/fefw/config.py']
SITES = 'games/fefw/main.sites'
# what the link takes from sdk/ besides the lifted objects and fefw_rt.lib (stamped on its own, as 'runtime')
SDK_FILES = ['fefw_stub.cpp', 'fefw_rt_api.h', 'fefw_fma.c', 'fefw.rc', 'fefw.manifest', 'fefw.ico']

# name, NSO file, guest base, NxModule.index, translation units (as recomp/games/fefw/CMakeLists.txt)
MODULES = [
    ('main', 'main', 0x7100000000, 0, 153),
    ('sdk', 'sdk', 0x7500000000, 1, 50),
    ('subsdk0', 'subsdk0', 0x7600000000, 2, 5),
    ('subsdk1', 'subsdk1', 0x7640000000, 3, 14),
    ('subsdk2', 'subsdk2', 0x7680000000, 4, 3),
    ('rtld', 'rtld', 0x7700000000, 5, 1),
]
EXEFS_FILES = ['main', 'sdk', 'subsdk0', 'subsdk1', 'subsdk2', 'rtld', 'main.npdm']
# The builds of the game code, one per kind of processor. What the lifted code needs: CMPXCHG16B (the guest's 128-bit exclusive
# pairs, ctx.h) and nothing else beyond x86-64; everything more is speed. fefw_rt.dll is baseline x86-64 (its AVX2 / FMA3 code: the
# BC7 encoder, the UCRT's math, picked by CPUID at run time), so the processor this needs is the game code's.
#   fma     FMA3 (with AVX, which it implies in LLVM): the guest's FMADD / FMLA family is one vfmadd. Intel Haswell (2013), AMD
#           Piledriver (2012) and newer.
#   compat  no FMA3: std::fma / fmaf are calls into sdk/fefw_fma.c (musl's, correctly rounded: the same bits as vfmadd; the fma
#           Zig links otherwise is not exact), so slower in float-heavy code. SSE4.2 and POPCNT keep rounding (FRINT*) and CNT
#           inline. Intel Nehalem (2008) and newer, AMD Bulldozer (2011) and newer: Sandy / Ivy Bridge, Xeon E5 v1 / v2 (issue 33).
# 'needs' is what fefw_rt.dll checks before the first game instruction (fefw_rt_api.h FEFW_CPU_*, the stub passes it on);
# launcher/src-tauri/src/cpu.rs picks the variant by the same rule (variant_for).
CPU_VARIANTS = {
    'fma': {'mcpu': 'x86_64+cx16+fma', 'needs': 'FEFW_CPU_NEEDS_FMA_BUILD', 'features': ('cx16', 'sse4_2', 'avx', 'fma'), 'link': []},
    'compat': {'mcpu': 'x86_64+cx16+sse4_2+popcnt', 'needs': 'FEFW_CPU_NEEDS_COMPAT_BUILD', 'features': ('cx16', 'sse4_2', 'popcnt'),
               'link': ['fefw_fma.c']},
}
# recomp/cmake/NxRecomp.cmake flags for the lifted code, in clang (not clang-cl) syntax, plus NX_INLINE_INDIRECT
# (recomp/games/fefw/CMakeLists.txt); -mcpu per variant. -ffp-contract=off: no multiply and add is fused that the guest kept apart.
CXXFLAGS_COMMON = ['-std=c++20', '-O1', '-w', '-fno-exceptions', '-fno-rtti', '-fno-strict-aliasing', '-ffp-contract=off', '-fno-math-errno',
                   '-fno-threadsafe-statics', '-DNX_INLINE_INDIRECT']


def cxxflags(variant):
    """The compile flags of the lifted code for a CPU_VARIANTS key ('fma': the flags every release before 0.1.7 used)."""
    return ['-target', 'x86_64-windows-gnu', '-mcpu=' + CPU_VARIANTS[variant]['mcpu']] + CXXFLAGS_COMMON
CREATE_NO_WINDOW = 0x08000000
BELOW_NORMAL = 0x00004000


def say(kind, *parts):
    print('@' + kind + ' ' + ' '.join(str(p) for p in parts), flush=True)


def progress(phase, done, total, text):
    say('progress', phase, done, total, text)


class Fail(Exception):
    pass


class Cancelled(Exception):
    pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def tree_hash(root, rels):
    """One sha256 over the files under root/rel for each rel (a file or a folder, walked in sorted order): each file's path
    relative to root and its sha256. Python's bytecode caches are left out (the lifter's own runs write them); a missing
    path counts as missing, so the hash still compares."""
    h = hashlib.sha256()
    for rel in rels:
        top = os.path.join(root, *rel.split('/'))
        if os.path.isfile(top):
            files = [rel]
        elif os.path.isdir(top):
            files = []
            for d, dirs, names in os.walk(top):
                dirs[:] = [x for x in dirs if x != '__pycache__']
                files += [os.path.relpath(os.path.join(d, n), root).replace('\\', '/') for n in names if not n.endswith('.pyc')]
        else:
            h.update(('%s\0missing\0' % rel).encode())
            continue
        for f in sorted(files):
            h.update(('%s\0%s\0' % (f, sha256(os.path.join(root, *f.split('/'))))).encode())
    return h.hexdigest()


# The launcher's SHADER_TRANSLATOR_VERSION (launcher/src-tauri/src/setup.rs): keep the two equal.
SHADER_TRANSLATOR_VERSION = 3


def tool_stamp(variant, patch_stamp=None):
    """The part of the build stamp this package decides: a release that changes any of it rebuilds fefw.exe. The processor
    variant is in it as well, so a fefw.exe built for the other one is rebuilt (the lift is kept: see main). The exefs patches of
    the installed mods (exefs_patch_stamp) are in it only when there are any, so a build without patches keeps its old stamp."""
    stamp = {'lifter': tree_hash(RECOMP, LIFTER), 'sites': tree_hash(RECOMP, [SITES]), 'sdk': tree_hash(SDK, SDK_FILES),
             'runtime': sha256(os.path.join(SDK, 'fefw_rt.lib')), 'cxxflags': cxxflags(variant), 'cpu': variant,
             'modules': [list(m) for m in MODULES]}
    if patch_stamp:
        stamp['exefs_patches'] = patch_stamp
    return stamp


# FEFW_CPU_FORCE_MISSING names (as fefw_rt.dll and the launcher read them) -> Zig's feature names
FORCE_NAMES = {'fma': ('fma',), 'avx': ('avx', 'fma'), 'cx16': ('cx16',), 'sse42': ('sse4_2',), 'sse4.2': ('sse4_2',), 'popcnt': ('popcnt',)}


def host_features():
    """This processor's features as the bundled Zig detects them (`zig targets`: .native .cpu .features), or None when that
    cannot be read. Zig leaves AVX and FMA out when the operating system does not save the YMM registers, as the check in
    fefw_rt.dll does. FEFW_CPU_FORCE_MISSING (a test hook, as for the launcher and fefw_rt.dll) hides features."""
    try:
        out = subprocess.run([ZIG, 'targets'], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL,
                             creationflags=CREATE_NO_WINDOW, timeout=120).stdout.decode('utf-8', 'replace')
    except (OSError, subprocess.SubprocessError):
        return None
    i = out.find('.native')
    j = out.find('.features', i) if i >= 0 else -1
    if j < 0:
        return None
    feats = set(re.findall(r'"([^"]+)"', out[j:out.find('}', j)]))
    if not feats:
        return None
    for name in os.environ.get('FEFW_CPU_FORCE_MISSING', '').split(','):
        feats.difference_update(FORCE_NAMES.get(name.strip().lower(), ()))
    return feats


def pick_variant(want):
    """--cpu (fma / compat / auto) -> (variant, None), or (None, why) when this processor runs neither. auto: FEFW_BUILD_CPU when
    set to a variant, else the fastest one this processor runs; fma when the processor cannot be read."""
    if want in (None, '', 'auto'):
        want = os.environ.get('FEFW_BUILD_CPU', '').strip().lower() or 'auto'
    if want in CPU_VARIANTS:
        return want, None
    if want != 'auto':
        return None, 'unknown --cpu %s (fma, compat or auto)' % want
    feats = host_features()
    if feats is None:
        return 'fma', None
    for v in ('fma', 'compat'):
        if all(f in feats for f in CPU_VARIANTS[v]['features']):
            return v, None
    missing = [f for f in CPU_VARIANTS['compat']['features'] if f not in feats]
    return None, ("This PC's processor does not support %s, which the game needs (an Intel Core i3 / i5 / i7 of 2008 or newer, or "
                  "an AMD FX of 2011 or newer)." % ', '.join(f.replace('cx16', 'CMPXCHG16B').replace('sse4_2', 'SSE4.2').upper() for f in missing))


def check(game, variant):
    """Whether fefw.exe and <game>/build/done.json were built with this package's tools for this processor: (current, reason)."""
    if not os.path.isfile(os.path.join(game, 'fefw.exe')):
        return False, 'fefw.exe is not built yet'
    done = read_json(os.path.join(game, 'build', 'done.json'))
    if not isinstance(done, dict):
        return False, 'fefw.exe has no finished build record'
    if done.get('format') != FORMAT or not isinstance(done.get('tools'), dict):
        return False, 'fefw.exe was built by an older release'
    try:
        bid = json.load(open(os.path.join(HERE, 'dump_hashes.json'), encoding='utf-8'))['main_build_id']
        patch_stamp = exefs_patch_stamp(collect_exefs_patches(game, bid))
    except Fail as e:
        return False, str(e)
    want = tool_stamp(variant, patch_stamp)
    changed = [k for k in sorted(set(want) | set(done['tools'])) if done['tools'].get(k) != want.get(k)]
    if changed == ['exefs_patches']:
        return False, 'the exefs patches in mods changed (Set up applies them)'
    if 'cpu' in changed and done['tools'].get('cpu') in CPU_VARIANTS:
        return False, ('fefw.exe was built for processors with FMA3; this one needs the compatible build' if variant == 'compat'
                       else 'fefw.exe is the compatible build; this processor runs the faster FMA3 build')
    if changed:
        return False, 'this release changes ' + ', '.join(changed)
    shaders = read_json(os.path.join(game, 'shader_cache', 'setup.json'))
    if isinstance(shaders, dict) and shaders.get('translator_version') != SHADER_TRANSLATOR_VERSION:
        return False, 'this release changes the shader translator (shaders are translated again)'
    return True, 'fefw.exe matches this release'


def read_json(path):
    """The JSON in path, or None if it is missing or unreadable (a file cut short by a crash or power loss starts over)."""
    try:
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def write_json(path, value):
    """Write through a temporary file and rename, so a stop mid-write never leaves half a file behind."""
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(value, f, indent=1)
    os.replace(tmp, path)


def verify(exefs):
    want = json.load(open(os.path.join(HERE, 'dump_hashes.json'), encoding='utf-8'))
    missing = [n for n in want['sha256'] if not os.path.isfile(os.path.join(exefs, n))]
    if missing:
        raise Fail('The game files folder %s is missing %s. Pick the exefs of a complete dump.' % (exefs, ', '.join(missing)))
    with open(os.path.join(exefs, 'main'), 'rb') as f:
        head = f.read(0x60)
    if head[:4] != b'NSO0':
        raise Fail('exefs/main is not a decrypted NSO. Extract the game files with your own keys first (decrypted exefs and romfs).')
    bid = head[0x40:0x54].hex()
    if bid != want['main_build_id']:
        raise Fail('This is a different version of the game (main build ID %s). This release supports %s (build ID %s).'
                   % (bid, want['version'], want['main_build_id']))
    names = list(want['sha256'])
    for i, n in enumerate(names):
        progress('verify', i, len(names), 'Checking your game files: %s' % n)
        got = sha256(os.path.join(exefs, n))
        if n == 'main' and got == want.get('main_plain_sha256'):
            continue                     # a dump tool that already wrote main uncompressed
        if got != want['sha256'][n]:
            raise Fail('exefs/%s does not match the supported %s dump (sha256 %s). Re-dump the game, or check that the update %s is installed.'
                       % (n, want['version'], got, want['version']))
    progress('verify', len(names), len(names), 'Game files match %s' % want['version'])
    return want


def write_exefs(exefs, out):
    """<game>/exefs: the dump's NSOs, main uncompressed (the lifter and the runtime read plain NSOs only)."""
    sys.path.insert(0, HERE)
    from nso_unpack import unpack
    os.makedirs(out, exist_ok=True)
    for n in EXEFS_FILES:
        src = os.path.join(exefs, n)
        if not os.path.isfile(src):
            continue
        dst = os.path.join(out, n)
        if n == 'main':
            data = open(src, 'rb').read()
            flags = int.from_bytes(data[12:16], 'little')
            if flags & 0x87:
                progress('verify', 0, 1, 'Unpacking main (ZBIC)')
                data, _ = unpack(data)
            tmp = dst + '.tmp'
            with open(tmp, 'wb') as f:
                f.write(data)
            os.replace(tmp, dst)
        else:
            shutil.copyfile(src, dst)


# ---- exefs patches (mods/<name>/exefs/*.ips, *.pchtxt) ----------------------------------------------------------------------
# The game code is lifted ahead of time, so a patch cannot be applied when the game starts as on an emulator. Set up applies the
# patches of the installed mods to <game>/exefs/main (the plain NSO the lifter and the runtime both read) before lifting: code
# patches are then compiled into fefw.exe, data patches are loaded by the runtime. Only patches for main's build ID are used. The
# patched bytes are part of the build stamp, so adding, changing or removing a patch makes Set up build again. A patch must not touch
# an instruction that main.sites patches or hooks (the hook would run against code it was not written for): Set up stops and names it.
PROGRAM_ID = 0x04007EF01F8AE000
NSO_HEADER = 0x100              # IPS offsets (and pchtxt addresses with @flag offset_shift 0x100) count from the NSO header


def find_mod_dirs(root):
    """The mods as the runtime finds them (runtime/src/services/common.cpp find_mods): a folder below mods/ that has romfs/ or
    exefs/ is a mod; names starting with -, _ or . are off; a 16-digit hex folder that is another title's program ID is skipped.
    Returns [(name, path)] sorted by name, so the mod whose name sorts last wins where two patch the same bytes."""
    out = []

    def walk(d, name, depth):
        if depth > 0 and (os.path.isdir(os.path.join(d, 'romfs')) or os.path.isdir(os.path.join(d, 'exefs'))):
            out.append((name, d))
            return
        if depth >= 4:
            return
        try:
            subs = sorted(os.listdir(d))
        except OSError:
            return
        for sub in subs:
            p = os.path.join(d, sub)
            if not os.path.isdir(p) or not sub or sub[0] in '-_.':
                continue
            if len(sub) == 16 and all(c in '0123456789abcdefABCDEF' for c in sub) and int(sub, 16) != PROGRAM_ID:
                continue
            walk(p, sub if not name else name + '/' + sub, depth + 1)

    if os.path.isdir(root):
        walk(root, '', 0)
    return sorted(out, key=lambda t: t[0].lower())


def bid_matches(text, build_id):
    """An IPS file name or a pchtxt @nsobid against main's build ID (hex, 32 bytes): a prefix of it, the rest zeros, as Atmosphere."""
    t = text.strip().lower()
    full = build_id.lower().ljust(64, '0')          # dump_hashes.json keeps the 20 bytes of the ID; the header field is 32
    if len(t) < 16 or len(t) > 64 or any(c not in '0123456789abcdef' for c in t):
        return False
    return full.startswith(t) and set(full[len(t):]) <= {'0'}


def parse_ips(data, where):
    """IPS (PATCH ... EOF, 3-byte offsets) or IPS32 (IPS32 ... EEOF, 4-byte offsets); RLE records too. -> [(file offset, bytes)]."""
    if data[:5] == b'IPS32':
        osz, end, p = 4, b'EEOF', 5
    elif data[:5] == b'PATCH':
        osz, end, p = 3, b'EOF', 5
    else:
        raise Fail('%s is not an IPS patch (no PATCH or IPS32 header)' % where)
    out = []
    while True:
        if data[p:p + len(end)] == end and p + len(end) >= len(data) - 3:     # the end marker (a truncation size may follow)
            return out
        if p + osz + 2 > len(data):
            raise Fail('%s ends without %s' % (where, end.decode()))
        off = int.from_bytes(data[p:p + osz], 'big')
        size = int.from_bytes(data[p + osz:p + osz + 2], 'big')
        p += osz + 2
        if size == 0:                    # RLE: u16 count, one byte
            if p + 3 > len(data):
                raise Fail('%s: an RLE record is cut short' % where)
            count = int.from_bytes(data[p:p + 2], 'big')
            out.append((off, bytes([data[p + 2]]) * count))
            p += 3
        else:
            if p + size > len(data):
                raise Fail('%s: a record at 0x%x is cut short' % (where, off))
            out.append((off, bytes(data[p:p + size])))
            p += size


def parse_pchtxt(text, where, build_id):
    """IPSwitch .pchtxt: @nsobid-<id>, @flag offset_shift <n>, @enabled / @disabled blocks, @stop, @little-endian / @big-endian,
    `ADDR HEXBYTES` and `ADDR "string"` lines, // comments. -> [(file offset, bytes)], or None when it is for another build."""
    out = []
    shift = 0
    enabled = False
    little = False
    bid = None
    for ln, raw in enumerate(text.splitlines(), 1):
        line = raw.split('//', 1)[0].strip()
        if not line:
            continue
        low = line.lower()
        if low.startswith('@nsobid-'):
            bid = line[8:].strip()
            if not bid_matches(bid, build_id):
                return None
            continue
        if low == '@stop':
            break
        if low == '@enabled':
            enabled = True
            continue
        if low == '@disabled':
            enabled = False
            continue
        if low == '@little-endian':
            little = True
            continue
        if low == '@big-endian':
            little = False
            continue
        if low.startswith('@flag'):
            parts = line.split()
            if len(parts) >= 3 and parts[1].lower() == 'offset_shift':
                shift = int(parts[2], 0)
            continue
        if low.startswith('@'):
            continue                     # other @ lines (e.g. @flag print_values) change nothing here
        m = re.match(r'^([0-9A-Fa-f]{1,8})\s+(.+)$', line)
        if not m:
            continue                     # a title line between blocks
        if not enabled:
            continue
        addr = int(m.group(1), 16)
        val = m.group(2).strip()
        if val.startswith('"'):
            if not val.endswith('"') or len(val) < 2:
                raise Fail('%s:%d: the string is not closed' % (where, ln))
            b = val[1:-1].encode('utf-8').decode('unicode_escape').encode('latin-1')
        else:
            hx = re.sub(r'\s+', '', val)
            if len(hx) % 2 or any(c not in '0123456789abcdefABCDEF' for c in hx):
                raise Fail('%s:%d: %s is not hex bytes' % (where, ln, val))
            b = bytes.fromhex(hx)
            if little:
                b = b[::-1]
        out.append((addr + shift, b))
    if bid is None:
        raise Fail('%s has no @nsobid line, so it cannot be matched to the game version' % where)
    return out


def collect_exefs_patches(game, build_id):
    """The exefs patches of the installed mods for main: [(mod name, file, [(memory offset, bytes)])], in the order they apply.
    IPS files count when their name is main's build ID; pchtxt files when their @nsobid is."""
    found = []
    for name, d in find_mod_dirs(os.path.join(game, 'mods')):
        ex = os.path.join(d, 'exefs')
        if not os.path.isdir(ex):
            continue
        for f in sorted(os.listdir(ex), key=str.lower):
            p = os.path.join(ex, f)
            stem, ext = os.path.splitext(f)
            ext = ext.lower()
            where = '%s/exefs/%s' % (name, f)
            if ext == '.ips':
                if not bid_matches(stem, build_id):
                    continue             # another module or another version
                recs = parse_ips(open(p, 'rb').read(), where)
            elif ext == '.pchtxt':
                recs = parse_pchtxt(open(p, encoding='utf-8-sig', errors='replace').read(), where, build_id)
                if recs is None:
                    continue
            else:
                continue
            mem = []
            for off, b in recs:
                if off < NSO_HEADER:
                    raise Fail('%s: offset 0x%x is inside the NSO header (patches start at 0x100)' % (where, off))
                if b:
                    mem.append((off - NSO_HEADER, b))
            if mem:
                found.append((name, where, mem))
    return found


def exefs_patch_stamp(patches):
    """One hash over what the patches write, in order (a changed disabled block or comment does not build again)."""
    if not patches:
        return None
    h = hashlib.sha256()
    for name, where, mem in patches:
        h.update(('%s\0' % where).encode())
        for off, b in mem:
            h.update(off.to_bytes(8, 'little') + len(b).to_bytes(4, 'little') + b)
    return h.hexdigest()


def site_addresses():
    """main.sites: {module offset: kind} of every site but noinline (patch, inline, midhook, entryhook, dynimm, jtable)."""
    out = {}
    try:
        lines = open(os.path.join(RECOMP, *SITES.split('/'))).read().splitlines()
    except OSError:
        return out
    for line in lines:
        t = line.split('#', 1)[0].split()
        if len(t) >= 2 and t[0] != 'noinline':
            out[int(t[1], 16) - MODULES[0][2]] = t[0]
    return out


def apply_exefs_patches(main_path, patches):
    """Write the patches into the plain main NSO (uncompressed segments) and fix the segment hashes in its header."""
    if not patches:
        return
    data = bytearray(open(main_path, 'rb').read())
    flags = int.from_bytes(data[12:16], 'little')
    if flags & 7:
        raise Fail('exefs/main has LZ4-compressed segments; exefs patches need the plain main Set up writes')
    segs = []
    for i in range(3):
        foff, moff, size = (int.from_bytes(data[0x10 + 0x10 * i + k:0x14 + 0x10 * i + k], 'little') for k in (0, 4, 8))
        segs.append((foff, moff, size))
    sites = site_addresses()
    touched = set()
    owner = {}
    for name, where, mem in patches:
        for off, b in mem:
            seg = next((i for i, (fo, mo, sz) in enumerate(segs) if mo <= off and off + len(b) <= mo + sz), None)
            if seg is None:
                raise Fail('%s: 0x%x..0x%x is outside main\'s text, rodata and data' % (where, off, off + len(b)))
            for a in range(off & ~3, off + len(b), 4):
                if a in sites:
                    raise Fail('%s patches 0x%x, where this release has a main.sites %s site: the two cannot be combined. '
                               'Remove the patch or ask for the site to be moved.' % (where, a, sites[a]))
            clash = next((owner[a] for a in range(off, off + len(b)) if owner.get(a, where) != where), None)
            if clash:
                progress('verify', 0, 1, 'Note: %s overwrites bytes %s patched at 0x%x (the later mod wins)' % (where, clash, off))
            for a in range(off, off + len(b)):
                owner[a] = where
            fo, mo, _ = segs[seg]
            data[fo + off - mo:fo + off - mo + len(b)] = b
            touched.add(seg)
    for i in touched:
        fo, _, sz = segs[i]
        data[0xA0 + 0x20 * i:0xC0 + 0x20 * i] = hashlib.sha256(data[fo:fo + sz]).digest()
    tmp = main_path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, main_path)
    n = sum(len(m) for _, _, m in patches)
    progress('verify', 1, 1, 'Applied %d exefs patch record(s) from %s' % (n, ', '.join(sorted({w for _, w, _ in patches}))))


def popen(cmd, **kw):
    if WINDOWS:
        kw['creationflags'] = CREATE_NO_WINDOW | BELOW_NORMAL
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **kw)


class Runner:
    """Runs tool processes; kills them all when the cancel file appears."""

    def __init__(self, cancel_file):
        self.cancel_file = cancel_file
        self.procs = set()
        self.lock = threading.Lock()
        self.cancelled = False
        threading.Thread(target=self._watch, daemon=True).start()

    def _watch(self):
        while True:
            if os.path.exists(self.cancel_file):
                self.cancelled = True
                with self.lock:
                    for p in list(self.procs):
                        try:
                            p.kill()
                        except OSError:
                            pass
            time.sleep(0.3)

    def check(self):
        if self.cancelled or os.path.exists(self.cancel_file):
            self.cancelled = True
            raise Cancelled()

    def run(self, cmd, env=None, cwd=None, on_line=None):
        self.check()
        p = popen(cmd, env=env, cwd=cwd)
        with self.lock:
            self.procs.add(p)
        tail = []
        for raw in p.stdout:
            line = raw.decode('utf-8', 'replace').rstrip()
            tail.append(line)
            del tail[:-30]
            if on_line:
                on_line(line)
        rc = p.wait()
        with self.lock:
            self.procs.discard(p)
        self.check()
        return rc, '\n'.join(tail)


def lift(runner, exefs, gen, jobs):
    """One lifter process per module (the lifter itself runs `jobs` worker processes)."""
    sites = os.path.join(RECOMP, 'games', 'fefw', 'main.sites')
    for k, (name, nso, base, index, shards) in enumerate(MODULES):
        out = os.path.join(gen, name)
        marker = os.path.join(out, '.lifted')
        if os.path.isfile(marker):
            continue
        progress('lift', k, len(MODULES), 'Translating the game code: %s (%d of %d)' % (name, k + 1, len(MODULES)))
        if os.path.isdir(out):
            shutil.rmtree(out)
        os.makedirs(out)
        args = ['lift', os.path.join(exefs, nso), out, '--name', name, '--base', hex(base), '--shards', str(shards),
                '--jobs', str(jobs), '--game', 'fefw', '--index', str(index)]
        if name == 'main':
            args += ['--sites', sites]
        cmd = [sys.executable, '-I', os.path.abspath(__file__), '--lift-one', json.dumps(args)]
        rc, tail = runner.run(cmd, cwd=RECOMP)
        if rc != 0:
            raise Fail('Translating %s failed (exit %d):\n%s' % (name, rc, tail))
        open(marker, 'w').write('ok\n')
    progress('lift', len(MODULES), len(MODULES), 'Game code translated')


def free_memory():
    """Available physical memory in bytes, or None."""
    if not WINDOWS:
        try:
            with open('/proc/meminfo') as f:
                for line in f:
                    if line.startswith('MemAvailable:'):
                        return int(line.split()[1]) * 1024
        except (OSError, ValueError, IndexError):
            pass
        return None
    try:
        import ctypes

        class MS(ctypes.Structure):
            _fields_ = [('len', ctypes.c_uint32), ('load', ctypes.c_uint32)] + [(n, ctypes.c_uint64) for n in
                        ('total', 'avail', 'pagetotal', 'pageavail', 'virttotal', 'virtavail', 'extavail')]
        m = MS(len=ctypes.sizeof(MS))
        return m.avail if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)) else None
    except (AttributeError, OSError):
        return None


def compile_all(runner, gen, obj, jobs, env, variant):
    units = []
    for name, _, _, _, shards in MODULES:
        d = os.path.join(gen, name)
        for f in sorted(os.listdir(d)):
            if f.endswith('.cpp'):
                units.append((name, os.path.join(d, f)))
    total = len(units)
    todo = []
    for name, src in units:
        o = os.path.join(obj, name, os.path.basename(src)[:-4] + '.o')
        if not os.path.isfile(o):
            todo.append((name, src, o))
    done = total - len(todo)
    progress('compile', done, total, 'Compiling: %d of %d' % (done, total))
    for name, _, _, _, _ in MODULES:
        os.makedirs(os.path.join(obj, name), exist_ok=True)
    # biggest first, so the long ones do not end up last on a single core
    todo.sort(key=lambda t: -os.path.getsize(t[1]))

    def one(t):
        name, src, o = t
        tmp = o + '.tmp'
        cmd = [ZIG, 'c++'] + cxxflags(variant) + ['-I', os.path.join(RECOMP, 'include'), '-I', os.path.dirname(src), '-c', src, '-o', tmp]
        rc, tail = runner.run(cmd, env=env)
        if rc != 0:
            return 'Compiling %s/%s failed (exit %d):\n%s' % (name, os.path.basename(src), rc, tail)
        try:
            replace_retry(tmp, o)
        except OSError as e:     # a failed unit, retried one at a time below, not an abort of the whole build
            return 'Compiling %s/%s: %s' % (name, os.path.basename(src), e)
        return None

    # a big unit takes a GB or two in clang; with too many at once memory runs out and clang dies on every unit with
    # exit 541541187 (0x20474343: an uncaught C++ exception, std::bad_alloc)
    # ponytail: 2 GiB per job is a guess from the unit sizes; measure if machines with plenty of RAM get capped
    avail = free_memory()
    if avail:
        jobs = max(1, min(jobs, avail // (2 << 30)))
    failed = []
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        pending = {ex.submit(one, t): t for t in todo}
        try:
            while pending:
                fin, _ = wait(pending, return_when=FIRST_COMPLETED)
                for f in fin:
                    t = pending.pop(f)
                    if f.result():
                        failed.append(t)
                        continue
                    done += 1
                    progress('compile', done, total, 'Compiling: %d of %d' % (done, total))
        except BaseException:
            for f in pending:
                f.cancel()
            raise
    for t in failed:                     # again one at a time with all the memory: only a failure here is real
        err = one(t)
        if err:
            raise Fail(err)
        done += 1
        progress('compile', done, total, 'Compiling: %d of %d' % (done, total))


def replace_retry(src, dst):
    """os.replace, retried for a few seconds: an antivirus scanning a freshly written file holds it open (PermissionError)."""
    delay = 0.1
    while True:
        try:
            os.replace(src, dst)
            return
        except PermissionError:
            if delay > 3.2:
                raise
            time.sleep(delay)
            delay *= 2


def clear_high_entropy_va(exe):
    """fefw.exe keeps host allocations low (the guest ranges at 0x71_0000_0000 must stay free): no high-entropy ASLR,
    as /HIGHENTROPYVA:NO does for the clang-cl build. zig's linker has no flag for it, so clear the PE bit."""
    with open(exe, 'r+b') as f:
        f.seek(0x3c)
        pe = int.from_bytes(f.read(4), 'little')
        f.seek(pe + 24)
        if f.read(2) != b'\x0b\x02':
            raise Fail('fefw.exe is not a PE32+ image')
        off = pe + 24 + 70
        f.seek(off)
        dc = int.from_bytes(f.read(2), 'little') & ~0x20
        f.seek(off)
        f.write(dc.to_bytes(2, 'little'))


def link(runner, obj, game, env, variant):
    progress('link', 0, 1, 'Linking fefw.exe')
    libs = []
    for name, _, _, _, _ in MODULES:
        a = os.path.join(obj, 'fefw_%s.a' % name)
        if os.path.isfile(a):
            os.remove(a)
        objs = sorted(os.path.join(obj, name, f) for f in os.listdir(os.path.join(obj, name)) if f.endswith('.o'))
        rsp = os.path.join(obj, '%s.rsp' % name)
        # UTF-8, not the ANSI code page: zig ar reads the response file as UTF-8, so a game folder path with non-ASCII
        # characters (an accented user name) failed the archive step.
        with open(rsp, 'w', encoding='utf-8') as f:
            f.write('\n'.join('"%s"' % p.replace('\\', '/') for p in objs))
        rc, tail = runner.run([ZIG, 'ar', 'rcs', a, '@' + rsp], env=env)
        if rc != 0:
            raise Fail('Archiving %s failed (exit %d):\n%s' % (name, rc, tail))
        libs.append(a)
    # the variant's own sources (compat: sdk/fefw_fma.c), as C and ahead of the libraries, so their fma / fmaf are the ones linked
    extra = []
    for src in CPU_VARIANTS[variant]['link']:
        o = os.path.join(obj, os.path.splitext(src)[0] + '.o')
        rc, tail = runner.run([ZIG, 'cc', '-target', 'x86_64-windows-gnu', '-mcpu=' + CPU_VARIANTS[variant]['mcpu'], '-O1', '-ffp-contract=off',
                               '-fno-math-errno', '-c', os.path.join(SDK, src), '-o', o], env=env)
        if rc != 0:
            raise Fail('Compiling %s failed (exit %d):\n%s' % (src, rc, tail))
        extra.append(o)
    tmp = os.path.join(obj, 'fefw.exe')
    # FEFW_CPU_NEEDS: what fefw_rt.dll checks the processor for before the game starts (fefw_rt_api.h)
    cmd = [ZIG, 'c++', '-target', 'x86_64-windows-gnu', '-mcpu=x86_64+cx16', '-O1', '-fno-exceptions', '-fno-rtti', '-I', SDK,
           '-DFEFW_CPU_NEEDS=' + CPU_VARIANTS[variant]['needs'], os.path.join(SDK, 'fefw_stub.cpp'), os.path.join(SDK, 'fefw.rc')] +         extra + libs + [os.path.join(SDK, 'fefw_rt.lib'), '-o', tmp, '-Wl,--stack,134217728', '-Wl,--subsystem,windows']
    rc, tail = runner.run(cmd, env=env, cwd=SDK)
    if rc != 0:
        raise Fail('Linking fefw.exe failed (exit %d):\n%s' % (rc, tail))
    clear_high_entropy_va(tmp)
    dst = os.path.join(game, 'fefw.exe')
    replace_retry(tmp, dst)
    progress('link', 1, 1, 'fefw.exe built')
    return dst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exefs')
    ap.add_argument('--game')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument('--keep', action='store_true', help='keep the generated sources and objects')
    ap.add_argument('--check', action='store_true', help='only say whether <game>/build/done.json matches these tools')
    ap.add_argument('--cpu', default='auto', help='the build for the processor: fma, compat or auto (CPU_VARIANTS)')
    ap.add_argument('--lift-one', help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.lift_one:                       # child process: one lifter run (keeps the lifter's own pool out of this process)
        sys.path.insert(0, RECOMP)
        from nxrecomp.__main__ import main as nx_main
        return nx_main(json.loads(a.lift_one)) or 0
    if a.check:
        if not a.game:
            ap.error('--check needs --game')
        variant, why = pick_variant(a.cpu)
        try:
            current, reason = check(os.path.abspath(a.game), variant) if variant else (False, why)
        except OSError as e:                 # a tool file that cannot be read: Set up then runs and names it
            current, reason = False, 'cannot read the build tools: %s' % e
        print(json.dumps({'current': current, 'reason': reason}), flush=True)
        return 0 if current else 3
    if not a.exefs or not a.game:
        ap.error('--exefs and --game are required')
    if not WINDOWS:
        try:
            os.nice(10)                  # the BELOW_NORMAL of the Windows build: the lifter and the compilers inherit it
        except OSError:
            pass
    game = os.path.abspath(a.game)
    build = os.path.join(game, 'build')
    os.makedirs(build, exist_ok=True)
    cancel_file = os.path.join(build, 'cancel')
    if os.path.exists(cancel_file):
        os.remove(cancel_file)
    t0 = time.time()
    times = {}
    try:
        if not os.path.isfile(ZIG):
            raise Fail('The bundled compiler is missing (%s). Extract the release zip again.' % ZIG)
        variant, why = pick_variant(a.cpu)
        if not variant:
            raise Fail(why)
        if variant == 'compat':
            progress('verify', 0, 1, 'Building the compatible version of the game (for processors without FMA3)')
        want = verify(a.exefs)
        plain = os.path.join(game, 'exefs')
        write_exefs(a.exefs, plain)
        patches = collect_exefs_patches(game, want['main_build_id'])
        apply_exefs_patches(os.path.join(plain, 'main'), patches)
        stamp = {'format': FORMAT, 'main_build_id': want['main_build_id'], 'sha256': want['sha256'],
                 'tools': tool_stamp(variant, exefs_patch_stamp(patches))}
        state_p = os.path.join(build, 'state.json')
        # another dump or another release (or no readable state): start over. Removing lift/ drops the modules' .lifted
        # markers too, so a changed lifter or main.sites lifts everything again, not only recompiles
        if read_json(state_p) != stamp:
            for d in ('lift', 'obj'):
                shutil.rmtree(os.path.join(build, d), ignore_errors=True)
            write_json(state_p, stamp)
        times['verify'] = round(time.time() - t0, 1)
        runner = Runner(cancel_file)
        env = dict(os.environ)
        env['ZIG_GLOBAL_CACHE_DIR'] = os.path.join(build, 'zig-cache')
        env['ZIG_LOCAL_CACHE_DIR'] = os.path.join(build, 'zig-cache')
        gen, obj = os.path.join(build, 'lift'), os.path.join(build, 'obj')
        t = time.time()
        lift(runner, plain, gen, a.jobs)
        times['lift'] = round(time.time() - t, 1)
        t = time.time()
        compile_all(runner, gen, obj, a.jobs, env, variant)
        times['compile'] = round(time.time() - t, 1)
        t = time.time()
        exe = link(runner, obj, game, env, variant)
        times['link'] = round(time.time() - t, 1)
        if not a.keep:
            progress('finish', 0, 1, 'Removing the build files')
            for d in ('lift', 'obj', 'zig-cache'):
                shutil.rmtree(os.path.join(build, d), ignore_errors=True)
        done = dict(stamp, exe_sha256=sha256(exe), seconds=times, finished=time.strftime('%Y-%m-%d %H:%M:%S'))
        write_json(os.path.join(build, 'done.json'), done)
        progress('finish', 1, 1, 'fefw.exe is ready (%d s)' % (time.time() - t0))
        return 0
    except Cancelled:
        say('cancelled', 'Build paused. Set up again continues where it stopped.')
        return 2
    except Fail as e:
        say('error', str(e).replace('\n', '\\n'))
        return 1
    except Exception as e:            # an unexpected failure: say what it was, plainly
        say('error', '%s: %s' % (type(e).__name__, str(e).replace('\n', '\\n')))
        return 1


if __name__ == '__main__':
    sys.exit(main())
