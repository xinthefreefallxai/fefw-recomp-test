# Intel XeSS SDK: licence files

`libxess.dll` (Intel XeSS Super Resolution) is loaded at run time by `src/gfx/extras/temporal_xess.cpp`. No Intel header, library or
binary is in this repository; the runtime declares the few types and entry points it uses from the SDK's documented C ABI.

The release script (`game-build/scripts/make-release.ps1`) downloads the DLL, unmodified, from Intel's repository and checks it:

| File | Source | SHA-256 |
|---|---|---|
| `libxess.dll` (file version 2.0.2.68, signed by Intel Corporation) | https://github.com/intel/xess/raw/v3.0.2/bin/libxess.dll (XeSS SDK 3.0.2, released 2026-07-24) | `251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7` |
| `LICENSE.txt` (Intel Simplified Software License, October 2022) | https://github.com/intel/xess/blob/v3.0.2/LICENSE.txt | `784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df` |
| `third-party-programs.txt` | https://github.com/intel/xess/blob/v3.0.2/third-party-programs.txt | `e8d8899dbfc4de0e8ca25560be9358e03c1405d7c956d20a1838c3c709b9b51c` |

The licence allows redistribution of the binary without modification, with its copyright notice and terms reproduced in the
distribution. This project does not redistribute it: the player downloads `libxess.dll` and puts it in `upscalers\xess\`. Reverse engineering, decompiling
or modifying the DLL is not permitted.

`libxess.dll` links the Microsoft Visual C++ 2015-2022 runtime dynamically (`MSVCP140.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`;
14.40 or newer). The rest of the game does not need it; without it, XeSS is off and the log says so.
