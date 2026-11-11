# exefs patches for update 131072 (main build ID DB93B0125F4850601D61C3EDDE012F8EB2ECFE77)

Each folder is a mod: copy it into `mods\` next to the launcher (`mods\<name>\exefs\<file>.pchtxt`) and press **Set up**;
fefw_build.py applies exefs patches when it builds the game (see the README's Mods section). The same files work on
emulators that load exefs patches.

| Mod | Address | Change | Effect |
|---|---|---|---|
| `levelup_random_seed` | 0x8ce64c | `ldr w19, [x0, #0x1c0]` -> `mrs x19, cntpct_el0` | The random level-up roll is seeded from the system tick instead of the unit's saved seed, so reloading (or rewinding) before a level-up rolls new stats. Fixed growth and combat RNG are unchanged. |
| `unitinfo_animal_crashfix` | 0x1703498 | `csel x8, xzr, x22, eq` -> `mov x8, x22` | The unit detail page no longer reads through a null class pointer for a unit that has a mount while its class has no dismounted class (crash reading address 0x80, e.g. Io). |

The same changes as `recomp/games/fefw/main.sites` lines, for a build without exefs patch support:

```
patch 0x71008ce64c b941c013 d53be033
patch 0x7101703498 9a9603e8 aa1603e8
```
