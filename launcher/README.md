# fefw recomp launcher

Tauri 2 launcher for this repository's `fefw.exe`. It ships no game files and never writes to your dump. It works on a checkout of
the repository: it finds the checkout from its own location (or Settings, Repository folder), checks your dump, runs the repository's
own setup steps on your PC (see docs/BUILDING.md), writes `fefw.ini` in a run folder (`work/play` by default) and starts the game.

- Build: `npm install`, then `npm run tauri build -- --no-bundle`; the exe is `src-tauri/target/release/fefw-launcher.exe`.
- Development: `npm run tauri dev`; `cargo test` in `src-tauri` runs the unit tests.
- Setup state is read from what exists on disk (the copied ExeFS, the lifted libraries, `fefw.exe`, the shader cache and its
  `work/shaders/setup.json`), so a paused setup continues where it stopped. The full log is `work/setup.log`.
- The look follows the game's own menus (a title list, framed panels with tabs, a help box, key help), redrawn from scratch in
  `src/theme.css`, `src/layout.css` and `src/components.css`; `src/menu.js` adds the cursor and keyboard / controller navigation
  (arrows or the D-pad to move, left / right to change a value, Enter / A to confirm, Esc / B to go back, Q and E or L and R for tabs).
- The icon is drawn by `icon/make_icon.py` (no outside artwork); `npx tauri icon icon/icon.png -o src-tauri/icons` makes the sizes.
