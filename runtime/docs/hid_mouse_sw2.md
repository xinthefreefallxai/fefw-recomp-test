# Switch 2 Joy-Con 2 mouse (Nmouse / Jcms) and hid cmd 214

Found by reading the SDK 20.5 client in Pokopia 2.0.1a (`sdk` mapped at 0x7500000000, `main` at 0x7100000000). No public Switch 2
emulator source covers it: `refs/pound` and `refs/Instaurix` have no Nmouse/Jcms code, and Switchbrew lists none of these commands
(checked Nov 2026). Confidence notes: **confirmed** = read in the client code; **inferred** = a guess that fits what the code does.

## What the game does (main)

`sub_7101124DE0` (input setup):

```
buf = alloc(0xC000, align 0x1000); nn::hid::InitializeNmouse(buf, 0xC000, 1920, 1080)
buf = alloc(0x4000, align 0x1000); nn::hid::InitializeJcms(buf, 0x4000)
nn::hid::SetMergedNmouseSourceTypes(6)                  // JoyLeftJcms | JoyRightJcms
SetNmouseButtonConfigJoyLeftJcms ({1,2,4,3,0,5,0,0})
SetNmouseButtonConfigJoyRightJcms({1,2,4,5,3,0,0,0})
```

`sub_7101125F40` (every frame, from the pad update `sub_7101125680`): unless a "merged" flag is set it calls
`GetNmouseHandles(out, 2, npad 0)` and `(…, npad 1)`; for each handle found it takes the two Jcms handles of that npad
(`GetJcmsHandle(npad, 0/1)`) and calls `StartJcms` on both (so StartJcms runs every frame while a mouse is listed). It then reads
`GetNmouseStates(…, 16, handle)` for up to two mice, keeps the newest sampling number, and sums `state+0x20` / `state+0x24` of the new
states into two counters clamped to ±600.

`sub_71011340C0(reader, out, right)`: picks the listed mouse whose `GetNmouseTypeSet` has bit 2 (right) or bit 1 (left).

**Mouse mode detection** `sub_7100318C1C` (confirmed): every frame it asks for the remembered side's mouse, then the other side's, and
counts consecutive frames where `NmouseState.attributes & 2` is set; mouse controls switch on once the count reaches a tuning value
(`*(cfg + 2560)`). So "the Joy-Con 2 is on its side on a surface" reaches the game as attribute bit 1 of the Nmouse state. Side 1 is
type bit 2 (right Joy-Con), side 2 is type bit 1.

**Cursor** `sub_710031935C` (placement / Rock Smash target) and `sub_7100EBF7E0`: the cursor is `NmouseState.x`, `.y` (+0x10, +0x14) in
the 1920x1080 range given to InitializeNmouse, turned into a world ray (`sub_710091CD80`). `JcmsState+0x10` non-zero (plus a game
condition) makes the player turn toward the cursor.

Clicks: no reader of `NmouseState.buttons` was found in main; the button configs map the Joy-Con's own buttons, which still arrive as
npad buttons (inferred: the game takes R/ZR or L/ZL from the npad).

## IPC commands (IHidServer): confirmed

From the sdk proxy thunks at 0x75006701b4.. (vtable order from slot 0x538) matched to the `nn::hid::detail` callers' vtable slots.
Raw-argument offsets are after the CMIF header.

| Cmd | Slot | Function | Arguments | Output | Pokopia |
|---|---|---|---|---|---|
| 3110 | 0x538 | InitializeJcms | aruid @0, u64 size @8, copy handle: transfer memory; sends PID | none | yes |
| 3111 | 0x540 | FinalizeJcms | aruid | none | yes |
| 3112 | 0x548 | StartJcms | u32 JcmsHandle @0, aruid @8 | none | yes |
| 3113 | 0x550 | StopJcms | u32 JcmsHandle @0, aruid @8 | none | no |
| 3130 | 0x558 | InitializeNmouse(NmouseStateRange) (sdk 0x75006576c0) | u8 pointer type (0) @0, s32 width @4, s32 height @8 (from the range table: 1280x720, 1920x1080, 2560x1440, 3840x2160), aruid @0x10, u64 size @0x18, transfer memory; the SDK then sends 3138 with 6 | none | no |
| 3131 | 0x560 | FinalizeNmouse | aruid | none | yes |
| 3132 | 0x568 | SetNmousePointerRanges / SetNmouseStateRanges | s32 width @0, s32 height @4, aruid @8 | none | no |
| 3133 | 0x570 | GetNmousePointerRanges / GetNmouseStateRanges | aruid | s32 width @0, s32 height @4 | no |
| 3135 | 0x578 | EnableShowingCursor (sdk 0x750065719c) | bool @0, aruid @8 | none | no |
| 3136 | 0x580 | SetNmouseButtonConfigJoyLeftJcms | aruid @0, 8-byte config @8 | none | yes |
| 3137 | 0x588 | SetNmouseButtonConfigJoyRightJcms | aruid @0, 8-byte config @8 | none | yes |
| 3138 | 0x590 | SetMergedNmouseSourceTypes | u32 type set @0, aruid @8 | none | yes |
| 3139 | 0x598 | InitializeNmouse (sdk 0x75006569e0) | u8 pointer type @0, s32 width @4, s32 height @8, u32 (1) @0xC, aruid @0x10, u64 size @0x18, copy handle: transfer memory; sends PID | none | yes |
| 3140 | 0x5A0 | SetNmousePointerPosition | s32 x @0, s32 y @4, aruid @8, NmouseHandle @0x10 | none | no |
| 3141 | 0x5A8 | EnableNmousePointer | bool @0, aruid @8, NmouseHandle @0x10 | none | no |
| 3142 | 0x5B0 | RestrictNmousePointer | four floats @0..0xC, aruid @0x10 | none | no |
| 3143 | 0x5B8 | unknown (no nn::hid::detail caller) | aruid @0, u64 @8 | none | no |

Ids are the `mov w, #id` of each thunk; the slots were checked against the IHidServer vtable in the sdk data (3110 at +0x538). There is
no 3114-3129 or 3134. "Pokopia" = the game imports the nn::hid wrapper that sends it.

The old runtime names (3110 = InitializeNmouse … 3139 = StartJcms) were wrong; the boot order is 3139, 3110, 3138, 3136, 3137.

## Nmouse transfer memory (the game's 0xC000 buffer): confirmed unless marked

Read directly by `nn::hid::detail::NmouseTransferMemoryAccessor` (sdk 0x7500658168..0x75006590a0); the server owns the contents.

| Offset | Size | Contents |
|---|---|---|
| 0x0000 | 17 × 0x10 | handle table: u64 NmouseHandle, u8 valid |
| 0x0220 | 17 × 0x818 | one state lifo per handle index |
| 0x8C40 | 17 × 0x1C | info: u32 NmouseType set @0, u8 valid @4, u32 NpadIdType @8, u8 valid @0xC, u8 pointer enabled @0x10 (rest unknown) |
| 0x8E68 | 1 | IsEnabledShowingCursor |

Lifo: the usual hid ring (0x20 header: unused, buffer count, index of the newest entry, count; then 17 × (u64 seqlock, state)), states
0x70 bytes. The reader (sdk 0x75006590a0) retries on an odd seqlock and checks neighbours' sampling numbers differ by 1.

NmouseType bits: 0 merged (GetMergedNmouseHandle returns the first valid entry with bit 0), 1 Joy-Con (L) mouse, 2 Joy-Con (R) mouse
(GetNmouseHandles(npad) returns the entries of that npad whose type set is exactly 2, then exactly 4).

Internal state (0x70) and the public `NmouseState` (0x30) copied from it (GetNmouseStates 0x7500658980); the field meanings come from
the SDK's own conversions: DevNmouseState (0x7500657dc0), GetNmouseStatesDev (0x7500658c80) and GetWheelDeltaDev (0x7500658da0).

| Internal | Public | Field |
|---|---|---|
| 0x00 u64 | 0x00 | unknown, never read by the game or the SDK conversions (left 0) |
| 0x08 u64 | 0x08 | sampling number |
| 0x10 s32 ×2 | 0x10 | x, y in the pointer range: **confirmed by use** |
| 0x18 s32 ×2 | 0x18 | pointer range width, height: **confirmed** (DevNmouseState maps 1280x720 / 1920x1080 / 2560x1440 / 3840x2160 to 0..3). There is no movement delta in the state. |
| 0x20 s32 | 0x20 | wheel delta (first of GetWheelDeltaDev): the game sums public +0x20 and +0x24 of every new state into two counters clamped to ±600 (main 0x7101126240) |
| 0x24 s32 |: | dev only |
| 0x28 u32 | 0x28 | buttons: *inferred* (MouseButton bits; copied to DevNmouseState, no reader interprets them) |
| 0x2C u32 | 0x2C | attributes; bit 1 = on its surface: **confirmed by use**; DevNmouseState keeps only bit 1, as a bool |
| 0x30 s32 | 0x24 | wheel delta (second of GetWheelDeltaDev) |
| 0x34.. |: | dev data |

Lifo header (both blocks, read by sdk 0x75006590a0 / GetJcmsStates): +0x10 index of the newest entry, +0x18 count, entries from +0x20;
+0x08 (buffer count) is not read by the client.

Wheel units: *inferred* 120 per notch (the game clamps its sum at ±600).

## Jcms transfer memory (the game's 0x4000 buffer): confirmed

`nn::hid::detail::InitializeJcms` stores the buffer pointer (sdk global 0x7500e081b8); `GetJcmsStates` (sdk 0x7500656520) reads lifo
`index * 0x2C8`, index = npad × 2 + type (JcmsHandle = npad | type << 8, npad < 8, type 0 or 1). Lifo of 17 × (u64 seqlock,
0x20-byte JcmsState); JcmsState: +0x08 sampling number, +0x10 u64 read by the placement code, the rest unknown.

## Fortune's Weave (main of update 131072)

Read in main (`work`-side scripts, not shipped); `runtime/games/ObeliskChamber/ObeliskChamber_mouse.cpp` reads the same fields for the log and the F1 menu.

- Setup (0x6509e8): `InitializeNmouse(buf, 0xC000, 1920, 1080)` and the two button configs (0x1c5cd10, 0x1c5cd18). No Jcms.
- Every frame (0x19c6ea0, from the input update 0x650280; the input object is `[GOT 0x216a4b0]`, the mouse manager at +0x2c8): for each
  handle of npad 0 (`GetNmouseHandles(…, 2, 0)`) it reads the type set (bit 2 right, bit 1 left: records +0x00 / +0x60), then
  `GetNmouseState` into the record and **`SetNmousePointerPosition(handle, 960, 540)`**: the pointer goes back to the centre every frame and
  the frame's movement is `x - 960`, `y - 540` (record +0x3c / +0x40). Buttons: +0x30 held (from the state's +0x28), +0x34 pressed,
  +0x38 released; the battle cursor reads the left button held (input +0x2f8 bit 0, 0x73511c). Wheel sums at +0x44 / +0x48.
- Mouse mode: while attribute bit 1 is set the record's +0x50 counts frames up to +0xc8 (30), but only while the Joy-Con's accelerometer
  says it lies flat (controller +0x18 <= +0xd0, -0.8; a PC pad never does), or jumps there when one frame's movement squared is over
  +0xcc (12000, ~110 units); then +0x5e = 1 (mouse mode) and the right mouse's movement moves the game's pointer (+0xdc / +0xe0, kept
  in 1920 x 1080). Once the bit clears, +0x4c counts frames and mode ends after +0xd8 (120), or after +0xd4 (20) when the Joy-Con's
  orientation changed. The values are logged at start (`game mouse controls: ...`). "Mouse mode?" (0x19c7210, ~90 callers, and inlined copies): seen (+0x5f) and (SL + SR mode +0x5c, or enabled +0xe4
  and allowed +0x5d and +0x5e).
- Settings (the game's own Settings menu; read by 0x1584e.. / 0x1585020 into the mouse system `[0x22983a8]` and the flags above):
  System: "Method for Switching to Mouse" (automatic / manual: SL + SR together), "Menu Cursor Speed (Mouse)"; battle maps: "Map Cursor
  Speed (Mouse)", "Camera Sensitivity (Mouse)"; bases and dungeons: "Camera Sensitivity (Mouse)". Calibration ("Calibrate Angle", the
  Joy-Con held at an angle) only rotates a real Joy-Con's motion; a PC mouse needs none (keep it at the default; "Reset Calibration").
- The mouse system `[0x22983a8]` turns the record's movement into +0x208 / +0x20c (rotated by the calibration angle +0x234, degrees,
  0 by default) and keeps a stack of "situations" (+0xa0 deque; 13 in the Dagsion plaza) that the consumers check.
- In mouse mode (System > Mouse Controls lists it per scene): bases and dungeons turn the camera with the mouse (Camera Control; the
  wheel zooms while standing still; left click Access / Attack, right click Cancel / Menu; the left stick still walks), battle maps
  move the map cursor with it (`game::map::Cursor::Impl::Tick` 0x735020 reads the mouse system; "the faster you move it, the farther the
  cursor moves"), menus move their selection by the mouse's motion (the base menu: about one entry per 500 units up or down at the
  default Menu Cursor Speed, wrapping; no pointer is drawn there; `gameui::MouseCursor` draws one where a screen uses +0xdc / +0xe0).
  Measured in the plaza
  at 30 fps: 3 and 6 units per frame did not turn the camera at all, 12 and 24 turned it (about 0.07 degrees per unit above that), so
  a slow hand on a 1:1 PC mouse falls under the camera's threshold. Clicks: left = R, right = ZR per the game's guide (0_ISM_Battle.msbt),
  sent as npad buttons; the battle cursor also reads the Nmouse left button.

## What the runtime does

- `src/services/hid.cpp`: the commands above; InitializeNmouse/InitializeJcms keep the guest buffer (a guest address is a host pointer),
  zero it, set up the rings and, unless `[input] jc2_mouse = off`, list two devices: a merged mouse (slot 0, type 1) and a Joy-Con (R)
  mouse of player 1 (slot 1, type 4, npad 0). The sampler (every 5 ms) pushes the same state into both rings and empty Jcms states
  for player 1.
- The device is a mouse that moves, as a real Joy-Con 2: a virtual pointer in the pointer range, moved by the PC mouse's motion
  (`jc2_mouse_feed`: counts since the last feed times `[mouse] jc2_speed`) and placed by `SetNmousePointerPosition` (3140). Where the
  Windows pointer is on the window does not matter. (Before 0.1.6 x, y were the pointer's position on the window: Fortune's Weave,
  which re-centres every frame, read the distance from the window's centre as a movement every frame and its cursor ran off.)
- Attribute bit 1 ("on its surface") is who has control. `auto`: set when the mouse moves `[mouse] jc2_wake` counts within 0.25 s over
  the picture, clicks or scrolls; cleared at once by any key or pad input (`jc2_mouse_other_input`, from play.cpp's key and pad
  handling), so the last device used wins and a still or jittery mouse never switches the game. `on`: always set while the window has
  focus. Motion while lifted only counts toward waking it.
- Fortune's Weave (`games/ObeliskChamber/ObeliskChamber_mouse.cpp`, `[fefw] mouse_switch_now`): with the game's switching method on automatic, the game's
  mode follows the attribute at once (on: +0x50 = +0xc8; off: +0x5e = 0) instead of its 30-frame flat-on-the-table wait (which a PC
  never satisfies) and 120-frame tail. When mode starts, the game's pointer is put where the Windows pointer was (picture u, v to
  1920 x 1080 UI units through the centred 16:9 region when ultrawide is active); while it runs, its position is handed back so play
  mode puts the Windows pointer there when it frees the mouse.
- While it is on its surface play mode captures the mouse (`[mouse] jc2_capture`): relative raw motion, no Windows pointer (the game
  draws its own), no window edge. Released when it is lifted, on focus loss and while the F1 menu is open. Left / right click are npad
  R / ZR while it is on its surface (held 50 ms at least), as the Joy-Con 2 (R)'s own buttons; the click that puts it down is not a press.
- Logs: InitializeNmouse/InitializeJcms with the block address, the device listing, the first three StartJcms and SetNmousePointerPosition
  calls, every on-surface / lifted change (first 200), the first three samples with movement, buttons or wheel, and (fefw) `game mouse
  controls ON/off` with the counts and the game's own thresholds once.
- cmd 214 is `SendVibrationValues` in its Switch 2 form (the SDK uses it unless the Switch 1 compatible mode is on; 206 otherwise):
  same two buffers plus a u16 (2). It is stored and forwarded to the pad rumble like 206.

## Handing the mouse over (camera owner)

- Before turning the camera with mouse counts each frame: if `nxrt::hid::jc2_mouse_engaged()` is true (the device is on its surface),
  drop the counts.
- In auto mode, to give the mouse to the game (placement or Rock Smash, once detected, or a key): `jc2_mouse_request(true)`, and
  `jc2_mouse_request(false)` to take it back. Play mode stops capturing while `jc2_mouse_wants_pointer()` is true (mode on, or auto with
  a request), so the pointer becomes absolute and the Joy-Con 2 mouse goes on its surface.
- Placement / Rock Smash detection is not done: auto mode without a request means the Joy-Con 2 mouse is live only where the camera
  does not hold the mouse.

## Menus outside play (pkp_menu.cpp pick_extend)

The title, settings, language, save and yes / no screens go through the same pane scan as the in-game menus (play mode calls
`menu_frame` with the menu context whenever the game is not in play). main holds no pane-name strings (the names live in the layout
data and the screens are scripted), so no per-menu selection field was found; the selection is read off the cursor panes. Two extra rules:
a part's entry inside a list entry is promoted to the list entry (settings: `Win_cursor` in each `CL_0020_parts_setting_menu_btn_N`),
and sibling instances of one button layout with the triangle cursor in one of them form the entry list (yes / no, language, save choices).
A click on a key guide listed in `[pkp] menu_key_guides` (default `881468973=btn_minus`, the title's "(-) Settings") sends its button.
`menu_log 1` prints `menu: selection by <rule>: ...` whenever the rule or layout changes.
