// SPDX-License-Identifier: GPL-3.0-or-later
//! Launcher for the fefw recomp (ObeliskChamber.exe). It works on a checkout of this repository: it reads the player's own dump, runs the
//! repository's own lift and build scripts on this PC, writes ObeliskChamber.ini in a run folder and starts the game. It ships no game files.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod cpu;
mod dump;
mod ini;
mod locale;
mod nup;
mod play;
mod settings;
mod setup;
mod steam;
mod update;

use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;
use std::sync::Arc;
use tauri::Emitter;
use tauri::{AppHandle, Manager, State};
use tauri_plugin_dialog::DialogExt;

/// The launcher's own options, in the user's app config folder.
#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
struct Config {
    /// The repository checkout. Empty: found from the launcher's own location.
    repo_dir: String,
    /// The dump folder the player picked (exefs/ and romfs/, or base/ and updates/).
    dump_dir: String,
    /// Where ObeliskChamber.ini, controls.ini, the log and the saves live. Empty: <repo>/work/play.
    game_dir: String,
    /// Build processes. 0: one fewer than the CPU threads.
    jobs: u32,
    minimize_on_play: bool,
    /// do not ask GitHub for a newer release when the launcher starts
    skip_update_check: bool,
    /// Linux: the command that starts ObeliskChamber.exe (Wine or Proton, e.g. `umu-run` or `wine`). Empty: Steam's own Proton if it is
    /// installed, else umu-run, else wine. Unused on Windows.
    runner: String,
    /// The launcher's own interface language ("en", "zh", or the code of a language pack in locales/). Empty: English.
    lang: String,
    /// Draw the launcher with the game's own menu art, read from the dump when the launcher starts (read_game_layout).
    game_art: bool,
}

fn config_path(app: &AppHandle) -> Result<PathBuf, String> {
    let dir = app.path().app_config_dir().map_err(|e| e.to_string())?;
    fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {}", dir.display(), e))?;
    Ok(dir.join("launcher.json"))
}

fn load_config(app: &AppHandle) -> Config {
    config_path(app).ok().and_then(|p| fs::read_to_string(p).ok()).and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
}

fn exe_dir() -> PathBuf {
    // an AppImage runs from a temporary mount: the package is the folder the .AppImage file is in
    #[cfg(target_os = "linux")]
    if let Some(d) = std::env::var_os("APPIMAGE").map(PathBuf::from).and_then(|p| p.parent().map(|d| d.to_path_buf())) {
        return d;
    }
    std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.to_path_buf())).unwrap_or_else(|| PathBuf::from("."))
}

/// The release package's tools/ folder, when the launcher runs from the release zip (tools/build/fefw_build.py next to it).
fn package_tools() -> Option<PathBuf> {
    let t = exe_dir().join("tools");
    t.join("build").join("fefw_build.py").is_file().then_some(t)
}

fn is_repo(dir: &Path) -> bool {
    dir.join("recomp").join("games").join("fefw").join("config.py").is_file() && dir.join("runtime").join("CMakeLists.txt").is_file()
}

/// The checkout the launcher runs in: the configured one, else the first parent of the launcher's exe that is one.
fn repo_of(cfg: &Config) -> Option<PathBuf> {
    if package_tools().is_some() {
        // the release package: the install folder plays the checkout's part
        return Some(if cfg.game_dir.trim().is_empty() { exe_dir() } else { PathBuf::from(cfg.game_dir.trim()) });
    }
    if !cfg.repo_dir.trim().is_empty() {
        let p = PathBuf::from(cfg.repo_dir.trim());
        return is_repo(&p).then_some(p);
    }
    let exe = std::env::current_exe().ok()?;
    exe.ancestors().skip(1).take(8).find(|d| is_repo(d)).map(|d| d.to_path_buf())
}

fn game_dir_of(cfg: &Config, repo: &Path) -> PathBuf {
    if cfg.game_dir.trim().is_empty() {
        if package_tools().is_some() {
            return exe_dir();
        }
        repo.join("work").join("play")
    } else {
        PathBuf::from(cfg.game_dir.trim())
    }
}

fn need_repo(cfg: &Config) -> Result<PathBuf, String> {
    repo_of(cfg).ok_or_else(|| "The repository was not found. Build the launcher inside a checkout of this project, or set the repository folder in Settings.".to_string())
}

/// Setup's paths from the configuration, when the dump is usable.
fn paths(cfg: &Config) -> Result<setup::Paths, String> {
    let repo = need_repo(cfg)?;
    let report = dump::scan(&cfg.dump_dir, &builds_of(&repo));
    let chosen = report.chosen.ok_or_else(|| {
        if cfg.dump_dir.trim().is_empty() { "Choose your game files first.".to_string() } else { format!("The game files at {} are not usable: {}", cfg.dump_dir, report.errors.join(" ")) }
    })?;
    let game_dir = game_dir_of(cfg, &repo);
    Ok(setup::Paths {
        repo,
        exefs: PathBuf::from(chosen.exefs),
        romfs: PathBuf::from(chosen.romfs),
        dlc: report.dlc_dir.map(PathBuf::from),
        game_dir,
        package: package_tools(),
    })
}

/// The supported builds of main: the package's dump_hashes.json, else the checkout's recomp/games/fefw/config.py.
fn builds_of(repo: &Path) -> Vec<(String, String)> {
    if let Some(t) = package_tools() {
        let v: serde_json::Value = fs::read_to_string(t.join("build").join("dump_hashes.json")).ok().and_then(|x| serde_json::from_str(&x).ok()).unwrap_or_default();
        if let (Some(id), Some(ver)) = (v["main_build_id"].as_str(), v["version"].as_str()) {
            return vec![(id.to_uppercase(), ver.to_string())];
        }
        return vec![];
    }
    dump::supported_builds(repo)
}

fn jobs_of(cfg: &Config) -> usize {
    if cfg.jobs > 0 {
        return cfg.jobs as usize;
    }
    std::thread::available_parallelism().map(|n| n.get().saturating_sub(1)).unwrap_or(2).max(1)
}

#[derive(Serialize)]
struct Overview {
    repo_dir: String,
    repo_ok: bool,
    game_dir: String,
    exe: String,
    exe_exists: bool,
}

#[tauri::command]
fn get_config(app: AppHandle) -> Config {
    load_config(&app)
}

#[tauri::command]
fn save_config(app: AppHandle, config: Config) -> Result<(), String> {
    let text = serde_json::to_string_pretty(&config).map_err(|e| e.to_string())?;
    settings::write_atomic(&config_path(&app)?, &text).map_err(|e| format!("cannot save launcher settings: {e}"))
}

/// The layout archives the launcher's "game art" look reads (launcher/src/gameart.js).
const GAME_LAYOUTS: [&str; 4] = ["Title.arc", "CommonMenu.arc", "Common.arc", "Textures/Common.arc"];

/// One of the game's layout archives, from the player's own dump, unpacked (zstd). The bytes go to the page as they are and are
/// written nowhere. The archive is looked for in the dump's romfs/Layout: the folder itself, base/ and each of updates/.
#[tauri::command]
fn read_game_layout(app: AppHandle, name: String) -> Result<tauri::ipc::Response, String> {
    if !GAME_LAYOUTS.contains(&name.as_str()) {
        return Err(format!("not a layout the launcher reads: {name}"));
    }
    let cfg = load_config(&app);
    if cfg.dump_dir.is_empty() {
        return Err("no game files chosen yet".into());
    }
    let root = PathBuf::from(&cfg.dump_dir);
    let mut places = vec![root.join("romfs"), root.join("base").join("romfs")];
    if let Ok(entries) = fs::read_dir(root.join("updates")) {
        for e in entries.flatten() {
            places.push(e.path().join("romfs"));
        }
    }
    let rel: PathBuf = name.split('/').collect();
    for romfs in places {
        let plain = romfs.join("Layout").join(&rel);
        let mut packed = plain.clone().into_os_string();
        packed.push(".zst");
        let packed = PathBuf::from(packed);
        if packed.is_file() {
            let data = fs::read(&packed).map_err(|e| format!("cannot read {}: {}", packed.display(), e))?;
            let bytes = zstd::stream::decode_all(&data[..]).map_err(|e| format!("cannot unpack {}: {}", packed.display(), e))?;
            return Ok(tauri::ipc::Response::new(bytes));
        }
        if plain.is_file() {
            let bytes = fs::read(&plain).map_err(|e| format!("cannot read {}: {}", plain.display(), e))?;
            return Ok(tauri::ipc::Response::new(bytes));
        }
    }
    Err(format!("{name} is not in the chosen game files"))
}

#[tauri::command]
fn get_overview(app: AppHandle) -> Overview {
    let cfg = load_config(&app);
    match repo_of(&cfg) {
        Some(repo) => {
            let exe = if package_tools().is_some() { game_dir_of(&cfg, &repo).join("ObeliskChamber.exe") } else { setup::exe_path(&repo) };
            Overview {
                repo_dir: dump::fwd(&repo),
                repo_ok: true,
                game_dir: dump::fwd(&game_dir_of(&cfg, &repo)),
                exe_exists: exe.is_file(),
                exe: dump::fwd(&exe),
            }
        }
        None => Overview { repo_dir: cfg.repo_dir.clone(), repo_ok: false, game_dir: String::new(), exe: String::new(), exe_exists: false },
    }
}

#[tauri::command]
async fn pick_folder(app: AppHandle, title: String, start: Option<String>) -> Option<String> {
    let mut dlg = app.dialog().file().set_title(title);
    if let Some(s) = start.filter(|s| PathBuf::from(s).is_dir()) {
        dlg = dlg.set_directory(s);
    }
    dlg.blocking_pick_folder().and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}

#[tauri::command]
async fn pick_nup(app: AppHandle, start: Option<String>) -> Option<String> {
    let mut dlg = app.dialog().file().set_title("Choose your .nup package").add_filter("NUP package", &["nup"]);
    if let Some(s) = start.filter(|s| PathBuf::from(s).is_dir()) {
        dlg = dlg.set_directory(s);
    }
    dlg.blocking_pick_file().and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}

#[derive(Serialize, Clone)]
struct NupProgress {
    done: u64,
    total: u64,
    file: String,
}

/// Unpacks a .nup (https://github.com/a55uka/nup) into `<repo>/game/nup_dump` (base/ and updates/<version>/, the layout of a dump
/// folder) and returns that folder. Emits "nup-progress". A finished unpack of the same package is reused.
#[tauri::command]
async fn import_nup(app: AppHandle, state: State<'_, Arc<setup::Shared>>, path: String) -> Result<String, String> {
    if state.progress.lock().unwrap().state == "running" {
        return Err("Setup is running. Pause it first.".into());
    }
    let cfg = load_config(&app);
    let repo = need_repo(&cfg)?;
    let out = if package_tools().is_some() { game_dir_of(&cfg, &repo).join("nup_dump") } else { repo.join("game").join("nup_dump") };
    let src = PathBuf::from(path.trim());
    let emitter = app.clone();
    let res = tauri::async_runtime::spawn_blocking(move || {
        let mut last = 0u64;
        nup::extract(
            &src,
            &out,
            &mut |done, total, file| {
                if total == 0 || done == total || done - last >= total / 100 {
                    last = done;
                    let _ = emitter.emit("nup-progress", NupProgress { done, total, file: file.to_string() });
                }
            },
            &|| false,
        )
    })
    .await
    .map_err(|e| e.to_string())??;
    Ok(dump::fwd(&res))
}

#[tauri::command]
fn validate_dump(app: AppHandle, path: String) -> Result<dump::DumpReport, String> {
    Ok(dump::scan(&path, &builds_of(&need_repo(&load_config(&app))?)))
}

#[tauri::command]
fn save_dump(app: AppHandle, path: String) -> Result<dump::DumpReport, String> {
    let mut cfg = load_config(&app);
    let report = dump::scan(&path, &builds_of(&need_repo(&cfg)?));
    if report.chosen.is_none() {
        return Err(report.errors.join(" "));
    }
    cfg.dump_dir = report.path.clone();
    save_config(app, cfg)?;
    Ok(report)
}

#[tauri::command]
fn read_settings(app: AppHandle) -> Result<settings::SettingsDump, String> {
    let cfg = load_config(&app);
    Ok(settings::read(&game_dir_of(&cfg, &need_repo(&cfg)?)))
}

#[tauri::command]
fn write_settings(app: AppHandle, edits: Vec<settings::Edit>) -> Result<(), String> {
    let cfg = load_config(&app);
    settings::apply(&game_dir_of(&cfg, &need_repo(&cfg)?), &edits)
}

/// What one upscaler / frame generation feature finds (Settings): `found` only when every file it needs is in one folder.
#[derive(Serialize)]
struct FeatureFiles {
    found: bool,
    /// the folder the game will use (found), or the files missing and where they were looked for
    detail: String,
}

#[derive(Serialize)]
struct UpscalerFiles {
    fsr_upscaling: FeatureFiles,
    fsr_framegen: FeatureFiles,
    dlss: FeatureFiles,
    dlss_framegen: FeatureFiles,
    xess: FeatureFiles,
}

/// The folders the game searches (runtime/src/gfx/extras/ffx_files.cpp, temporal_dlss.cpp, temporal_xess.cpp): the setting's folder (a file in it counts), then
/// `upscalers/<sub>` next to the game, next to the launcher and in the run folder.
fn upscaler_dirs(setting: &str, sub: &str, game: &Path, exe: &Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = Vec::new();
    let s = setting.trim();
    if !s.is_empty() {
        let p = PathBuf::from(s);
        v.push(if p.is_file() { p.parent().map(|d| d.to_path_buf()).unwrap_or(p) } else { p });
    }
    for base in [exe.parent().map(|d| d.to_path_buf()).unwrap_or_default(), exe_dir(), game.to_path_buf()] {
        let d = base.join("upscalers").join(sub);
        if !v.contains(&d) {
            v.push(d);
        }
    }
    v
}

fn files_in(dirs: &[PathBuf], sets: &[&[&str]], what: &str) -> FeatureFiles {
    for d in dirs {
        for set in sets {
            if set.iter().all(|f| d.join(f).is_file()) {
                return FeatureFiles { found: true, detail: dump::fwd(d) };
            }
        }
    }
    // the first folder holding any of the files says which ones it lacks
    for d in dirs {
        let set = sets[0];
        let have: Vec<&str> = set.iter().copied().filter(|f| d.join(f).is_file()).collect();
        if !have.is_empty() {
            let lack: Vec<&str> = set.iter().copied().filter(|f| !d.join(f).is_file()).collect();
            return FeatureFiles { found: false, detail: format!("{} lacks {}", dump::fwd(d), lack.join(", ")) };
        }
    }
    FeatureFiles { found: false, detail: format!("{} not found in {}", what, dirs.iter().map(|d| dump::fwd(d)).collect::<Vec<_>>().join(", ")) }
}

#[tauri::command]
fn upscaler_files(app: AppHandle, fsr_dir: String, dlss_dir: String, xess_dir: Option<String>) -> Result<UpscalerFiles, String> {
    let cfg = load_config(&app);
    let repo = need_repo(&cfg)?;
    let game = game_dir_of(&cfg, &repo);
    let exe = if package_tools().is_some() { game.join("ObeliskChamber.exe") } else { setup::exe_path(&repo) };
    let fsr = upscaler_dirs(&fsr_dir, "fsr", &game, &exe);
    // Streamline: the game takes the setting's folder, else the first default folder holding sl.interposer.dll, and needs every file there
    let sl = upscaler_dirs(&dlss_dir, "streamline", &game, &exe);
    let sl_pick: Vec<PathBuf> = if !dlss_dir.trim().is_empty() {
        sl[..1].to_vec()
    } else {
        sl.iter().find(|d| d.join("sl.interposer.dll").is_file()).cloned().map(|d| vec![d]).unwrap_or(sl.clone())
    };
    // XeSS (runtime/src/gfx/extras/temporal_xess.cpp): libxess.dll in the setting's folder, upscalers/xess, else next to the game
    let mut xess = upscaler_dirs(xess_dir.as_deref().unwrap_or(""), "xess", &game, &exe);
    if let Some(d) = exe.parent() {
        xess.push(d.to_path_buf());
    }
    const DLSS: [&str; 4] = ["sl.interposer.dll", "sl.common.dll", "sl.dlss.dll", "nvngx_dlss.dll"];
    const DLSS_G: [&str; 9] =
        ["sl.interposer.dll", "sl.common.dll", "sl.dlss.dll", "nvngx_dlss.dll", "sl.dlss_g.dll", "sl.reflex.dll", "sl.pcl.dll", "nvngx_dlssg.dll", "NvLowLatencyVk.dll"];
    Ok(UpscalerFiles {
        fsr_upscaling: files_in(&fsr, &[&["amd_fidelityfx_upscaler_vk.dll"], &["amd_fidelityfx_vk.dll"]], "amd_fidelityfx_upscaler_vk.dll"),
        fsr_framegen: files_in(&fsr, &[&["amd_fidelityfx_vk.dll"]], "amd_fidelityfx_vk.dll (FidelityFX SDK v1.1.4)"),
        dlss: files_in(&sl_pick, &[&DLSS], "the Streamline files"),
        dlss_framegen: files_in(&sl_pick, &[&DLSS_G], "the Streamline frame generation files"),
        xess: files_in(&xess, &[&["libxess.dll"]], "libxess.dll"),
    })
}

#[derive(Serialize)]
struct SetupStatus {
    progress: setup::Progress,
    steps: Vec<setup::StepState>,
    complete: bool,
    tools: Vec<setup::Tool>,
    log: Vec<String>,
    jobs: usize,
    /// running from the release package (no build tools needed)
    package: bool,
    /// why this PC's processor cannot run the compiled game (missing CMPXCHG16B / SSE4.2 / POPCNT), if it cannot
    cpu_problem: Option<String>,
    /// set when this processor gets the compatible build of the game code (no FMA3: cpu.rs)
    cpu_note: Option<String>,
}

#[tauri::command]
fn setup_status(app: AppHandle, state: State<Arc<setup::Shared>>) -> SetupStatus {
    let cfg = load_config(&app);
    let mut progress = state.progress.lock().unwrap().clone();
    if progress.state.is_empty() {
        progress.state = "idle".into();
    }
    let running = (progress.state == "running").then(|| progress.step.clone());
    let steps = paths(&cfg).map(|p| setup::steps(&p, running.as_deref())).unwrap_or_default();
    SetupStatus {
        complete: !steps.is_empty() && steps.iter().all(|s| s.state == "done"),
        steps,
        progress,
        tools: setup::check_tools(package_tools().is_some()),
        package: package_tools().is_some(),
        cpu_problem: cpu::check(),
        cpu_note: cpu::note(),
        log: state.log.lock().unwrap().iter().cloned().collect(),
        jobs: jobs_of(&cfg),
    }
}

#[tauri::command]
fn start_setup(app: AppHandle, state: State<Arc<setup::Shared>>) -> Result<(), String> {
    if let Some(why) = cpu::check() {
        return Err(why);
    }
    let cfg = load_config(&app);
    let p = paths(&cfg)?;
    {
        let mut pr = state.progress.lock().unwrap();
        if pr.state == "running" {
            return Err("Setup is already running.".into());
        }
        *pr = setup::Progress { state: "running".into(), step: "python".into(), message: "Starting...".into(), ..Default::default() };
    }
    let shared = state.inner().clone();
    shared.cancel.store(false, Ordering::SeqCst);
    let jobs = jobs_of(&cfg);
    std::thread::spawn(move || {
        let a1 = app.clone();
        let a2 = app.clone();
        let emit = move |p: setup::Progress| {
            let _ = a1.emit("setup-status", p);
        };
        let emit_log = move |lines: Vec<String>| {
            let _ = a2.emit("setup-log", lines);
        };
        let end = setup::run(&shared, &p, jobs, &emit, &emit_log);
        let _ = app.emit("setup-status", end);
    });
    Ok(())
}

#[tauri::command]
fn cancel_setup(state: State<Arc<setup::Shared>>) {
    state.cancel.store(true, Ordering::SeqCst);
    setup::kill_tree(state.pid.load(Ordering::SeqCst));
}

#[tauri::command]
fn play(app: AppHandle, state: State<Arc<setup::Shared>>) -> Result<(), String> {
    if state.progress.lock().unwrap().state == "running" {
        return Err("Setup is running. Wait for it or pause it first.".into());
    }
    let cfg = load_config(&app);
    let p = paths(&cfg)?;
    if !setup::complete(&p) {
        return Err("Setup is not finished: open Setup and press Set up.".into());
    }
    settings::ensure_play_paths(&p.game_dir, &p)?;
    play::start(&app, &p.exe(), &p.game_dir, &exe_dir(), cfg.minimize_on_play, &cfg.runner)
}

/// Linux: whether Steam is installed and the shortcut to ObeliskChamber.exe exists.
#[tauri::command]
async fn steam_status(app: AppHandle) -> steam::State {
    // off the main thread: it reads the dump folder and walks /proc
    let none = steam::State { available: false, running: false, added: false };
    if !cfg!(target_os = "linux") {
        return none;
    }
    tauri::async_runtime::spawn_blocking(move || match paths(&load_config(&app)) {
        Ok(p) => steam::state(&p.exe()),
        Err(_) => steam::State { available: false, running: false, added: false },
    })
    .await
    .unwrap_or(none)
}

/// Linux: adds ObeliskChamber.exe to Steam as a non-Steam game. Steam has to be closed for that: when it runs, the player is asked first.
#[tauri::command]
async fn add_to_steam(app: AppHandle) -> Result<String, String> {
    use tauri_plugin_dialog::{MessageDialogButtons, MessageDialogKind};
    let cfg = load_config(&app);
    let p = paths(&cfg)?;
    let exe = p.exe();
    if !exe.is_file() {
        return Err("ObeliskChamber.exe is not built yet. Run Set up first.".into());
    }
    let close = if steam::state(&exe).running {
        app.dialog()
            .message("Steam saves its game list when it closes, so it has to close while the launcher adds the game. It is started again right after.")
            .title("Add to Steam")
            .kind(MessageDialogKind::Info)
            .buttons(MessageDialogButtons::OkCancelCustom("Close Steam and add".into(), "Cancel".into()))
            .blocking_show()
    } else {
        false
    };
    if steam::state(&exe).running && !close {
        return Err("Not added: Steam is still running.".into());
    }
    // the upscaler DLLs next to the launcher: ObeliskChamber.exe finds them by itself when the run folder is the install folder
    let launcher_dir = (p.game_dir != exe_dir()).then(|| dump::game_path(&exe_dir()));
    let game_dir = p.game_dir.clone();
    // what ObeliskChamber.ini points the game at, for Flatpak Steam's sandbox
    let mut reads = vec![p.game_dir.clone(), p.romfs.clone(), p.plain_exefs(), exe_dir()];
    reads.extend(p.dlc.clone());
    tauri::async_runtime::spawn_blocking(move || steam::add(&exe, &game_dir, launcher_dir.as_deref(), &reads, close)).await.map_err(|e| e.to_string())?
}

#[tauri::command]
fn game_status(state: State<play::PlayState>) -> play::Status {
    state.0.lock().unwrap().clone()
}

#[tauri::command]
fn open_game_file(app: AppHandle, name: String) -> Result<(), String> {
    if !play::REPORT_FILES.contains(&name.as_str()) {
        return Err("That file cannot be opened from here.".into());
    }
    let cfg = load_config(&app);
    let path = game_dir_of(&cfg, &need_repo(&cfg)?).join(&name);
    if !path.is_file() {
        return Err(format!("{name} does not exist."));
    }
    play::open_with_shell(&path)
}

#[tauri::command]
fn open_game_dir(app: AppHandle) -> Result<(), String> {
    let cfg = load_config(&app);
    let dir = game_dir_of(&cfg, &need_repo(&cfg)?);
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    play::open_with_shell(&dir)
}

/// Opens <run folder>/mods ([fs] mods), creating it: RomFS mods go in mods/<name>/romfs.
#[tauri::command]
fn open_mods_dir(app: AppHandle) -> Result<(), String> {
    let cfg = load_config(&app);
    let dir = game_dir_of(&cfg, &need_repo(&cfg)?).join("mods");
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    play::open_with_shell(&dir)
}

#[tauri::command]
fn open_setup_log(app: AppHandle) -> Result<(), String> {
    let cfg = load_config(&app);
    let repo = need_repo(&cfg)?;
    let log = if package_tools().is_some() { game_dir_of(&cfg, &repo).join("setup.log") } else { repo.join("work").join("setup.log") };
    if !log.is_file() {
        return Err("There is no setup log yet.".into());
    }
    play::open_with_shell(&log)
}

#[tauri::command]
async fn check_update(app: AppHandle) -> Result<update::Check, String> {
    // FEFW_LAUNCHER_PRETEND_VERSION: test the update path against the published release from an older-looking launcher
    let current = std::env::var("FEFW_LAUNCHER_PRETEND_VERSION").ok().filter(|v| update::parse_version(v).is_some()).unwrap_or_else(|| update::BUILD_ID.map(String::from).unwrap_or_else(|| app.package_info().version.to_string()));
    let package = package_tools().is_some();
    tauri::async_runtime::spawn_blocking(move || update::check(&current, package)).await.map_err(|e| e.to_string())?
}

#[tauri::command]
fn update_status(state: State<Arc<update::Shared>>) -> update::Status {
    let mut s = state.status.lock().unwrap().clone();
    if s.state.is_empty() {
        s.state = "idle".into();
    }
    s
}

#[tauri::command]
fn start_update(
    app: AppHandle,
    state: State<Arc<update::Shared>>,
    setup_state: State<Arc<setup::Shared>>,
    play_state: State<play::PlayState>,
    release: update::Release,
) -> Result<(), String> {
    if package_tools().is_none() {
        return Err("Updates are for the release package. In a checkout, use git.".into());
    }
    if release.asset_url.is_empty() {
        return Err(format!("Release {} has no package for this system yet.", release.version));
    }
    if setup_state.progress.lock().unwrap().state == "running" {
        return Err("Setup is running. Wait for it or pause it first.".into());
    }
    if play_state.0.lock().unwrap().state == "running" {
        return Err("The game is running. Close it first.".into());
    }
    {
        let mut s = state.status.lock().unwrap();
        if s.state == "running" {
            return Err("An update is already running.".into());
        }
        *s = update::Status { state: "running".into(), phase: "download".into(), message: "Starting".into(), version: release.version.clone(), ..Default::default() };
    }
    let cfg = load_config(&app);
    let install = exe_dir();
    let game_dir = repo_of(&cfg).map(|r| game_dir_of(&cfg, &r)).unwrap_or_else(|| install.clone());
    let shared = state.inner().clone();
    shared.cancel.store(false, Ordering::SeqCst);
    std::thread::spawn(move || {
        let a = app.clone();
        let emit = move |s: update::Status| {
            let _ = a.emit("update-status", s);
        };
        let run = update::Run { shared: &shared, install, game_dir, emit: &emit };
        let end = match run.run(&release) {
            Ok(s) => s,
            Err(e) => update::Status { state: "error".into(), message: e, version: release.version.clone(), ..Default::default() },
        };
        *shared.status.lock().unwrap() = end.clone();
        let _ = app.emit("update-status", end);
    });
    Ok(())
}

#[tauri::command]
fn cancel_update(state: State<Arc<update::Shared>>) {
    state.cancel.store(true, Ordering::SeqCst);
}

/// Starts the (new) launcher from the install folder and closes this one.
#[tauri::command]
fn restart_launcher(app: AppHandle) -> Result<(), String> {
    let exe = match std::env::var_os("APPIMAGE") {
        Some(a) if !cfg!(windows) => PathBuf::from(a),
        // the launcher in the install folder: under its name, or the old one when the package is from before the rename
        _ => {
            let ext = if cfg!(windows) { ".exe" } else { "" };
            let new = exe_dir().join(format!("ObeliskChamber-launcher{ext}"));
            let old = exe_dir().join(format!("fefw-launcher{ext}"));
            if !new.is_file() && old.is_file() { old } else { new }
        }
    };
    let mut c = std::process::Command::new(&exe);
    setup::host_env(&mut c); // not this AppImage's environment: its mount goes away when this launcher exits
    c.current_dir(exe_dir()).spawn().map_err(|e| format!("cannot start {}: {e}", exe.display()))?;
    app.exit(0);
    Ok(())
}

fn main() {
    if package_tools().is_some() {
        update::cleanup(&exe_dir());
    }
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(play::PlayState::default())
        .manage(Arc::new(setup::Shared::default()))
        .manage(Arc::new(update::Shared::default()))
        .on_window_event(|window, event| {
            // closing the window pauses a running setup rather than leaving its compilers behind
            if let tauri::WindowEvent::Destroyed = event {
                let st = window.state::<Arc<setup::Shared>>();
                st.cancel.store(true, Ordering::SeqCst);
                setup::kill_tree(st.pid.load(Ordering::SeqCst));
            }
        })
        .invoke_handler(tauri::generate_handler![
            get_config,
            save_config,
            get_overview,
            read_game_layout,
            pick_folder,
            pick_nup,
            import_nup,
            validate_dump,
            save_dump,
            read_settings,
            write_settings,
            upscaler_files,
            setup_status,
            start_setup,
            cancel_setup,
            play,
            game_status,
            steam_status,
            add_to_steam,
            open_game_file,
            open_game_dir,
            open_mods_dir,
            open_setup_log,
            check_update,
            update_status,
            start_update,
            cancel_update,
            restart_launcher,
            locale::list_locales,
            locale::read_locale,
        ])
        .run(tauri::generate_context!())
        .expect("error while running the launcher");
}
