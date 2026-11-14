// SPDX-License-Identifier: GPL-3.0-or-later
//! Reads and edits the runtime's ini files in the run folder: ObeliskChamber.ini (every key: runtime/docs/CONFIG.md) and controls.ini (the
//! play-mode file the game writes on its first start). The key schema lives in the frontend; this side moves `section.key` values in
//! and out with the careful line editor in ini.rs, so comments and unknown keys survive.

use crate::ini::Ini;
use crate::setup::Paths;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

pub const GAME_INI: &str = "ObeliskChamber.ini";
/// The game's settings file before the project was renamed.
const OLD_GAME_INI: &str = "fefw.ini";

/// The game's settings file in a run folder. A folder that has only the old file (fefw.ini) gets a copy under the new name, its log
/// path renamed with it, so the player's settings carry over; the old file is left where it is. The game does the same at start.
pub fn game_ini(dir: &Path) -> PathBuf {
    let new = dir.join(GAME_INI);
    if !new.exists() {
        if let Ok(text) = fs::read_to_string(dir.join(OLD_GAME_INI)) {
            let _ = fs::write(&new, text.replace("fefw.log", "ObeliskChamber.log"));
        }
    }
    new
}
pub const CONTROLS_INI: &str = "controls.ini";

#[derive(Serialize)]
pub struct SettingsDump {
    pub game_dir: String,
    pub game_exists: bool,
    pub controls_exists: bool,
    pub game: HashMap<String, String>,
    pub controls: HashMap<String, String>,
}

#[derive(Deserialize)]
pub struct Edit {
    /// "game" (ObeliskChamber.ini) or "controls" (controls.ini)
    pub file: String,
    pub section: String,
    pub key: String,
    pub value: String,
}

fn load(path: &Path) -> Option<Ini> {
    let bytes = fs::read(path).ok()?;
    Some(Ini::parse(&String::from_utf8_lossy(&bytes)))
}

fn map_of(ini: &Option<Ini>) -> HashMap<String, String> {
    ini.as_ref().map(|i| i.all().into_iter().collect()).unwrap_or_default()
}

fn file_for(dir: &Path, which: &str) -> Result<PathBuf, String> {
    match which {
        "game" => Ok(game_ini(dir)),
        "controls" => Ok(dir.join(CONTROLS_INI)),
        other => Err(format!("unknown settings file '{other}'")),
    }
}

pub fn read(dir: &Path) -> SettingsDump {
    let game = load(&game_ini(dir));
    let controls = load(&dir.join(CONTROLS_INI));
    SettingsDump {
        game_dir: crate::dump::fwd(dir),
        game_exists: game.is_some(),
        controls_exists: controls.is_some(),
        game: map_of(&game),
        controls: map_of(&controls),
    }
}

pub fn write_atomic(path: &Path, text: &str) -> Result<(), String> {
    if let Some(d) = path.parent() {
        fs::create_dir_all(d).map_err(|e| format!("cannot create {}: {}", d.display(), e))?;
    }
    let tmp = PathBuf::from(format!("{}.tmp", path.display()));
    fs::write(&tmp, text).map_err(|e| format!("cannot write {}: {}", tmp.display(), e))?;
    fs::rename(&tmp, path).map_err(|e| {
        let _ = fs::remove_file(&tmp);
        format!("cannot replace {}: {}", path.display(), e)
    })
}

/// Applies edits grouped per file. controls.ini is never created here: the game writes it with its factory bindings on first run.
pub fn apply(dir: &Path, edits: &[Edit]) -> Result<(), String> {
    for which in ["game", "controls"] {
        let mine: Vec<&Edit> = edits.iter().filter(|e| e.file == which).collect();
        if mine.is_empty() {
            continue;
        }
        let path = file_for(dir, which)?;
        let mut ini = match load(&path) {
            Some(i) => i,
            None if which == "game" => Ini::empty(),
            None => return Err(format!("{CONTROLS_INI} does not exist yet. Start the game once so it creates it.")),
        };
        for e in mine {
            ini.set(&e.section, &e.key, &e.value);
        }
        write_atomic(&path, &ini.to_text())?;
    }
    Ok(())
}

/// The ini values setup owns: where the game code, the romfs, the dumped add-on content, the mods, the shader cache and the texture
/// cache are. With no dlc/ in the dump, [aoc] dir is empty: no add-on content.
fn owned(p: &Paths) -> Vec<(&'static str, &'static str, String)> {
    vec![
        ("fs", "exefs", crate::dump::game_path(&p.plain_exefs())),
        ("fs", "main", String::new()),
        ("fs", "romfs", crate::dump::game_path(&p.romfs)),
        ("fs", "mods", crate::dump::game_path(&p.mods())),
        ("aoc", "dir", p.dlc.as_deref().map(crate::dump::game_path).unwrap_or_default()),
        ("nvn", "shader_cache", crate::dump::game_path(&p.spirv())),
        ("nvn", "bc7_cache", crate::dump::game_path(&p.bc7())),
    ]
}

pub fn play_paths_ok(dir: &Path, p: &Paths) -> bool {
    let Some(ini) = load(&game_ini(dir)) else { return false };
    owned(p).iter().all(|(s, k, v)| ini.get(s, k).unwrap_or_default().replace('\\', "/") == *v)
}

/// The shipped language tables (`locales\*.ini`), from the package next to the launcher or from the checkout.
fn locale_sources(p: &Paths) -> Vec<PathBuf> {
    let mut out = Vec::new();
    if let Some(tools) = &p.package {
        if let Some(root) = tools.parent() {
            out.push(root.join("locales"));
        }
    }
    out.push(p.repo.join("locales"));
    out
}

/// Copies the language tables the package ships into the run folder, where fefw_rt.dll reads them (`locales\<lang>.ini` next to
/// the DLL). A table already there is left alone: it may be the player's own translation of that language. Only `.ini` files are
/// copied, and only the ones the run folder does not have yet.
pub fn install_locales(dir: &Path, p: &Paths) {
    let target = dir.join("locales");
    for src in locale_sources(p) {
        let Ok(rd) = fs::read_dir(&src) else { continue };
        for e in rd.flatten() {
            let from = e.path();
            let is_ini = from.extension().and_then(|x| x.to_str()).is_some_and(|x| x.eq_ignore_ascii_case("ini"));
            if !is_ini || !from.is_file() {
                continue;
            }
            let Some(name) = from.file_name() else { continue };
            let to = target.join(name);
            if to.exists() {
                continue; // the player's copy, or one this already put there
            }
            let _ = fs::create_dir_all(&target);
            if let Err(e) = fs::copy(&from, &to) {
                eprintln!("[locales] could not copy {} to {}: {e}", from.display(), to.display());
            }
        }
    }
}

/// Writes ObeliskChamber.ini's paths (kept: every other key, and a save folder the player chose). Used by setup and before every Play.
pub fn ensure_play_paths(dir: &Path, p: &Paths) -> Result<(), String> {
    let path = game_ini(dir);
    let new = !path.exists();
    let mut ini = load(&path).unwrap_or_else(|| {
        Ini::parse("; ObeliskChamber.ini: settings of ObeliskChamber.exe. Every key is described in runtime/docs/CONFIG.md. The launcher writes [fs], [aoc] and [nvn] paths.\n")
    });
    let mut changed = new;
    let _ = fs::create_dir_all(p.mods());
    install_locales(dir, p);
    for (s, k, v) in owned(p) {
        if ini.get(s, k).unwrap_or_default().replace('\\', "/") != v {
            ini.set(s, k, &v);
            changed = true;
        }
    }
    if ini.get("fs", "save_dir").unwrap_or_default().trim().is_empty() {
        let _ = fs::create_dir_all(dir.join("save"));
        ini.set("fs", "save_dir", "save"); // relative to the run folder
        changed = true;
    }
    if new {
        // a first ini: the log next to it, the built-in temporal upscaler, the game's own frame rate
        ini.set("log", "path", "ObeliskChamber.log");
        ini.set("log", "level", "info");
        ini.set("boot", "gfx", "window");
        ini.set("dlss", "upscaler", "taau");
        ini.set("fefw", "fps60", "0");
        ini.set("fefw", "output", "console");
    }
    if changed {
        write_atomic(&path, &ini.to_text())?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("fefw-loc-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    fn paths_with(repo: &Path, game: &Path) -> Paths {
        Paths {
            repo: repo.to_path_buf(),
            exefs: repo.join("exefs"),
            romfs: repo.join("romfs"),
            dlc: None,
            game_dir: game.to_path_buf(),
            package: None,
        }
    }

    #[test]
    fn install_locales_copies_the_shipped_tables_only_once() {
        let repo = tmp("repo");
        let game = tmp("game");
        fs::create_dir_all(repo.join("locales")).unwrap();
        fs::write(repo.join("locales").join("zh.ini"), "shipped zh").unwrap();
        fs::write(repo.join("locales").join("README.md"), "docs").unwrap();
        let p = paths_with(&repo, &game);

        install_locales(&game, &p);
        assert_eq!(fs::read_to_string(game.join("locales").join("zh.ini")).unwrap(), "shipped zh");
        assert!(!game.join("locales").join("README.md").exists(), "only the tables are copied");

        // the player's own copy is never replaced by a later run
        fs::write(game.join("locales").join("zh.ini"), "the player's own").unwrap();
        install_locales(&game, &p);
        assert_eq!(fs::read_to_string(game.join("locales").join("zh.ini")).unwrap(), "the player's own");
    }
}
