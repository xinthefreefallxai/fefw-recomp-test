// SPDX-License-Identifier: GPL-3.0-or-later
//! Checks the player's own decrypted dump. Read-only: nothing here writes to the dump.
//!
//! Accepted layouts: a folder with `exefs/` and `romfs/` (one title version, e.g. the update), or a dump root with `base/` and
//! `updates/<version>/`, each holding `exefs/` and `romfs/`. The newest complete candidate whose `main` is a supported build is used.
//! Add-on content (DLC) the player dumped sits in `dlc/<16-hex-digit title id>/romfs` (or `dlc/<id>/<n>/romfs`) of that folder. The
//! runtime is pointed at that `dlc/` folder ([aoc] dir) and serves only what is there.

use serde::Serialize;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};

pub const EXEFS_FILES: [&str; 6] = ["main", "sdk", "subsdk0", "subsdk1", "subsdk2", "rtld"];

#[derive(Serialize, Clone)]
pub struct Check {
    pub label: String,
    pub ok: bool,
}

#[derive(Serialize, Clone)]
pub struct Candidate {
    /// "folder", "base" or "update"
    pub kind: String,
    pub version: String,
    pub path: String,
    pub exefs: String,
    pub romfs: String,
    pub ok: bool,
    pub checks: Vec<Check>,
}

#[derive(Serialize, Clone)]
pub struct DumpReport {
    pub path: String,
    pub exists: bool,
    pub errors: Vec<String>,
    pub candidates: Vec<Candidate>,
    pub chosen: Option<Candidate>,
    /// the folder's `dlc/`, when it holds at least one add-on
    pub dlc_dir: Option<String>,
    /// title ids of the add-ons found there (upper-case hex), sorted
    pub dlc: Vec<String>,
}

pub fn fwd(p: &Path) -> String {
    p.to_string_lossy().replace('\\', "/")
}

/// A path as ObeliskChamber.exe sees it. On Windows that is the path itself. On Linux ObeliskChamber.exe runs under Wine/Proton, where the host's `/`
/// is drive Z: (every Wine and Proton prefix maps it), so an absolute path becomes `Z:/...`; a relative one stays as it is.
pub fn game_path(p: &Path) -> String {
    let s = fwd(p);
    if cfg!(windows) || !s.starts_with('/') {
        s
    } else {
        format!("Z:{s}")
    }
}

/// The builds of main the recomp supports: `BUILDS = { '<40 hex>': '<version>', ... }` in recomp/games/fefw/config.py.
pub fn supported_builds(repo: &Path) -> Vec<(String, String)> {
    let text = fs::read_to_string(repo.join("recomp").join("games").join("fefw").join("config.py")).unwrap_or_default();
    let Some(start) = text.find("BUILDS") else { return vec![] };
    let body = &text[start..];
    let body = &body[..body.find('}').unwrap_or(body.len())];
    let mut out = Vec::new();
    for line in body.lines() {
        let q: Vec<&str> = line.split('\'').collect();
        if q.len() >= 4 && q[1].len() == 40 && q[1].chars().all(|c| c.is_ascii_hexdigit()) {
            out.push((q[1].to_uppercase(), q[3].to_string()));
        }
    }
    out
}

/// Build id of an NSO (20 bytes at 0x40), upper-case hex; None if the file is missing, short or not a decrypted NSO.
fn build_id(main: &Path) -> Option<String> {
    let mut h = [0u8; 0x54];
    fs::File::open(main).ok()?.read_exact(&mut h).ok()?;
    if &h[..4] != b"NSO0" {
        return None;
    }
    Some(h[0x40..0x54].iter().map(|b| format!("{:02X}", b)).collect())
}

fn check_candidate(kind: &str, version: &str, dir: &Path, builds: &[(String, String)]) -> Candidate {
    let exefs = dir.join("exefs");
    let romfs = dir.join("romfs");
    let mut checks: Vec<Check> = EXEFS_FILES.iter().map(|f| Check { label: format!("exefs/{f}"), ok: exefs.join(f).is_file() }).collect();
    checks.push(Check { label: "romfs/".into(), ok: romfs.is_dir() });
    if exefs.join("main").is_file() {
        let id = build_id(&exefs.join("main"));
        checks.push(Check { label: "exefs/main is decrypted".into(), ok: id.is_some() });
        if let Some(id) = id {
            if !builds.is_empty() {
                let want = builds.iter().map(|(_, v)| v.as_str()).collect::<Vec<_>>().join(" or ");
                checks.push(Check { label: format!("supported version ({want})"), ok: builds.iter().any(|(b, _)| *b == id) });
            }
        }
    }
    let ok = checks.iter().all(|c| c.ok);
    Candidate { kind: kind.into(), version: version.into(), path: fwd(dir), exefs: fwd(&exefs), romfs: fwd(&romfs), ok, checks }
}

fn is_title_id(s: &str) -> bool {
    s.len() == 16 && s.chars().all(|c| c.is_ascii_hexdigit())
}

fn has_romfs(addon: &Path) -> bool {
    if addon.join("romfs").is_dir() {
        return true;
    }
    // one level down, a numbered folder: dlc/<id>/<n>/romfs
    fs::read_dir(addon).map_or(false, |rd| {
        rd.filter_map(|e| e.ok()).any(|e| {
            let n = e.file_name().to_string_lossy().to_string();
            !n.is_empty() && n.chars().all(|c| c.is_ascii_digit()) && e.path().join("romfs").is_dir()
        })
    })
}

/// The add-ons in `dlc`: folders named by a title id that hold a `romfs/`. Upper-case ids, sorted.
pub fn scan_dlc(dlc: &Path) -> Vec<String> {
    let mut out: Vec<String> = fs::read_dir(dlc)
        .map(|rd| {
            rd.filter_map(|e| e.ok())
                .filter(|e| e.path().is_dir())
                .filter_map(|e| {
                    let name = e.file_name().to_string_lossy().to_string();
                    (is_title_id(&name) && has_romfs(&e.path())).then(|| name.to_uppercase())
                })
                .collect()
        })
        .unwrap_or_default();
    out.sort();
    out
}

fn version_order(a: &str, b: &str) -> std::cmp::Ordering {
    match (a.parse::<u64>(), b.parse::<u64>()) {
        (Ok(x), Ok(y)) => y.cmp(&x),
        (Ok(_), Err(_)) => std::cmp::Ordering::Less,
        (Err(_), Ok(_)) => std::cmp::Ordering::Greater,
        (Err(_), Err(_)) => b.cmp(a),
    }
}

pub fn scan(path: &str, builds: &[(String, String)]) -> DumpReport {
    let root = PathBuf::from(path.trim());
    let mut report =
        DumpReport { path: fwd(&root), exists: root.is_dir(), errors: vec![], candidates: vec![], chosen: None, dlc_dir: None, dlc: vec![] };
    if path.trim().is_empty() {
        report.errors.push("No folder chosen yet.".into());
        return report;
    }
    if !report.exists {
        report.errors.push("That folder does not exist or cannot be read.".into());
        return report;
    }
    if root.join("exefs").is_dir() {
        report.candidates.push(check_candidate("folder", "", &root, builds));
    }
    let updates = root.join("updates");
    if updates.is_dir() {
        let mut versions: Vec<String> = fs::read_dir(&updates)
            .map(|rd| rd.filter_map(|e| e.ok()).filter(|e| e.path().is_dir()).map(|e| e.file_name().to_string_lossy().to_string()).collect())
            .unwrap_or_default();
        versions.sort_by(|a, b| version_order(a, b));
        for v in versions {
            report.candidates.push(check_candidate("update", &v, &updates.join(&v), builds));
        }
    }
    if root.join("base").is_dir() {
        report.candidates.push(check_candidate("base", "", &root.join("base"), builds));
    }
    if report.candidates.is_empty() {
        report.errors.push("This folder has no exefs/ folder, and no base/ or updates/ folder holding one. Pick the folder of your dump that contains exefs/ and romfs/.".into());
        return report;
    }
    report.chosen = report.candidates.iter().find(|c| c.ok).cloned();
    let dlc = root.join("dlc");
    report.dlc = scan_dlc(&dlc);
    if !report.dlc.is_empty() {
        report.dlc_dir = Some(fwd(&dlc));
    }
    if report.chosen.is_none() {
        let wrong = report.candidates.iter().any(|c| c.checks.iter().any(|k| k.label.starts_with("supported version") && !k.ok));
        report.errors.push(if wrong {
            "These game files are a different version from the one this recomp supports. Dump the supported update.".into()
        } else {
            "No usable game files found. The folder needs exefs/ (main, sdk, subsdk0, subsdk1, subsdk2, rtld, decrypted) and romfs/.".into()
        });
    }
    report
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn game_paths_for_wine() {
        assert_eq!(game_path(Path::new("save")), "save");
        if cfg!(windows) {
            assert_eq!(game_path(Path::new(r"C:\games\fefw\exefs")), "C:/games/fefw/exefs");
        } else {
            assert_eq!(game_path(Path::new("/home/deck/fefw/exefs")), "Z:/home/deck/fefw/exefs");
        }
    }

    #[test]
    fn build_id_of_an_nso_header() {
        let dir = std::env::temp_dir().join(format!("fefw_dump_test_{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let mut h = vec![0u8; 0x60];
        h[..4].copy_from_slice(b"NSO0");
        h[0x40] = 0xdb;
        h[0x53] = 0x0e;
        fs::write(dir.join("main"), &h).unwrap();
        assert_eq!(build_id(&dir.join("main")).unwrap(), format!("DB{}0E", "00".repeat(18)));
        h[0] = b'X';
        fs::write(dir.join("main"), &h).unwrap();
        assert_eq!(build_id(&dir.join("main")), None);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn dlc_folders_need_a_title_id_and_a_romfs() {
        let dir = std::env::temp_dir().join(format!("fefw_dlc_test_{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(dir.join("04007ef01f8af002").join("romfs")).unwrap();
        fs::create_dir_all(dir.join("04007EF01F8AF001").join("0").join("romfs")).unwrap();
        fs::create_dir_all(dir.join("04007EF01F8AF003")).unwrap();
        fs::create_dir_all(dir.join("notes").join("romfs")).unwrap();
        assert_eq!(scan_dlc(&dir), vec!["04007EF01F8AF001".to_string(), "04007EF01F8AF002".to_string()]);
        assert!(scan_dlc(&dir.join("missing")).is_empty());
        let _ = fs::remove_dir_all(&dir);
    }
}
