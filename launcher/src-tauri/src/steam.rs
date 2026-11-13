// SPDX-License-Identifier: GPL-3.0-or-later
//! Linux: adds fefw.exe to Steam as a non-Steam game (userdata/<account>/config/shortcuts.vdf), so it can be started from Steam and
//! from the Steam Deck's Game Mode. Steam gives an added .exe Proton by itself, so only the shortcut is written.
//!
//! Steam keeps shortcuts.vdf in memory and writes it back when it exits, so the file is only changed while Steam is closed: a
//! running Steam is asked to shut down first (and started again afterwards with the same command line). The file is binary
//! KeyValues; it is only rewritten when parsing and writing it back gives the same bytes, and the file as it was before the
//! launcher first changed it is kept as shortcuts.vdf.fefw-backup.

use serde::Serialize;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

pub const APP_NAME: &str = "Fire Emblem: Fortune's Weave (recomp)";

// ----- binary KeyValues (shortcuts.vdf) -----

#[derive(Clone, Debug, PartialEq)]
pub enum Node {
    Map(Vec<(Vec<u8>, Node)>),
    Str(Vec<u8>),
    Int(u32),
    Float([u8; 4]),
    U64(u64),
}

fn cstr<'a>(d: &'a [u8], i: &mut usize) -> Result<&'a [u8], String> {
    let end = d[*i..].iter().position(|&b| b == 0).ok_or("shortcuts.vdf: unterminated string")? + *i;
    let s = &d[*i..end];
    *i = end + 1;
    Ok(s)
}

fn take<const N: usize>(d: &[u8], i: &mut usize) -> Result<[u8; N], String> {
    let b: [u8; N] = d.get(*i..*i + N).ok_or("shortcuts.vdf: cut short")?.try_into().unwrap();
    *i += N;
    Ok(b)
}

/// The entries of one map, up to and including its 0x08 end byte.
fn parse_map(d: &[u8], i: &mut usize) -> Result<Vec<(Vec<u8>, Node)>, String> {
    let mut out = Vec::new();
    loop {
        let t = *d.get(*i).ok_or("shortcuts.vdf: cut short")?;
        *i += 1;
        if t == 0x08 {
            return Ok(out);
        }
        let key = cstr(d, i)?.to_vec();
        let v = match t {
            0x00 => Node::Map(parse_map(d, i)?),
            0x01 => Node::Str(cstr(d, i)?.to_vec()),
            0x02 => Node::Int(u32::from_le_bytes(take::<4>(d, i)?)),
            0x03 => Node::Float(take::<4>(d, i)?),
            0x07 => Node::U64(u64::from_le_bytes(take::<8>(d, i)?)),
            other => return Err(format!("shortcuts.vdf: unknown field type 0x{other:02x}")),
        };
        out.push((key, v));
    }
}

pub fn parse(d: &[u8]) -> Result<Vec<(Vec<u8>, Node)>, String> {
    let mut i = 0;
    let top = parse_map(d, &mut i)?;
    if i != d.len() {
        return Err("shortcuts.vdf: data after the end".into());
    }
    Ok(top)
}

fn write_map(out: &mut Vec<u8>, m: &[(Vec<u8>, Node)]) {
    for (k, v) in m {
        let t = match v {
            Node::Map(_) => 0x00,
            Node::Str(_) => 0x01,
            Node::Int(_) => 0x02,
            Node::Float(_) => 0x03,
            Node::U64(_) => 0x07,
        };
        out.push(t);
        out.extend_from_slice(k);
        out.push(0);
        match v {
            Node::Map(m) => write_map(out, m),
            Node::Str(s) => {
                out.extend_from_slice(s);
                out.push(0);
            }
            Node::Int(n) => out.extend_from_slice(&n.to_le_bytes()),
            Node::Float(b) => out.extend_from_slice(b),
            Node::U64(n) => out.extend_from_slice(&n.to_le_bytes()),
        }
    }
    out.push(0x08);
}

pub fn write(top: &[(Vec<u8>, Node)]) -> Vec<u8> {
    let mut out = Vec::new();
    write_map(&mut out, top);
    out
}

fn get<'a>(m: &'a [(Vec<u8>, Node)], key: &str) -> Option<&'a Node> {
    m.iter().find(|(k, _)| k.eq_ignore_ascii_case(key.as_bytes())).map(|(_, v)| v)
}

fn set(m: &mut Vec<(Vec<u8>, Node)>, key: &str, v: Node) {
    match m.iter_mut().find(|(k, _)| k.eq_ignore_ascii_case(key.as_bytes())) {
        Some(e) => e.1 = v,
        None => m.push((key.as_bytes().to_vec(), v)),
    }
}

fn str_of(m: &[(Vec<u8>, Node)], key: &str) -> String {
    match get(m, key) {
        Some(Node::Str(s)) => String::from_utf8_lossy(s).into_owned(),
        _ => String::new(),
    }
}

fn crc32(data: &[u8]) -> u32 {
    let mut c = !0u32;
    for &b in data {
        c ^= b as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { (c >> 1) ^ 0xEDB8_8320 } else { c >> 1 };
        }
    }
    !c
}

/// The id Steam ROM Manager and others give a shortcut: crc32 of the quoted exe and the name, with the top bit set. Steam keys
/// the shortcut's artwork (config/grid/<id>p.png ...) and its Proton choice on it.
pub fn shortcut_id(exe_quoted: &str, name: &str) -> u32 {
    crc32(format!("{exe_quoted}{name}").as_bytes()) | 0x8000_0000
}

fn quoted(p: &Path) -> String {
    format!("\"{}\"", p.display())
}

/// Whether a shortcut entry starts this fefw.exe (the one the launcher added, or one the player added by hand).
fn is_ours(e: &[(Vec<u8>, Node)], exe: &Path) -> bool {
    str_of(e, "Exe").trim().trim_matches('"') == exe.to_string_lossy()
}

const DIR_VAR: &str = "NXRT_LAUNCHER_DIR=\"";

/// A shortcut's launch options with this launcher's NXRT_LAUNCHER_DIR="..." (`dir`, or none) in place of the one it had; whatever
/// else the player put there is kept.
pub fn launch_options(existing: &str, dir: Option<&str>) -> String {
    let mut rest = existing.to_string();
    if let Some(i) = rest.find(DIR_VAR) {
        let end = rest[i + DIR_VAR.len()..].find('"').map_or(rest.len(), |j| i + DIR_VAR.len() + j + 1);
        rest.replace_range(i..end, "");
    }
    let rest = rest.split_whitespace().collect::<Vec<_>>().join(" ");
    match dir {
        None if rest == "%command%" => String::new(),
        None => rest,
        Some(d) if rest.is_empty() => format!("{DIR_VAR}{d}\" %command%"),
        Some(d) if rest.contains("%command%") => format!("{DIR_VAR}{d}\" {rest}"),
        Some(d) => format!("{DIR_VAR}{d}\" %command% {rest}"),
    }
}

/// Adds or updates the fefw.exe shortcut in a parsed shortcuts.vdf. Returns whether it was there already. `launcher_dir`: the
/// NXRT_LAUNCHER_DIR the game needs (None when it finds the upscalers next to itself).
pub fn upsert(top: &mut Vec<(Vec<u8>, Node)>, exe: &Path, start_dir: &Path, launcher_dir: Option<&str>) -> Result<bool, String> {
    if top.is_empty() {
        top.push((b"shortcuts".to_vec(), Node::Map(vec![])));
    }
    let Some(Node::Map(list)) = top.iter_mut().find(|(k, _)| k.eq_ignore_ascii_case(b"shortcuts")).map(|(_, v)| v) else {
        return Err("shortcuts.vdf has no shortcuts list".into());
    };
    let exe_q = quoted(exe);
    if let Some((_, Node::Map(e))) = list.iter_mut().find(|(_, v)| matches!(v, Node::Map(e) if is_ours(e, exe))) {
        // keep the player's name, artwork id and options; only make sure it still starts in the run folder
        set(e, "Exe", Node::Str(exe_q.into_bytes()));
        set(e, "StartDir", Node::Str(quoted(start_dir).into_bytes()));
        let opts = launch_options(&str_of(e, "LaunchOptions"), launcher_dir);
        set(e, "LaunchOptions", Node::Str(opts.into_bytes()));
        return Ok(true);
    }
    let index = (0..).map(|n: usize| n.to_string()).find(|n| !list.iter().any(|(k, _)| k == n.as_bytes())).unwrap();
    let s = |v: &str| Node::Str(v.as_bytes().to_vec());
    let entry = vec![
        (b"appid".to_vec(), Node::Int(shortcut_id(&exe_q, APP_NAME))),
        (b"AppName".to_vec(), s(APP_NAME)),
        (b"Exe".to_vec(), s(&exe_q)),
        (b"StartDir".to_vec(), s(&quoted(start_dir))),
        (b"icon".to_vec(), s("")),
        (b"ShortcutPath".to_vec(), s("")),
        (b"LaunchOptions".to_vec(), s(&launch_options("", launcher_dir))),
        (b"IsHidden".to_vec(), Node::Int(0)),
        (b"AllowDesktopConfig".to_vec(), Node::Int(1)),
        (b"AllowOverlay".to_vec(), Node::Int(1)),
        (b"OpenVR".to_vec(), Node::Int(0)),
        (b"Devkit".to_vec(), Node::Int(0)),
        (b"DevkitGameID".to_vec(), s("")),
        (b"DevkitOverrideAppID".to_vec(), Node::Int(0)),
        (b"LastPlayTime".to_vec(), Node::Int(0)),
        (b"FlatpakAppID".to_vec(), s("")),
        (b"tags".to_vec(), Node::Map(vec![])),
    ];
    list.push((index.into_bytes(), Node::Map(entry)));
    Ok(false)
}

// ----- the Steam install -----

struct Install {
    root: PathBuf,
    flatpak: bool,
}

fn home() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_default()
}

fn find_install() -> Option<Install> {
    let h = home();
    let native = [h.join(".steam").join("steam"), h.join(".local").join("share").join("Steam")];
    for r in native {
        if r.join("userdata").is_dir() {
            return Some(Install { root: fs::canonicalize(&r).unwrap_or(r), flatpak: false });
        }
    }
    let fp = h.join(".var/app/com.valvesoftware.Steam/.local/share/Steam");
    fp.join("userdata").is_dir().then_some(Install { root: fp, flatpak: true })
}

/// The account folders under userdata/ (numeric, not 0).
fn accounts(root: &Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = fs::read_dir(root.join("userdata"))
        .into_iter()
        .flatten()
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.is_dir() && p.file_name().and_then(|n| n.to_str()).is_some_and(|n| n != "0" && n.bytes().all(|b| b.is_ascii_digit())))
        .collect();
    v.sort();
    v
}

#[cfg(not(unix))]
fn running() -> Option<(u32, Vec<String>, Vec<(String, String)>)> {
    None
}

/// This user's running Steam client (another user's is not ours to close): its pid, command line and environment (to start it
/// again exactly as it was).
#[cfg(unix)]
fn running() -> Option<(u32, Vec<String>, Vec<(String, String)>)> {
    use std::os::unix::fs::MetadataExt;
    let me = fs::metadata("/proc/self").ok()?.uid();
    for e in fs::read_dir("/proc").ok()?.flatten() {
        let Some(pid) = e.file_name().to_str().and_then(|s| s.parse::<u32>().ok()) else { continue };
        if fs::metadata(e.path()).map_or(true, |m| m.uid() != me) {
            continue;
        }
        if fs::read_to_string(e.path().join("comm")).map(|c| c.trim() == "steam").unwrap_or(false) {
            let cmd = fs::read(e.path().join("cmdline")).unwrap_or_default();
            let args = cmd.split(|&b| b == 0).filter(|a| !a.is_empty()).map(|a| String::from_utf8_lossy(a).into_owned()).collect();
            let env = fs::read(e.path().join("environ")).unwrap_or_default();
            let env = env
                .split(|&b| b == 0)
                .filter_map(|kv| {
                    let kv = String::from_utf8_lossy(kv);
                    kv.split_once('=').filter(|(k, _)| !k.is_empty()).map(|(k, v)| (k.to_string(), v.to_string()))
                })
                .collect();
            return Some((pid, args, env));
        }
    }
    None
}

fn steam_command(flatpak: bool) -> Command {
    let mut c = if flatpak {
        let mut c = Command::new("flatpak");
        c.args(["run", "com.valvesoftware.Steam"]);
        c
    } else {
        Command::new("steam")
    };
    crate::setup::host_env(&mut c);
    c
}

#[derive(Serialize, Clone)]
pub struct State {
    /// Steam is installed for this user (Linux only)
    pub available: bool,
    pub running: bool,
    /// a shortcut to this fefw.exe exists already (as saved on disk)
    pub added: bool,
}

pub fn state(exe: &Path) -> State {
    let Some(inst) = find_install().filter(|_| cfg!(target_os = "linux")) else {
        return State { available: false, running: false, added: false };
    };
    let added = accounts(&inst.root).iter().any(|a| {
        fs::read(a.join("config").join("shortcuts.vdf")).ok().and_then(|d| parse(&d).ok()).is_some_and(|top| {
            matches!(get(&top, "shortcuts"), Some(Node::Map(l)) if l.iter().any(|(_, v)| matches!(v, Node::Map(e) if is_ours(e, exe))))
        })
    });
    State { available: true, running: running().is_some(), added }
}

fn update_file(path: &Path, exe: &Path, start_dir: &Path, launcher_dir: Option<&str>) -> Result<bool, String> {
    let old = fs::read(path).ok();
    let mut top = match &old {
        Some(d) => {
            let top = parse(d)?;
            if write(&top) != *d {
                return Err(format!("{} has a layout the launcher does not know; it was left as it is.", path.display()));
            }
            top
        }
        None => vec![],
    };
    let existed = upsert(&mut top, exe, start_dir, launcher_dir)?;
    // the file as it was before the launcher first changed it: a later Add to Steam does not replace it
    let backup = path.with_extension("vdf.fefw-backup");
    if let (Some(d), false) = (&old, backup.exists()) {
        fs::write(&backup, d).map_err(|e| format!("cannot back up {}: {e}", path.display()))?;
    }
    let tmp = path.with_extension("vdf.tmp");
    fs::create_dir_all(path.parent().unwrap()).map_err(|e| e.to_string())?;
    fs::write(&tmp, write(&top)).map_err(|e| format!("cannot write {}: {e}", tmp.display()))?;
    fs::rename(&tmp, path).map_err(|e| format!("cannot replace {}: {e}", path.display()))?;
    Ok(existed)
}

/// Adds fefw.exe to every Steam account on this PC. `close_steam`: a running Steam may be shut down (and is started again).
/// `reads`: the folders the game reads (the run folder, the dump, the package), named for Flatpak Steam's sandbox.
pub fn add(exe: &Path, start_dir: &Path, launcher_dir: Option<&str>, reads: &[PathBuf], close_steam: bool) -> Result<String, String> {
    if !cfg!(target_os = "linux") {
        return Err("Add to Steam is for Linux, where Steam runs fefw.exe with Proton.".into());
    }
    // Steam keeps these quoted ("...") in the file and in the launch options, with no escaping
    if [exe.to_string_lossy(), start_dir.to_string_lossy()].iter().chain(launcher_dir.map(Into::into).iter()).any(|p| p.contains('"')) {
        return Err("The game's folder has a \" in its path, which a Steam shortcut cannot hold. Move it to a folder without one.".into());
    }
    let inst = find_install().ok_or("Steam was not found (no ~/.steam/steam or ~/.local/share/Steam).")?;
    let accts = accounts(&inst.root);
    if accts.is_empty() {
        return Err("Steam has no signed-in account on this PC yet. Start Steam and sign in once.".into());
    }
    let mut restart = None;
    if let Some((pid, args, env)) = running() {
        if !close_steam {
            return Err("Steam is running.".into());
        }
        let _ = steam_command(inst.flatpak).arg("-shutdown").stdout(Stdio::null()).stderr(Stdio::null()).status();
        let t = Instant::now();
        while Path::new(&format!("/proc/{pid}")).exists() {
            if t.elapsed() > Duration::from_secs(45) {
                return Err("Steam did not close. Quit it from its menu and press Add to Steam again.".into());
            }
            std::thread::sleep(Duration::from_millis(300));
        }
        // the client's own flags (e.g. -steamdeck), not the ones its wrapper adds for itself
        restart = Some((args.into_iter().skip(1).filter(|a| a != "-srt-logger-opened").collect::<Vec<_>>(), env));
    }
    // every account, also after one fails, so one odd file does not keep the game from the others
    let (mut updated, mut added, mut failed) = (0, 0, vec![]);
    for a in &accts {
        match update_file(&a.join("config").join("shortcuts.vdf"), exe, start_dir, launcher_dir) {
            Ok(true) => updated += 1,
            Ok(false) => added += 1,
            Err(e) => failed.push(e),
        }
    }
    if let Some((args, env)) = restart {
        let mut c = steam_command(inst.flatpak);
        // the environment it had (its session's), not the launcher's: an AppImage's would point into a mount that goes away
        // (cleaned too: a Steam an older launcher restarted from its AppImage carries that one's mount). Not for Flatpak Steam:
        // the client found in /proc runs inside the sandbox, and its environment does not fit `flatpak run` on the host.
        if !env.is_empty() && !inst.flatpak {
            let fixes = crate::setup::appimage_fixes(&env);
            let env = env.into_iter().filter_map(|(k, v)| match fixes.iter().find(|(f, _)| *f == k) {
                Some((_, new)) => new.clone().map(|n| (k, n)),
                None => Some((k, v)),
            });
            c.env_clear().envs(env);
        }
        c.args(args).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());
        #[cfg(unix)]
        {
            use std::os::unix::process::CommandExt;
            c.process_group(0); // outlives the launcher
        }
        let _ = c.spawn();
    }
    if !failed.is_empty() {
        let done = updated + added;
        let head = if done == 0 { "Not added to Steam".to_string() } else { format!("Added for {done} of {} Steam accounts", accts.len()) };
        return Err(format!("{head}: {}", failed.join(" ")));
    }
    let mut msg = if added == 0 {
        "fefw.exe was already in Steam; its shortcut now starts in the run folder.".to_string()
    } else {
        format!("Added to Steam as \"{APP_NAME}\". Steam runs it with Proton.")
    };
    if inst.flatpak {
        // the Flatpak's sandbox sees only its own folders unless it is given more: every folder the game reads, once each
        let mut dirs: Vec<&Path> = vec![];
        for d in reads.iter().map(PathBuf::as_path).chain([start_dir]) {
            if !dirs.iter().any(|k| d.starts_with(k)) {
                dirs.retain(|k| !k.starts_with(d));
                dirs.push(d);
            }
        }
        let args: Vec<String> = dirs.iter().map(|d| format!("--filesystem=\"{}\"", d.display())).collect();
        msg += &format!(" Flatpak Steam cannot open these folders by itself: run flatpak override --user {} com.valvesoftware.Steam", args.join(" "));
    }
    // Proton runs in the Steam Linux Runtime's container, which sees only the home folder, /mnt, /media and /run/media (#142)
    let all: Vec<PathBuf> = reads.iter().cloned().chain([start_dir.to_path_buf()]).collect();
    let home = std::env::var_os("HOME").map(PathBuf::from);
    if let Some(m) = crate::play::container_mounts(&all, home.as_deref()) {
        msg += &format!(" Steam's Proton cannot see {m} by itself: in the game's Properties in Steam, put STEAM_COMPAT_MOUNTS={m} in front of %command% in the launch options, or move those folders into your home folder and run Set up again.");
    }
    Ok(msg)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// FEFW_TEST_SHORTCUTS=<a real shortcuts.vdf> cargo test -- --ignored: parses it and writes the same bytes back (read-only)
    #[test]
    #[ignore]
    fn a_real_shortcuts_file_round_trips() {
        let p = std::env::var("FEFW_TEST_SHORTCUTS").expect("set FEFW_TEST_SHORTCUTS");
        let d = fs::read(p).unwrap();
        assert_eq!(write(&parse(&d).unwrap()), d);
    }

    #[test]
    #[cfg(target_os = "linux")]
    fn appimage_variables_are_dropped() {
        let v = |k: &str, x: &str| (k.to_string(), x.to_string());
        let env = [
            v("APPDIR", "/tmp/.mount_x"),
            v("APPIMAGE", "/home/deck/fefw/fefw-launcher.AppImage"),
            v("PYTHONHOME", "/tmp/.mount_x/usr/"),
            v("XDG_DATA_DIRS", "/tmp/.mount_x/usr/share/:/usr/share:/usr/local/share"),
            v("GTK_PATH", "/tmp/.mount_x//usr/lib/gtk-3.0"),
            v("HOME", "/home/deck"),
        ];
        let f = crate::setup::appimage_fixes(&env);
        let get = |k: &str| f.iter().find(|(n, _)| n == k).map(|(_, x)| x.clone());
        assert_eq!(get("APPDIR"), Some(None));
        assert_eq!(get("PYTHONHOME"), Some(None));
        assert_eq!(get("GTK_PATH"), Some(None));
        assert_eq!(get("XDG_DATA_DIRS"), Some(Some("/usr/share:/usr/local/share".into())));
        assert_eq!(get("HOME"), None);
        assert!(crate::setup::appimage_fixes(&env[1..]).is_empty());
        assert!(crate::setup::appimage_fixes(&[v("APPDIR", "C:\\Apps"), v("PATH", "C:\\Apps\\bin;C:\\Windows")]).is_empty());
    }

    #[test]
    fn launch_options_keep_the_players_own() {
        assert_eq!(launch_options("", None), "");
        assert_eq!(launch_options("", Some("Z:/a")), "NXRT_LAUNCHER_DIR=\"Z:/a\" %command%");
        assert_eq!(launch_options("NXRT_LAUNCHER_DIR=\"Z:/old\" %command%", Some("Z:/new")), "NXRT_LAUNCHER_DIR=\"Z:/new\" %command%");
        assert_eq!(launch_options("PROTON_LOG=1 NXRT_LAUNCHER_DIR=\"Z:/old\" %command%", None), "PROTON_LOG=1 %command%");
        assert_eq!(launch_options("NXRT_LAUNCHER_DIR=\"Z:/old\" %command%", None), "");
        assert_eq!(launch_options("-fullscreen", Some("Z:/a")), "NXRT_LAUNCHER_DIR=\"Z:/a\" %command% -fullscreen");
    }

    #[test]
    fn known_crc() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn add_then_find_and_round_trip() {
        let exe = Path::new("/home/deck/Games/fefw/fefw.exe");
        let mut top = vec![];
        assert!(!upsert(&mut top, exe, Path::new("/home/deck/Games/fefw"), None).unwrap());
        let bytes = write(&top);
        assert_eq!(&bytes[..14], b"\x00shortcuts\x00\x000\x00");
        let mut back = parse(&bytes).unwrap();
        assert_eq!(write(&back), bytes);
        // a second add updates the same entry, no duplicate
        assert!(upsert(&mut back, exe, Path::new("/elsewhere"), Some("Z:/l")).unwrap());
        let Some(Node::Map(l)) = get(&back, "shortcuts") else { panic!() };
        assert_eq!(l.len(), 1);
        let Node::Map(e) = &l[0].1 else { panic!() };
        assert_eq!(str_of(e, "StartDir"), "\"/elsewhere\"");
        assert_eq!(str_of(e, "LaunchOptions"), "NXRT_LAUNCHER_DIR=\"Z:/l\" %command%");
        assert!(matches!(get(e, "appid"), Some(Node::Int(id)) if id & 0x8000_0000 != 0));
    }

    #[test]
    fn a_hand_added_entry_is_reused() {
        let exe = Path::new("/g/fefw.exe");
        let mine = vec![(b"appid".to_vec(), Node::Int(123)), (b"appname".to_vec(), Node::Str(b"FE".to_vec())), (b"Exe".to_vec(), Node::Str(b"\"/g/fefw.exe\"".to_vec()))];
        let mut top = vec![(b"shortcuts".to_vec(), Node::Map(vec![(b"0".to_vec(), Node::Map(mine))]))];
        assert!(upsert(&mut top, exe, Path::new("/g"), None).unwrap());
        let Some(Node::Map(l)) = get(&top, "shortcuts") else { panic!() };
        let Node::Map(e) = &l[0].1 else { panic!() };
        assert_eq!(str_of(e, "appname"), "FE");
        assert!(matches!(get(e, "appid"), Some(Node::Int(123))));
    }
}
