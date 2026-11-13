// SPDX-License-Identifier: GPL-3.0-or-later
//! Updates of the release package from inside the launcher: asks GitHub for the latest release, downloads its package for this
//! platform (the -win64.zip, or the -linux-x64.tar.gz) with curl (Windows' own curl.exe), checks it against the SHA-256 digest
//! GitHub lists for the asset, unpacks it with tar (Windows' own tar.exe) and moves the new files over the install folder. Everything the old package had is moved to update/old first and moved back if any step fails.
//! The player's files (fefw.exe, fefw.ini, controls.ini, save/, shader_cache/, build/, logs) are not in the zip and stay as they are.
//! Afterwards the new package's `fefw_build.py --check` says whether fefw.exe has to be built again (Set up).

use serde::{Deserialize, Serialize};
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use std::time::Duration;

/// The GitHub repository whose releases the launcher follows. FEFW_UPDATE_REPO at build time: a fork's launcher follows the fork.
pub const REPO: &str = match option_env!("FEFW_UPDATE_REPO") {
    Some(r) => r,
    None => "Goldoire/fefw-recomp",
};
/// FEFW_BUILD_ID at build time: the release tag this launcher was built for (a fork tagging its releases by commit). Any other
/// latest release then counts as newer, as tags like that have no order.
pub const BUILD_ID: Option<&str> = option_env!("FEFW_BUILD_ID");
/// The release asset for this platform, and files a downloaded package must have.
#[cfg(windows)]
const PACKAGE: (&str, &str, [&str; 4]) = ("-win64.zip", "Windows", ["fefw-launcher.exe", "fefw_rt.dll", "tools/build/fefw_build.py", "tools/python/python.exe"]);
#[cfg(not(windows))]
const PACKAGE: (&str, &str, [&str; 4]) =
    ("-linux-x64.tar.gz", "Linux", ["fefw-launcher.AppImage", "fefw_rt.dll", "tools/build/fefw_build.py", "tools/python/bin/python3"]);
const AGENT: &str = "fefw-launcher";
const CURL: &str = if cfg!(windows) { "curl.exe" } else { "curl" };
const TAR: &str = if cfg!(windows) { "tar.exe" } else { "tar" };
/// The launcher's working folder inside the install folder: downloads, the unpacked package, the previous version's files.
const WORK: &str = "update";

#[derive(Serialize, Deserialize, Clone, Default)]
pub struct Release {
    pub version: String,
    pub name: String,
    pub notes: String,
    pub page: String,
    pub asset_name: String,
    pub asset_url: String,
    pub size: u64,
    /// lowercase hex; empty when GitHub lists no digest (the update then refuses to install)
    pub sha256: String,
}

#[derive(Serialize, Clone, Default)]
pub struct Check {
    pub current: String,
    pub latest: Option<Release>,
    pub newer: bool,
    /// false when the launcher runs in a checkout of the repository (updates come from git there)
    pub package: bool,
}

#[derive(Serialize, Clone, Default)]
pub struct Status {
    /// idle, running, done, error
    pub state: String,
    /// download, verify, unpack, install, check
    pub phase: String,
    pub done: u64,
    pub total: u64,
    pub message: String,
    pub version: String,
    /// after an install: whether Set up has to build fefw.exe again (None: could not tell)
    pub rebuild: Option<bool>,
    pub rebuild_reason: String,
}

#[derive(Default)]
pub struct Shared {
    pub status: Mutex<Status>,
    pub cancel: AtomicBool,
}

/// "v1.2.3" or "1.2.3-x" -> (1, 2, 3)
pub fn parse_version(s: &str) -> Option<(u64, u64, u64)> {
    let s = s.trim().trim_start_matches(['v', 'V']);
    let mut it = s.split('.').map(|p| p.chars().take_while(|c| c.is_ascii_digit()).collect::<String>().parse::<u64>().ok());
    Some((it.next()??, it.next().flatten().unwrap_or(0), it.next().flatten().unwrap_or(0)))
}

fn system_tool(name: &str) -> PathBuf {
    if !cfg!(windows) {
        return PathBuf::from(name.trim_end_matches(".exe")); // curl and tar from PATH
    }
    let root = std::env::var_os("SystemRoot").map(PathBuf::from).unwrap_or_else(|| PathBuf::from(r"C:\Windows"));
    let p = root.join("System32").join(name);
    if p.is_file() { p } else { PathBuf::from(name) }
}

fn curl() -> Command {
    let mut c = Command::new(system_tool(CURL));
    c.args(["--fail", "--location", "--silent", "--show-error", "--user-agent", AGENT]);
    crate::setup::quiet(&mut c);
    c
}

/// The latest published release of REPO and its package for this platform. A release without one (Linux, while only the Windows
/// zip is published) comes back with an empty asset: it can be shown, not installed.
pub fn latest() -> Result<Release, String> {
    let url = format!("https://api.github.com/repos/{REPO}/releases/latest");
    let out = curl()
        .args(["--retry", "2", "--retry-all-errors", "--max-time", "30", "--header", "Accept: application/vnd.github+json", &url])
        .stdin(Stdio::null())
        .output()
        .map_err(|e| format!("cannot run {CURL}: {e}"))?;
    if !out.status.success() {
        return Err(format!("Could not reach GitHub: {}", String::from_utf8_lossy(&out.stderr).trim()));
    }
    let v: serde_json::Value = serde_json::from_slice(&out.stdout).map_err(|e| format!("GitHub sent an unexpected answer: {e}"))?;
    let tag = v["tag_name"].as_str().unwrap_or_default();
    let version = tag.trim_start_matches(['v', 'V']).to_string();
    let assets = v["assets"].as_array().cloned().unwrap_or_default();
    let none = serde_json::Value::Null;
    let zip = assets.iter().find(|a| a["name"].as_str().is_some_and(|n| n.starts_with("fefw-recomp-") && n.ends_with(PACKAGE.0))).unwrap_or(&none);
    let digest = zip["digest"].as_str().unwrap_or_default();
    Ok(Release {
        name: v["name"].as_str().filter(|s| !s.is_empty()).unwrap_or(tag).to_string(),
        notes: v["body"].as_str().unwrap_or_default().replace("\r\n", "\n"),
        page: v["html_url"].as_str().unwrap_or_default().to_string(),
        asset_name: zip["name"].as_str().unwrap_or_default().to_string(),
        asset_url: zip["browser_download_url"].as_str().unwrap_or_default().to_string(),
        size: zip["size"].as_u64().unwrap_or(0),
        sha256: digest.strip_prefix("sha256:").unwrap_or("").to_ascii_lowercase(),
        version,
    })
}

pub fn check(current: &str, package: bool) -> Result<Check, String> {
    let rel = latest()?;
    let newer = match BUILD_ID {
        Some(_) => !rel.version.is_empty() && rel.version != current.trim_start_matches(['v', 'V']),
        None => matches!((parse_version(&rel.version), parse_version(current)), (Some(a), Some(b)) if a > b),
    };
    Ok(Check { current: current.to_string(), latest: Some(rel), newer, package })
}

fn sha256_file(path: &Path) -> Result<String, String> {
    use sha2::{Digest, Sha256};
    let mut f = fs::File::open(path).map_err(|e| format!("cannot read {}: {e}", path.display()))?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf).map_err(|e| format!("cannot read {}: {e}", path.display()))?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(h.finalize().iter().map(|b| format!("{b:02x}")).collect())
}

/// Runs one update: download, verify, unpack, install, check. `emit` gets every status change.
pub struct Run<'a> {
    pub shared: &'a Shared,
    pub install: PathBuf,
    pub game_dir: PathBuf,
    pub emit: &'a dyn Fn(Status),
}

impl Run<'_> {
    fn set(&self, phase: &str, done: u64, total: u64, message: &str) {
        let s = {
            let mut st = self.shared.status.lock().unwrap();
            st.state = "running".into();
            st.phase = phase.into();
            st.done = done;
            st.total = total;
            st.message = message.into();
            st.clone()
        };
        (self.emit)(s);
    }

    fn cancelled(&self) -> Result<(), String> {
        if self.shared.cancel.load(Ordering::SeqCst) { Err("Update cancelled.".into()) } else { Ok(()) }
    }

    pub fn run(&self, rel: &Release) -> Result<Status, String> {
        if rel.sha256.len() != 64 {
            return Err("GitHub lists no checksum for this download, so it cannot be verified. Download the package from the release page instead.".into());
        }
        if rel.asset_name.contains(['/', '\\']) || rel.asset_name.contains("..") {
            return Err("The release's file name is not usable.".into());
        }
        let work = self.install.join(WORK);
        fs::create_dir_all(&work).map_err(|e| format!("cannot create {}: {e}", work.display()))?;
        let zip = self.download(rel, &work)?;
        self.cancelled()?;
        let top = self.unpack(&zip, &work.join("stage"))?;
        self.cancelled()?;
        self.set("install", 0, 0, "Installing");
        install(&self.install, &top, &work.join("old"))?;
        let _ = fs::remove_file(&zip);
        let _ = fs::remove_dir_all(work.join("stage"));
        self.set("check", 0, 0, "Checking whether the game has to be built again");
        let (rebuild, reason) = rebuild_needed(&self.install, &self.game_dir);
        let st = Status {
            state: "done".into(),
            phase: "done".into(),
            message: format!("Version {} is installed. Restart the launcher to use it.", rel.version),
            version: rel.version.clone(),
            rebuild,
            rebuild_reason: reason,
            ..Default::default()
        };
        Ok(st)
    }

    fn download(&self, rel: &Release, work: &Path) -> Result<PathBuf, String> {
        let zip = work.join(&rel.asset_name);
        // a finished download of the same file from an earlier attempt
        if zip.is_file() && fs::metadata(&zip).map(|m| m.len()).unwrap_or(0) == rel.size && sha256_file(&zip)? == rel.sha256 {
            return Ok(zip);
        }
        let part = work.join(format!("{}.part", rel.asset_name));
        let _ = fs::remove_file(&zip);
        let _ = fs::remove_file(&part);
        let mut child = curl()
            .args(["--retry", "3", "--speed-limit", "1024", "--speed-time", "60", "--output"])  // a stalled connection fails after a minute instead of hanging
            .arg(&part)
            .arg(&rel.asset_url)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::piped())
            .spawn()
            .map_err(|e| format!("cannot run {CURL}: {e}"))?;
        let mb = |b: u64| b as f64 / (1024.0 * 1024.0);
        let status = loop {
            if let Some(s) = child.try_wait().map_err(|e| e.to_string())? {
                break s;
            }
            if self.shared.cancel.load(Ordering::SeqCst) {
                let _ = child.kill();
                let _ = child.wait();
                let _ = fs::remove_file(&part);
                return Err("Update cancelled.".into());
            }
            let got = fs::metadata(&part).map(|m| m.len()).unwrap_or(0);
            self.set("download", got, rel.size, &format!("Downloading {} ({:.0} of {:.0} MiB)", rel.asset_name, mb(got), mb(rel.size)));
            std::thread::sleep(Duration::from_millis(250));
        };
        if !status.success() {
            let mut err = String::new();
            if let Some(mut e) = child.stderr.take() {
                let _ = e.read_to_string(&mut err);
            }
            let _ = fs::remove_file(&part);
            return Err(format!("The download failed: {}", err.trim()));
        }
        self.set("verify", 0, 0, "Checking the download");
        let got = sha256_file(&part)?;
        if got != rel.sha256 {
            let _ = fs::remove_file(&part);
            return Err(format!("The download is damaged (SHA-256 {got}, expected {}). Try again.", rel.sha256));
        }
        fs::rename(&part, &zip).map_err(|e| format!("cannot rename {}: {e}", part.display()))?;
        Ok(zip)
    }

    /// Unpacks the zip into `stage` and returns its package folder (fefw-recomp-<version>/).
    fn unpack(&self, zip: &Path, stage: &Path) -> Result<PathBuf, String> {
        self.set("unpack", 0, 0, "Unpacking");
        if stage.exists() {
            fs::remove_dir_all(stage).map_err(|e| format!("cannot clear {}: {e}", stage.display()))?;
        }
        fs::create_dir_all(stage).map_err(|e| format!("cannot create {}: {e}", stage.display()))?;
        let mut c = Command::new(system_tool(TAR));
        // -m: do not restore the file times. The files only need to be there; a zip with a time the file system cannot hold
        // ("Can't restore time: Invalid argument", seen on some Windows setups) must not fail the whole update.
        c.arg("-xmf").arg(zip).arg("-C").arg(stage);
        crate::setup::quiet(&mut c);
        let out = c.stdin(Stdio::null()).output().map_err(|e| format!("cannot run {TAR}: {e}"))?;
        if !out.status.success() && !only_time_errors(&String::from_utf8_lossy(&out.stderr)) {
            return Err(format!("Unpacking failed: {}", String::from_utf8_lossy(&out.stderr).trim()));
        }
        let dirs: Vec<PathBuf> = fs::read_dir(stage).map_err(|e| e.to_string())?.flatten().map(|e| e.path()).filter(|p| p.is_dir()).collect();
        let top = match dirs.as_slice() {
            [one] => one.clone(),
            _ => return Err("The package does not have the expected layout.".into()),
        };
        for need in PACKAGE.2 {
            if !top.join(need).is_file() {
                return Err(format!("The package is missing {need}."));
            }
        }
        Ok(top)
    }
}

/// True when every line tar printed is a failure to set a file time (the contents were written). The package check after
/// unpacking still catches a missing file.
fn only_time_errors(stderr: &str) -> bool {
    let mut any = false;
    for l in stderr.lines().map(str::trim).filter(|l| !l.is_empty()) {
        let l = l.to_ascii_lowercase();
        if l.contains("error exit delayed") {   // bsdtar's closing line after any non-fatal error
            continue;
        }
        if !l.contains("can't restore time") && !l.contains("cannot utime") {
            return false;
        }
        any = true;
    }
    any
}

/// Files of a tree, as paths relative to `root` (sorted).
fn walk(root: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    let mut stack = vec![PathBuf::new()];
    while let Some(rel) = stack.pop() {
        let Ok(rd) = fs::read_dir(root.join(&rel)) else { continue };
        for e in rd.flatten() {
            let r = rel.join(e.file_name());
            match e.file_type() {
                Ok(ft) if ft.is_dir() => stack.push(r),
                Ok(_) => out.push(r),
                Err(_) => {}
            }
        }
    }
    out.sort();
    out
}

/// fs::rename with a few retries: antivirus scanners and the search indexer hold new files open for a moment.
fn rename_retry(from: &Path, to: &Path) -> std::io::Result<()> {
    let mut wait = 100;
    loop {
        match fs::rename(from, to) {
            Err(e) if wait <= 1600 && e.kind() == std::io::ErrorKind::PermissionDenied => {
                std::thread::sleep(Duration::from_millis(wait));
                wait *= 2;
            }
            r => return r,
        }
    }
}

/// Puts the new package `top` in place over `install`, file by file (a folder cannot be renamed while anything holds a handle
/// under it, a file can). Each old file goes to `old` first: the files the new package replaces, and in the package's own folders
/// (those the new package has, such as tools/) the files it no longer has. Anything else in the install folder (fefw.exe,
/// fefw.ini, save/, shader_cache/, build/) is left alone. Any failure moves back what was already moved. The running launcher
/// can be moved too (Windows allows renaming a running exe; on Linux the running AppImage keeps its old file until it exits).
pub fn install(install: &Path, top: &Path, old: &Path) -> Result<(), String> {
    if old.exists() {
        fs::remove_dir_all(old).map_err(|e| format!("cannot clear {} (close any program using it): {e}", old.display()))?;
    }
    fs::create_dir_all(old).map_err(|e| format!("cannot create {}: {e}", old.display()))?;
    let new_files = walk(top);
    let new_set: std::collections::HashSet<&PathBuf> = new_files.iter().collect();
    // the package's own folders: their files that the new package does not have are stale
    let mut stale = Vec::new();
    for e in fs::read_dir(top).map_err(|e| e.to_string())?.flatten() {
        if e.path().is_dir() {
            let name = PathBuf::from(e.file_name());
            // locales/ holds the shipped language tables, but the player may have put their own translation there too:
            // a table the package does not ship is not stale, it is theirs.
            if name.to_string_lossy().eq_ignore_ascii_case("locales") {
                continue;
            }            stale.extend(walk(&install.join(&name)).into_iter().map(|r| name.join(r)).filter(|r| !new_set.contains(r)));
        }
    }
    // (relative path, the old file was moved to old/, a new file was moved in)
    let mut done: Vec<(PathBuf, bool, bool)> = Vec::new();
    let mut step = || -> Result<(), String> {
        let aside = |rel: &Path| -> Result<(), String> {
            let target = install.join(rel);
            let dest = old.join(rel);
            if let Some(d) = dest.parent() {
                fs::create_dir_all(d).map_err(|e| format!("cannot create {}: {e}", d.display()))?;
            }
            rename_retry(&target, &dest).map_err(|e| format!("cannot move {} aside (is the game or another program using it?): {e}", target.display()))
        };
        for rel in &stale {
            aside(rel)?;
            done.push((rel.clone(), true, false));
        }
        for rel in &new_files {
            let target = install.join(rel);
            // symlink_metadata: a symlink (python-build-standalone has them) whose target was moved aside still counts
            let had = fs::symlink_metadata(&target).is_ok();
            // a language table is added, never replaced: the copy in the install folder may be the player's own.
            let shared = rel.components().next().is_some_and(|c| c.as_os_str().to_string_lossy().eq_ignore_ascii_case("locales"));
            if had && shared {
                continue;
            }
            if had {
                aside(rel)?;
            }
            done.push((rel.clone(), had, false));
            if let Some(d) = target.parent() {
                fs::create_dir_all(d).map_err(|e| format!("cannot create {}: {e}", d.display()))?;
            }
            rename_retry(&top.join(rel), &target).map_err(|e| format!("cannot put {} in place: {e}", target.display()))?;
            done.last_mut().unwrap().2 = true;
        }
        Ok(())
    };
    if let Err(e) = step() {
        for (rel, had, placed) in done.iter().rev() {
            let target = install.join(rel);
            if *placed {
                let _ = fs::remove_file(&target);
            }
            if *had {
                let _ = rename_retry(&old.join(rel), &target);
            }
        }
        return Err(format!("{e} Nothing was changed."));
    }
    Ok(())
}

/// Whether the installed package's tools would rebuild fefw.exe: the package's own `fefw_build.py --check`.
pub fn rebuild_needed(install: &Path, game_dir: &Path) -> (Option<bool>, String) {
    let tools = install.join("tools");
    if !game_dir.join("fefw.exe").is_file() {
        return (Some(true), "fefw.exe is not built yet".into());
    }
    let mut c = Command::new(crate::setup::package_python(&tools));
    c.arg("-I").arg(tools.join("build").join("fefw_build.py")).arg("--check").arg("--game").arg(game_dir);
    crate::setup::quiet(&mut c);
    match c.stdin(Stdio::null()).output() {
        Ok(o) => {
            let text = String::from_utf8_lossy(&o.stdout).to_string() + &String::from_utf8_lossy(&o.stderr);
            let last = text.lines().map(str::trim).filter(|l| !l.is_empty()).last().unwrap_or_default();
            // --check prints {"current": ..., "reason": "..."}
            let reason = serde_json::from_str::<serde_json::Value>(last).ok().and_then(|v| v["reason"].as_str().map(String::from)).unwrap_or_else(|| last.to_string());
            (Some(!o.status.success()), reason)
        }
        Err(e) => (None, format!("cannot run the build check: {e}")),
    }
}

/// At start: removes what an earlier update left behind (the previous version, downloads, the unpacked package).
pub fn cleanup(install: &Path) {
    let work = install.join(WORK);
    if work.is_dir() {
        let _ = fs::remove_dir_all(work.join("old"));
        let _ = fs::remove_dir_all(work.join("stage"));
        if let Ok(rd) = fs::read_dir(&work) {
            for e in rd.flatten() {
                if e.path().extension().is_some_and(|x| x == "part") {
                    let _ = fs::remove_file(e.path());
                }
            }
        }
        let _ = fs::remove_dir(&work); // only when empty: a finished download is kept for a retry
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unpacking_tolerates_only_time_errors() {
        // #129: tar could not set the file times (some Windows file systems); the files themselves were written
        assert!(only_time_errors("fefw-recomp-0.1.7/fefw.exe: Can't restore time: Invalid argument\nfefw-recomp-0.1.7/LEGAL.txt: Can't restore time: Invalid argument\n"));
        assert!(only_time_errors("a: Can't restore time: Invalid argument\ntar.exe: Error exit delayed from previous errors.\n"));
        assert!(!only_time_errors("a: Can't restore time: Invalid argument\ntar: b: Cannot open: Permission denied\n"));
        assert!(!only_time_errors("tar.exe: Error exit delayed from previous errors.\n"));
        assert!(!only_time_errors("fefw-recomp-0.1.7/a: Cannot open: No space left on device"));
        assert!(!only_time_errors(""));
    }

    #[test]
    fn versions() {
        assert_eq!(parse_version("v0.1.2"), Some((0, 1, 2)));
        assert_eq!(parse_version("1.10"), Some((1, 10, 0)));
        assert_eq!(parse_version("0.2.0-rc1"), Some((0, 2, 0)));
        assert!(parse_version("0.1.10") > parse_version("0.1.9"));
        assert_eq!(parse_version("dev"), None);
    }

    fn tmp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("fefw-upd-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }
    fn put(p: PathBuf, s: &str) {
        fs::create_dir_all(p.parent().unwrap()).unwrap();
        fs::write(p, s).unwrap();
    }
    fn get(p: PathBuf) -> String {
        fs::read_to_string(p).unwrap_or_else(|_| "<none>".into())
    }

    #[test]
    fn install_replaces_package_files_only() {
        let (inst, top, old) = (tmp("i1"), tmp("t1"), tmp("o1").join("old"));
        put(inst.join("fefw_rt.dll"), "old dll");
        put(inst.join("tools/a.py"), "old a");
        put(inst.join("tools/gone.py"), "stale");
        put(inst.join("fefw.ini"), "player ini");
        put(inst.join("save/x"), "player save");
        put(top.join("fefw_rt.dll"), "new dll");
        put(top.join("tools/a.py"), "new a");
        put(top.join("tools/sub/b.py"), "new b");
        install(&inst, &top, &old).unwrap();
        assert_eq!(get(inst.join("fefw_rt.dll")), "new dll");
        assert_eq!(get(inst.join("tools/a.py")), "new a");
        assert_eq!(get(inst.join("tools/sub/b.py")), "new b");
        assert!(!inst.join("tools/gone.py").exists());
        assert_eq!(get(inst.join("fefw.ini")), "player ini");
        assert_eq!(get(inst.join("save/x")), "player save");
        assert_eq!(get(old.join("fefw_rt.dll")), "old dll");
        assert_eq!(get(old.join("tools/gone.py")), "stale");
    }

    #[test]
    fn install_adds_language_tables_without_replacing_the_players_own() {
        let (inst, top, old) = (tmp("i3"), tmp("t3"), tmp("o3").join("old"));
        put(inst.join("locales/zh.ini"), "the player's own zh");
        put(inst.join("locales/mine.ini"), "a table the player wrote");
        put(top.join("locales/zh.ini"), "the shipped zh");
        put(top.join("fefw_rt.dll"), "new dll");
        install(&inst, &top, &old).unwrap();
        // the shipped table does not overwrite the copy already there
        assert_eq!(get(inst.join("locales/zh.ini")), "the player's own zh");
        // and a table only the player has is not treated as stale
        assert_eq!(get(inst.join("locales/mine.ini")), "a table the player wrote");
        assert_eq!(get(inst.join("fefw_rt.dll")), "new dll");

        // with no copy there yet, the shipped table is put in place
        let (inst2, top2, old2) = (tmp("i4"), tmp("t4"), tmp("o4").join("old"));
        put(top2.join("locales/zh.ini"), "the shipped zh");
        put(top2.join("fefw_rt.dll"), "new dll");
        install(&inst2, &top2, &old2).unwrap();
        assert_eq!(get(inst2.join("locales/zh.ini")), "the shipped zh");
    }

    #[test]
    fn install_rolls_back_on_failure() {
        let (inst, top, old) = (tmp("i2"), tmp("t2"), tmp("o2").join("old"));
        put(inst.join("fefw_rt.dll"), "old dll");
        put(inst.join("tools/a.py"), "old a");
        put(inst.join("tools/gone.py"), "stale");
        put(inst.join("zz"), "a file where the new package has a folder");
        put(top.join("fefw_rt.dll"), "new dll");
        put(top.join("tools/a.py"), "new a");
        put(top.join("zz/inner"), "cannot go under a file");
        // zz is a file in the install but a folder in the package: zz/inner cannot be created, after the other files moved
        assert!(install(&inst, &top, &old).is_err());
        assert_eq!(get(inst.join("fefw_rt.dll")), "old dll");
        assert_eq!(get(inst.join("tools/a.py")), "old a");
        assert_eq!(get(inst.join("tools/gone.py")), "stale");
        assert_eq!(get(inst.join("zz")), "a file where the new package has a folder");
    }

    /// End to end against GitHub: FEFW_UPDATE_TEST_DIR = an unpacked older package (fefw-recomp-<old>/ contents).
    /// cargo test --release -- --ignored update_end_to_end --nocapture
    #[test]
    #[ignore]
    fn update_end_to_end() {
        let install = PathBuf::from(std::env::var("FEFW_UPDATE_TEST_DIR").expect("FEFW_UPDATE_TEST_DIR"));
        let rel = latest().expect("latest");
        println!("latest {} {} {} sha256 {}", rel.version, rel.asset_name, rel.size, rel.sha256);
        let shared = Shared::default();
        let emit = |s: Status| {
            if s.phase != "download" || s.done % (16 << 20) < 200_000 {
                println!("{} {} {}/{} {}", s.state, s.phase, s.done, s.total, s.message)
            }
        };
        let run = Run { shared: &shared, install: install.clone(), game_dir: install.clone(), emit: &emit };
        let st = run.run(&rel).expect("update");
        println!("done: {} rebuild {:?} ({})", st.message, st.rebuild, st.rebuild_reason);
        assert!(install.join("fefw_rt.dll").is_file() && install.join("tools/build/fefw_build.py").is_file());
        assert!(install.join("update/old/fefw_rt.dll").is_file());
        cleanup(&install);
        assert!(!install.join("update/old").exists());
    }
}
