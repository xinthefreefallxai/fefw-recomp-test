# SPDX-License-Identifier: GPL-3.0-or-later
"""Copy the ExeFS of your own dump to game/dump/exefs, the folder the lifter and ObeliskChamber.exe read, with `main` written uncompressed.

    python -I tools/nso/prepare_exefs.py --exefs <dump>/exefs [--out game/dump/exefs] [--game fefw]

Checks first that main is the build the game's recomp config supports (recomp/games/<game>/config.py, BUILDS), so a different
version is reported in seconds instead of failing an hour into the lift. The dump is only read. `source.txt` in the output records
which ExeFS it came from (the launcher uses it to see that the copy is current).
"""
import argparse
import os
import re
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nso_unpack import unpack     # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FILES = ('main', 'sdk', 'subsdk0', 'subsdk1', 'subsdk2', 'rtld')
OPTIONAL = ('main.npdm',)


def supported_builds(game):
    """BUILDS of recomp/games/<game>/config.py (build id -> version), read as text so nothing of the lifter is imported."""
    text = open(os.path.join(ROOT, 'recomp', 'games', game, 'config.py'), encoding='utf-8').read()
    block = re.search(r'BUILDS\s*=\s*\{(.*?)\}', text, re.S)
    return dict(re.findall(r"'([0-9A-Fa-f]{40})'\s*:\s*'([^']*)'", block.group(1))) if block else {}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exefs', required=True, help="the dump's exefs folder (main, sdk, subsdk0..2, rtld)")
    ap.add_argument('--out', default=os.path.join(ROOT, 'game', 'dump', 'exefs'))
    ap.add_argument('--game', default='fefw')
    a = ap.parse_args()

    missing = [f for f in FILES if not os.path.isfile(os.path.join(a.exefs, f))]
    if missing:
        sys.exit(f'prepare_exefs: {a.exefs} has no {", ".join(missing)}. Pick the exefs folder of the decrypted dump '
                 f'(of the update when you have one).')
    with open(os.path.join(a.exefs, 'main'), 'rb') as f:
        head = f.read(0x60)
    if head[:4] != b'NSO0':
        sys.exit('prepare_exefs: main is not an NSO. The dump must be decrypted (exefs files start with "NSO0").')
    build_id = head[0x40:0x54].hex().upper()
    builds = supported_builds(a.game)
    if builds and build_id not in builds:
        sys.exit(f'prepare_exefs: main has build id {build_id}, which this recomp does not support. Supported: '
                 + ', '.join(f'{b} (version {v})' for b, v in builds.items()) + '. Dump that update of the game.')
    print(f'main: build id {build_id}' + (f' (version {builds[build_id]})' if build_id in builds else ''), flush=True)

    os.makedirs(a.out, exist_ok=True)
    for name in FILES + OPTIONAL:
        src = os.path.join(a.exefs, name)
        if not os.path.isfile(src):
            continue
        dst = os.path.join(a.out, name)
        if name == 'main':
            data = open(src, 'rb').read()
            flags = struct.unpack_from('<I', data, 0x0C)[0]
            out, _ = unpack(data) if flags & 0x87 else (data, flags)
            tmp = dst + '.tmp'
            with open(tmp, 'wb') as f:
                f.write(out)
            os.replace(tmp, dst)
            print(f'main: {"uncompressed" if flags & 0x87 else "copied"} ({len(out)} bytes)', flush=True)
        else:
            shutil.copyfile(src, dst)
            print(f'{name}: copied', flush=True)
    with open(os.path.join(a.out, 'source.txt'), 'w', encoding='utf-8') as f:
        f.write(a.exefs.replace('\\', '/') + '\n' + build_id + '\n')
    print(f'ExeFS ready in {a.out}', flush=True)


if __name__ == '__main__':
    main()
