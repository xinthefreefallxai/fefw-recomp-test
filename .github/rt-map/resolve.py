# Temporary: name the fefw_rt.dll addresses of a VTune export (preferred base 0x180000000) from the linker map, as CI notices.
import re, subprocess, sys, hashlib
mp, dll, undname = sys.argv[1], sys.argv[2], sys.argv[3]
addrs = [int(a, 16) for a in open(sys.argv[4]).read().split()]
syms = []
pat = re.compile(r'^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+(?:f\s+)?(?:i\s+)?(\S+)?')
for line in open(mp, errors='replace'):
    m = pat.match(line)
    if m: syms.append((int(m.group(2), 16), m.group(1), m.group(3) or ''))
syms.sort()
print(f"::notice title=map::{len(syms)} symbols; dll sha256 {hashlib.sha256(open(dll,'rb').read()).hexdigest()}")
import bisect
keys = [s[0] for s in syms]
for a in addrs:
    i = bisect.bisect_right(keys, a) - 1
    if i < 0: print(f"::notice title={a:#x}::before first symbol"); continue
    va, name, obj = syms[i]
    try: dem = subprocess.run([undname, name], capture_output=True, text=True).stdout.strip().splitlines()[-1]
    except Exception: dem = name
    dem = dem.replace('::', ' :: ').replace('%', '%25')[:400]
    print(f"::notice title={a:#x}::{dem} +{a-va:#x} [{obj}]")
