# Obelisk Chamber

The launcher of Obelisk Chamber (the project's name, after the place in the game where a route is chosen). File and package
names keep `fefw` (`ObeliskChamber-launcher.exe`, `ObeliskChamber-<version>-win64.zip`): the updater and the release scripts look for them.

Tauri 2 launcher for this repository's `ObeliskChamber.exe`. It ships no game files and never writes to your dump. It works on a checkout of
the repository: it finds the checkout from its own location (or Settings, Repository folder), checks your dump, runs the repository's
own setup steps on your PC (see docs/BUILDING.md), writes `ObeliskChamber.ini` in a run folder (`work/play` by default) and starts the game.

- Build: `npm install`, then `npm run tauri build -- --no-bundle`; the exe is `src-tauri/target/release/ObeliskChamber-launcher.exe`.
- Development: `npm run tauri dev`; `cargo test` in `src-tauri` runs the unit tests.
- Setup state is read from what exists on disk (the copied ExeFS, the lifted libraries, `ObeliskChamber.exe`, the shader cache and its
  `work/shaders/setup.json`), so a paused setup continues where it stopped. The full log is `work/setup.log`.
- The look follows the game's own menus (a title list, framed panels with tabs, a help box, key help), redrawn from scratch in
  `src/theme.css`, `src/layout.css` and `src/components.css`; `src/menu.js` adds the cursor and keyboard / controller navigation
  (arrows or the D-pad to move, left / right to change a value, Enter / A to confirm, Esc / B to go back, Q and E or L and R for tabs).
- Settings, Launcher, "Use the game's assets for the launcher" (`game_art` in launcher.json, read when the launcher starts): the
  title light, the windows and the buttons are then drawn with the game's own menu art. `read_game_layout` (main.rs) hands the page a
  layout archive from the dump's `romfs/Layout` (`Title.arc`, `CommonMenu.arc`, `Common.arc`, `Textures/Common.arc`), `src/gameart.js`
  reads the textures (SARC, BNTX, BC1 / BC3) and sets them as CSS variables: the title light, windows, buttons, the Settings rows
  and value boxes, the arrows and the cursor. Nothing is copied or cached, and any part that cannot be read keeps the drawn look.
- The icon is drawn by `icon/make_icon.py` (no outside artwork, needs Pillow); it writes every size the build uses and the game's
  `fefw.ico`.
