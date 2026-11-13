// SPDX-License-Identifier: GPL-3.0-or-later
//! Starts fefw.exe in the run folder, watches it, and reports how it ended. On Linux fefw.exe (a Windows program) runs through a
//! runner: the launcher's `runner` setting, else Steam's own Proton, else umu-run, else wine.

use serde::Serialize;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Mutex;
use std::time::{Duration, SystemTime};
use tauri::{AppHandle, Emitter, Manager};

/// Files the launcher may open for the player, by name, inside the run folder (the runtime writes the reports next to the log).
pub const REPORT_FILES: [&str; 4] = ["fefw_crash.txt", "fefw_freeze.txt", "fefw_fatal.txt", "fefw.log"];

#[derive(Serialize, Clone, Default)]
pub struct ReportFile {
    pub name: String,
    /// written since this run started
    pub fresh: bool,
}

#[derive(Serialize, Clone)]
pub struct Status {
    /// "idle", "running" or "exited"
    pub state: String,
    pub code: Option<i32>,
    pub crashed: bool,
    pub files: Vec<ReportFile>,
    pub message: String,
}

impl Default for Status {
    fn default() -> Self {
        Status { state: "idle".into(), code: None, crashed: false, files: vec![], message: String::new() }
    }
}

#[derive(Default)]
pub struct PlayState(pub Mutex<Status>);

fn set_status(app: &AppHandle, s: Status) {
    *app.state::<PlayState>().0.lock().unwrap() = s.clone();
    let _ = app.emit("game-status", s);
}

fn report_files(dir: &Path, started: SystemTime) -> Vec<ReportFile> {
    REPORT_FILES
        .iter()
        .filter_map(|n| {
            let md = std::fs::metadata(dir.join(n)).ok()?;
            let fresh = md.modified().map(|m| m + Duration::from_secs(2) >= started).unwrap_or(false);
            Some(ReportFile { name: n.to_string(), fresh })
        })
        .collect()
}

/// The command that starts fefw.exe: the program and its arguments before the exe. On Windows nothing (the exe itself). On Linux
/// `custom` (the runner setting, split into words like a shell does, so `"/path/Proton - Experimental/proton" run` works), else
/// Steam's Proton (in the Steam Linux Runtime, as Steam starts a game), else umu-run, else wine.
pub fn runner(custom: &str) -> Result<Vec<String>, String> {
    if cfg!(windows) {
        return Ok(vec![]);
    }
    let words = words(custom);
    if !words.is_empty() {
        return Ok(words);
    }
    if let Some(w) = steam_proton() {
        return Ok(w);
    }
    for r in ["umu-run", "wine"] {
        if on_path(r) {
            return Ok(vec![r.to_string()]);
        }
    }
    Err("fefw.exe is a Windows program: on Linux it runs through Proton or Wine. Install Steam and its Proton (Library, Tools, Proton Experimental), umu-launcher (umu-run) or \
         Wine, or set the runner in Settings."
        .into())
}

/// The values of `"key" "value"` lines in a Valve KeyValues file (libraryfolders.vdf, toolmanifest.vdf, appmanifest_*.acf).
fn vdf_values(text: &str, key: &str) -> Vec<String> {
    let quoted = format!("\"{key}\"");
    text.lines()
        .filter_map(|l| l.trim().strip_prefix(quoted.as_str()))
        .filter_map(|rest| rest.trim().strip_prefix('"').and_then(|v| v.strip_suffix('"')))
        .map(|v| v.replace("\\\\", "\\"))
        .collect()
}

/// (Steam root, its library folders) from libraryfolders.vdf. None without Steam.
fn steam_libraries() -> Option<(PathBuf, Vec<PathBuf>)> {
    let home = PathBuf::from(std::env::var_os("HOME")?);
    for r in [".local/share/Steam", ".steam/steam", ".steam/root", ".var/app/com.valvesoftware.Steam/.local/share/Steam"] {
        let Ok(root) = std::fs::canonicalize(home.join(r)) else { continue };
        let Ok(text) = std::fs::read_to_string(root.join("steamapps/libraryfolders.vdf")) else { continue };
        let mut libs: Vec<PathBuf> = Vec::new();
        for lib in std::iter::once(root.clone()).chain(vdf_values(&text, "path").into_iter().map(PathBuf::from)) {
            let Ok(lib) = std::fs::canonicalize(lib) else { continue };
            if lib.join("steamapps").is_dir() && !libs.contains(&lib) {
                libs.push(lib);
            }
        }
        return Some((root, libs));
    }
    None
}

/// Proton Experimental, else the newest "Proton N.N" Steam installed. `PROTON_DIR` (a folder with `proton`) overrides it.
fn pick_proton(libs: &[PathBuf]) -> Option<PathBuf> {
    if let Some(p) = std::env::var_os("PROTON_DIR").map(PathBuf::from) {
        return p.join("proton").is_file().then_some(p);
    }
    let rank = |name: &str| -> (u8, Vec<u32>) {
        if name.contains("Experimental") {
            return (2, vec![]);
        }
        let nums: Vec<u32> = name.split(|c: char| !c.is_ascii_digit()).filter_map(|x| x.parse().ok()).collect();
        (u8::from(!nums.is_empty()), nums)
    };
    let mut found: Vec<(String, PathBuf)> = Vec::new();
    for lib in libs {
        let Ok(rd) = std::fs::read_dir(lib.join("steamapps/common")) else { continue };
        for e in rd.flatten() {
            let name = e.file_name().to_string_lossy().to_string();
            if name.starts_with("Proton") && e.path().join("proton").is_file() {
                found.push((name, e.path()));
            }
        }
    }
    found.into_iter().max_by_key(|(n, _)| rank(n)).map(|(_, p)| p)
}

/// Steam's Proton as runner words: the Steam Linux Runtime's entry point around it when its toolmanifest.vdf asks for one (as Steam
/// starts a game), and STEAM_COMPAT_* set. The prefix is ~/.local/share/fefw-recomp/proton.
fn steam_proton() -> Option<Vec<String>> {
    let (root, libs) = steam_libraries()?;
    let pdir = pick_proton(&libs)?;
    let s = |p: &Path| p.to_string_lossy().into_owned();
    let data = std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share")))?
        .join("fefw-recomp/proton");
    std::fs::create_dir_all(&data).ok()?;
    let mut w = vec![format!("STEAM_COMPAT_DATA_PATH={}", s(&data)), format!("STEAM_COMPAT_CLIENT_INSTALL_PATH={}", s(&root))];
    let manifest = std::fs::read_to_string(pdir.join("toolmanifest.vdf")).unwrap_or_default();
    if let Some(appid) = vdf_values(&manifest, "require_tool_appid").first() {
        for lib in &libs {
            let acf = std::fs::read_to_string(lib.join(format!("steamapps/appmanifest_{appid}.acf"))).unwrap_or_default();
            let Some(dir) = vdf_values(&acf, "installdir").into_iter().next() else { continue };
            let entry = lib.join("steamapps/common").join(&dir).join("_v2-entry-point");
            if entry.is_file() {
                w.extend([s(&entry), "--verb=waitforexitandrun".into(), "--".into()]);
                break;
            }
        }
    }
    w.extend([s(&pdir.join("proton")), "waitforexitandrun".into()]);
    Some(w)
}

/// Shell-like words: spaces separate them, "..." and '...' group them, a backslash outside quotes takes the next character as is
/// (inside "..." only before " \\ $ `, as in a shell).
fn words(s: &str) -> Vec<String> {
    let (mut out, mut cur, mut any) = (Vec::new(), String::new(), false);
    let mut quote: Option<char> = None;
    let mut it = s.chars();
    while let Some(c) = it.next() {
        match (quote, c) {
            (Some(q), c) if c == q => quote = None,
            (Some('\''), c) => cur.push(c),
            (q, '\\') => {
                match it.next() {
                    Some(n) if q.is_none() || matches!(n, '"' | '\\' | '$' | '`') => cur.push(n),
                    Some(n) => {
                        cur.push('\\');
                        cur.push(n);
                    }
                    None => cur.push('\\'),
                }
                any = true;
            }
            (Some(_), c) => cur.push(c),
            (None, '"' | '\'') => {
                quote = Some(c);
                any = true;
            }
            (None, c) if c.is_whitespace() => {
                if any || !cur.is_empty() {
                    out.push(std::mem::take(&mut cur));
                }
                any = false;
            }
            (None, c) => cur.push(c),
        }
    }
    if any || !cur.is_empty() {
        out.push(cur);
    }
    out
}

/// Whether the runner is umu-run, also behind `env [-u NAME] VAR=...`.
fn is_umu(runner: &[String]) -> bool {
    let mut it = runner.iter();
    let mut in_env = false;
    while let Some(w) = it.next() {
        if !in_env && w == "env" {
            in_env = true;
        } else if in_env && (w == "-u" || w == "--unset") {
            it.next(); // the name it unsets
        } else if !(in_env && w.starts_with('-')) && !w.contains('=') {
            return Path::new(w).file_name().is_some_and(|n| n == "umu-run");
        }
    }
    false
}

fn on_path(name: &str) -> bool {
    std::env::var_os("PATH").is_some_and(|p| std::env::split_paths(&p).any(|d| d.join(name).is_file()))
}

/// `launcher_dir`: the folder the package's upscalers/ is in (the AppImage's folder, not its mount).
pub fn start(app: &AppHandle, exe: &Path, game_dir: &Path, launcher_dir: &Path, minimize: bool, runner_setting: &str) -> Result<(), String> {
    if app.state::<PlayState>().0.lock().unwrap().state == "running" {
        return Err("The game is already running.".into());
    }
    if !exe.is_file() {
        return Err(format!("fefw.exe was not found at {}. Run Set up first.", crate::dump::fwd(exe)));
    }
    let runner = runner(runner_setting)?;
    let app2 = app.clone();
    let dir = game_dir.to_path_buf();
    let exe = exe.to_path_buf();
    let launcher_dir = launcher_dir.to_path_buf();
    set_status(app, Status { state: "running".into(), message: "Starting fefw.exe...".into(), ..Default::default() });
    std::thread::spawn(move || {
        // leading NAME=value words are environment variables, as in a shell (there is none in between here)
        let n = runner.iter().take_while(|w| w.split_once('=').is_some_and(|(k, _)| !k.is_empty() && !k.contains('/'))).count();
        let (assign, words) = runner.split_at(n);
        let mut cmd = match words.split_first() {
            None => Command::new(&exe),
            Some((prog, args)) => {
                let mut c = Command::new(prog);
                c.args(args).arg(&exe);
                c
            }
        };
        crate::setup::host_env(&mut cmd);
        cmd.envs(assign.iter().filter_map(|w| w.split_once('=')));
        // Steam's Proton (and umu-run) run in a container that does not see a dump outside the home folder, /mnt and /media (#142)
        if !cfg!(windows) && std::env::var_os("STEAM_COMPAT_MOUNTS").is_none() && !assign.iter().any(|w| w.starts_with("STEAM_COMPAT_MOUNTS=")) {
            let home = std::env::var_os("HOME").map(PathBuf::from);
            if let Some(m) = container_mounts(&ini_dirs(&dir), home.as_deref()) {
                cmd.env("STEAM_COMPAT_MOUNTS", m);
            }
        }
        // umu-run wants a game id (it picks protonfixes for known games from it); "0" is the generic one its README uses
        if is_umu(&runner) && std::env::var_os("GAMEID").is_none() && !assign.iter().any(|w| w.starts_with("GAMEID=")) {
            cmd.env("GAMEID", "0");
        }
        // fefw.exe reads fefw.ini from its working directory: every path comes from there
        cmd.current_dir(&dir);
        // the upscaler DLLs ship next to the launcher (upscalers/fsr, upscalers/streamline), which need not be the run folder
        // (on Linux as fefw.exe sees it under Wine: Z:/...)
        if cfg!(windows) {
            cmd.env("NXRT_LAUNCHER_DIR", &launcher_dir);
        } else {
            cmd.env("NXRT_LAUNCHER_DIR", crate::dump::game_path(&launcher_dir));
        }
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: no stray console
        }
        let started = SystemTime::now();
        let mut child = match cmd.spawn() {
            Ok(c) => c,
            Err(e) => {
                let what = if runner.is_empty() { String::new() } else { format!(" ({})", runner.join(" ")) };
                set_status(&app2, Status { state: "exited".into(), crashed: true, message: format!("Could not start fefw.exe: {e}{what}"), ..Default::default() });
                return;
            }
        };
        set_status(&app2, Status { state: "running".into(), message: format!("fefw.exe started (process {})", child.id()), ..Default::default() });
        if minimize {
            if let Some(w) = app2.get_webview_window("main") {
                let _ = w.minimize();
            }
        }
        let (code, wait_err) = match child.wait() {
            Ok(s) => (s.code(), None),
            Err(e) => (None, Some(e.to_string())),
        };
        let files = report_files(&dir, started);
        let crash_file = files.iter().any(|f| f.fresh && f.name != "fefw.log");
        let crashed = crash_file || code.map_or(true, |c| c != 0);
        // Code 4 is both the CPU check's exit and the crash handler's: only a fresh fefw_fatal.txt means the processor (#82).
        // fefw_fatal.txt is also written for other problems found before the game starts (the game's files cannot be read, #142):
        // those show its explanation, after its first line
        let fatal_text = files
            .iter()
            .any(|f| f.fresh && f.name == "fefw_fatal.txt")
            .then(|| std::fs::read_to_string(dir.join("fefw_fatal.txt")).unwrap_or_default());
        let cpu_fatal = fatal_text.as_deref().is_some_and(|t| t.is_empty() || t.starts_with("fefw cannot start on this processor"));
        let mut other_fatal = fatal_text.filter(|_| !cpu_fatal).map(|t| fatal_message(&t)).filter(|m| !m.is_empty());
        // a crash report that starts with the cause the game found beforehand (an incomplete dump, #64)
        if other_fatal.is_none() && files.iter().any(|f| f.fresh && f.name == "fefw_crash.txt") {
            other_fatal = likely_cause(&std::fs::read_to_string(dir.join("fefw_crash.txt")).unwrap_or_default());
        }
        let message = match (&wait_err, code) {
            (Some(e), _) => format!("Lost track of fefw.exe: {e}"),
            (None, Some(0)) => "fefw.exe exited normally.".to_string(),
            (None, Some(_)) if other_fatal.is_some() => other_fatal.unwrap_or_default(),
            (None, Some(4)) if cpu_fatal => match crate::cpu::build_variant() {
                // fefw.exe was built for another processor (the folder moved, the processor changed): Set up builds the one for this
                Some(_) => "fefw.exe stopped before the game started: it was built for a processor with features this one lacks (fefw_fatal.txt says which). Run Set up again: it builds the version of the game for this processor.".to_string(),
                None => "fefw.exe stopped before the game started: this processor lacks CMPXCHG16B, SSE4.2 or POPCNT, which the game needs (an Intel Core i3 / i5 / i7 of 2008 or an AMD FX of 2011, or newer). fefw_fatal.txt says which.".to_string(),
            },
            (None, Some(c)) => format!("fefw.exe exited with code {} (0x{:08X}).", c, c as u32),
            (None, None) => "fefw.exe was ended by the system.".to_string(),
        };
        set_status(&app2, Status { state: "exited".into(), code, crashed, files, message });
        if minimize {
            if let Some(w) = app2.get_webview_window("main") {
                let _ = w.unminimize();
                let _ = w.set_focus();
            }
        }
    });
    Ok(())
}

/// STEAM_COMPAT_MOUNTS for the folders the game reads (`dirs`, Linux paths) that Steam's container cannot see by itself, or None.
/// Proton 8 and newer run inside the Steam Linux Runtime, which shares only the home folder, /mnt, /media, /run/media and Steam's
/// libraries: a dump in /games or /cracks does not exist inside it, and the game aborts on its first file (#142). Folders inside
/// another one listed are left out; a path holding ':' (the list separator) cannot be passed.
pub fn container_mounts(dirs: &[PathBuf], home: Option<&Path>) -> Option<String> {
    let shared = ["/mnt", "/media", "/run/media"];
    let mut out: Vec<&Path> = vec![];
    for d in dirs {
        let s = d.to_string_lossy();
        if !s.starts_with('/') || s.contains(':') || shared.iter().any(|p| d.starts_with(p)) || home.is_some_and(|h| d.starts_with(h)) {
            continue;
        }
        if !out.iter().any(|k| d.starts_with(k)) {
            out.retain(|k| !k.starts_with(d));
            out.push(d);
        }
    }
    (!out.is_empty()).then(|| out.iter().map(|d| d.to_string_lossy().into_owned()).collect::<Vec<_>>().join(":"))
}

/// The Linux folders fefw.ini in `dir` points the game at ([fs] romfs, exefs, mods, [aoc] dir; Z:/x is /x) and the run folder.
fn ini_dirs(dir: &Path) -> Vec<PathBuf> {
    let ini = crate::ini::Ini::parse(&std::fs::read_to_string(dir.join(crate::settings::GAME_INI)).unwrap_or_default());
    let mut v = vec![dir.to_path_buf()];
    for (s, k) in [("fs", "romfs"), ("fs", "exefs"), ("fs", "mods"), ("aoc", "dir")] {
        let p = ini.get(s, k).unwrap_or_default().replace('\\', "/");
        if let Some(rest) = p.strip_prefix("Z:").or_else(|| p.strip_prefix("z:")) {
            v.push(PathBuf::from(rest));
        } else if p.starts_with('/') {
            v.push(PathBuf::from(p));
        }
    }
    v
}

/// "The game stopped: <cause>" from a crash report whose first line is "LIKELY CAUSE: <cause>" (runtime crash_set_hint), else None.
fn likely_cause(report: &str) -> Option<String> {
    let cause = report.lines().next()?.strip_prefix("LIKELY CAUSE: ")?.trim();
    (!cause.is_empty()).then(|| format!("The game stopped: {cause}"))
}

/// The explanation in fefw_fatal.txt: the text after its first line (a heading), on one line.
fn fatal_message(text: &str) -> String {
    let body = text.split_once('\n').map_or(text, |(_, rest)| rest);
    body.split_whitespace().collect::<Vec<_>>().join(" ")
}

pub fn open_with_shell(path: &Path) -> Result<(), String> {
    let mut c = Command::new(if cfg!(windows) { "explorer.exe" } else { "xdg-open" });
    crate::setup::host_env(&mut c);
    c.arg(path).spawn().map(|_| ()).map_err(|e| format!("Could not open {}: {}", path.display(), e))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_fatal_file_gives_its_explanation() {
        let t = "fefw cannot find the game's files.\n\nThe game's files cannot be read:\n  /x/romfs\n\nRun Set up again.\n";
        assert_eq!(fatal_message(t), "The game's files cannot be read: /x/romfs Run Set up again.");
        assert_eq!(fatal_message(""), "");
        assert_eq!(likely_cause("LIKELY CAUSE: the files are incomplete.\n\nHOST EXCEPTION").as_deref(), Some("The game stopped: the files are incomplete."));
        assert_eq!(likely_cause("HOST EXCEPTION 0xC0000005\n"), None);
    }

    #[test]
    fn folders_outside_the_container_are_mounted() {
        let p = |s: &str| PathBuf::from(s);
        let home = p("/home/me");
        let dirs = [p("/home/me/fefw"), p("/cracks/ROMs/FEFW/romfs"), p("/cracks/ROMs/FEFW/exefs"), p("/cracks/ROMs"), p("/mnt/sd/x"), p("/run/media/me/sd")];
        assert_eq!(container_mounts(&dirs, Some(&home)).as_deref(), Some("/cracks/ROMs"));
        assert_eq!(container_mounts(&[p("/home/me/a"), p("/media/b"), p("/a:b")], Some(&home)), None);
        assert_eq!(container_mounts(&[p("/games/r"), p("/srv/d")], None).as_deref(), Some("/games/r:/srv/d"));
    }

    #[test]
    fn a_runner_setting_is_split_into_words() {
        if cfg!(windows) {
            assert!(runner("wine").unwrap().is_empty());
        } else {
            assert_eq!(runner("  env WINEPREFIX=/p  wine ").unwrap(), ["env", "WINEPREFIX=/p", "wine"]);
            assert_eq!(runner(r#""/s/Proton - Experimental/proton" run"#).unwrap(), ["/s/Proton - Experimental/proton", "run"]);
            assert_eq!(runner(r"/a\ b/wine '' x").unwrap(), ["/a b/wine", "", "x"]);
            assert!(is_umu(&words("env GAMEID=1 /usr/bin/umu-run")));
            assert!(is_umu(&words("env -u WINEPREFIX -i umu-run")));
            assert!(is_umu(&words("PROTONPATH=/p umu-run")));
            assert_eq!(words(r#""C:\games" "a\"b""#), [r"C:\games", r#"a"b"#]);
            assert!(!is_umu(&words("wine")));
            let vdf = "\"path\"\t\t\"/mnt/a b\"\n  \"require_tool_appid\" \"1628350\"\n";
            assert_eq!(vdf_values(vdf, "path"), ["/mnt/a b"]);
            assert_eq!(vdf_values(vdf, "require_tool_appid"), ["1628350"]);
        }
    }
}
