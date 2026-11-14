// SPDX-License-Identifier: GPL-3.0-or-later
//! A careful line editor for the runtime's ini files (ObeliskChamber.ini, controls.ini).
//!
//! It never rebuilds the file from a parsed model. Lines are kept as they are; `set` changes only the
//! value text of one `key = value` line (keeping the key spelling, the spacing and any trailing comment)
//! or inserts a new line. Comments, blank lines, unknown keys and the line-ending style survive.
//! Section and key names are case-insensitive, as in the runtime (runtime/docs/CONFIG.md).

pub struct Ini {
    lines: Vec<String>,
    eol: &'static str,
    final_eol: bool,
}

fn section_name(line: &str) -> Option<String> {
    let t = line.trim();
    if t.starts_with('[') {
        if let Some(end) = t.find(']') {
            return Some(t[1..end].trim().to_ascii_lowercase());
        }
    }
    None
}

/// Index of the `=` of a key line, or None for blanks, comments, sections and lines without `=`.
fn key_eq(line: &str) -> Option<usize> {
    let t = line.trim_start();
    if t.is_empty() || t.starts_with(';') || t.starts_with('#') || t.starts_with('[') {
        return None;
    }
    line.find('=')
}

/// Splits the text after `=` (leading blanks already removed) into (value, tail), where the tail is
/// the blanks and comment that follow the value.
fn split_value(rest: &str) -> (&str, &str) {
    if rest.starts_with('"') {
        if let Some(close) = rest[1..].find('"') {
            let end = close + 2;
            return (&rest[..end], &rest[end..]);
        }
    }
    let b = rest.as_bytes();
    for i in 1..b.len() {
        if (b[i] == b';' || b[i] == b'#') && (b[i - 1] == b' ' || b[i - 1] == b'\t') {
            let mut s = i;
            while s > 0 && (b[s - 1] == b' ' || b[s - 1] == b'\t') {
                s -= 1;
            }
            return (&rest[..s], &rest[s..]);
        }
    }
    let trimmed = rest.trim_end();
    (trimmed, &rest[trimmed.len()..])
}

fn unquote(v: &str) -> String {
    let v = v.trim();
    if v.len() >= 2 && v.starts_with('"') && v.ends_with('"') {
        v[1..v.len() - 1].to_string()
    } else {
        v.to_string()
    }
}

/// A value is written quoted when the runtime would otherwise misread it.
fn format_value(v: &str) -> String {
    let v: String = v.chars().filter(|c| *c != '\r' && *c != '\n').collect();
    let needs = v != v.trim()
        || v.contains(" ;")
        || v.contains(" #")
        || v.contains("\t;")
        || v.contains("\t#")
        || v.starts_with('"');
    if needs && !v.contains('"') {
        format!("\"{}\"", v)
    } else {
        v
    }
}

impl Ini {
    pub fn parse(text: &str) -> Ini {
        let eol = if text.contains("\r\n") { "\r\n" } else { "\n" };
        let final_eol = text.ends_with('\n');
        let lines = text.lines().map(|l| l.to_string()).collect();
        Ini { lines, eol, final_eol }
    }

    pub fn empty() -> Ini {
        Ini { lines: Vec::new(), eol: "\r\n", final_eol: true }
    }

    /// Line index of the last `key` in `section`, if any.
    fn find(&self, section: &str, key: &str) -> Option<usize> {
        let (section, key) = (section.to_ascii_lowercase(), key.to_ascii_lowercase());
        let mut cur = String::new();
        let mut hit = None;
        for (i, line) in self.lines.iter().enumerate() {
            if let Some(s) = section_name(line) {
                cur = s;
                continue;
            }
            if cur == section {
                if let Some(eq) = key_eq(line) {
                    if line[..eq].trim().to_ascii_lowercase() == key {
                        hit = Some(i);
                    }
                }
            }
        }
        hit
    }

    pub fn get(&self, section: &str, key: &str) -> Option<String> {
        let i = self.find(section, key)?;
        let line = &self.lines[i];
        let eq = key_eq(line)?;
        let rest = line[eq + 1..].trim_start();
        Some(unquote(split_value(rest).0))
    }

    /// Every `section.key` (lower case) with its value; a repeated key keeps the last value.
    pub fn all(&self) -> Vec<(String, String)> {
        let mut cur = String::new();
        let mut out: Vec<(String, String)> = Vec::new();
        for line in &self.lines {
            if let Some(s) = section_name(line) {
                cur = s;
                continue;
            }
            if let Some(eq) = key_eq(line) {
                if cur.is_empty() {
                    continue;
                }
                let k = format!("{}.{}", cur, line[..eq].trim().to_ascii_lowercase());
                let v = unquote(split_value(line[eq + 1..].trim_start()).0);
                if let Some(e) = out.iter_mut().find(|e| e.0 == k) {
                    e.1 = v;
                } else {
                    out.push((k, v));
                }
            }
        }
        out
    }

    pub fn set(&mut self, section: &str, key: &str, value: &str) {
        let value = format_value(value);
        if let Some(i) = self.find(section, key) {
            let line = &self.lines[i];
            let eq = key_eq(line).unwrap();
            let after = &line[eq + 1..];
            let blanks = after.len() - after.trim_start().len();
            let (_, tail) = split_value(&after[blanks..]);
            let new_line = format!("{}{}{}{}", &line[..=eq], &after[..blanks], value, tail);
            self.lines[i] = new_line;
            return;
        }
        let new_line = format!("{} = {}", key, value);
        // insert after the last non-blank line of the section, or start the section at the end
        let target = section.to_ascii_lowercase();
        let mut cur = String::new();
        let mut last_in_section: Option<usize> = None;
        let mut found_section = false;
        for (i, line) in self.lines.iter().enumerate() {
            if let Some(s) = section_name(line) {
                cur = s;
                if cur == target {
                    found_section = true;
                    last_in_section = Some(i);
                }
                continue;
            }
            if cur == target && !line.trim().is_empty() {
                last_in_section = Some(i);
            }
        }
        if found_section {
            self.lines.insert(last_in_section.unwrap() + 1, new_line);
        } else {
            if self.lines.last().map_or(false, |l| !l.trim().is_empty()) {
                self.lines.push(String::new());
            }
            self.lines.push(format!("[{}]", section));
            self.lines.push(new_line);
        }
    }

    pub fn to_text(&self) -> String {
        let mut s = self.lines.join(self.eol);
        if self.final_eol {
            s.push_str(self.eol);
        }
        s
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = "# header comment\r\n\r\n[display]\r\nwindow_mode = windowed ; keep\r\nvsync=1\r\nUnknownKey = 5\r\n\r\n[mouse]\r\nyaw = 0.5\r\n";

    #[test]
    fn edits_keep_everything_else() {
        let mut ini = Ini::parse(SAMPLE);
        assert_eq!(ini.get("DISPLAY", "Window_Mode").as_deref(), Some("windowed"));
        ini.set("display", "window_mode", "borderless");
        ini.set("display", "vsync", "0");
        let out = ini.to_text();
        assert!(out.contains("window_mode = borderless ; keep\r\n"));
        assert!(out.contains("vsync=0\r\n"));
        assert!(out.contains("UnknownKey = 5\r\n"));
        assert!(out.starts_with("# header comment\r\n"));
        assert!(out.ends_with("yaw = 0.5\r\n"));
    }

    #[test]
    fn inserts_keys_and_sections() {
        let mut ini = Ini::parse(SAMPLE);
        ini.set("display", "fps_limit", "auto");
        ini.set("audio", "volume", "0.5");
        let out = ini.to_text();
        assert!(out.contains("UnknownKey = 5\r\nfps_limit = auto\r\n\r\n[mouse]"));
        assert!(out.ends_with("[audio]\r\nvolume = 0.5\r\n"));
        assert_eq!(Ini::parse(&out).get("audio", "volume").as_deref(), Some("0.5"));
    }

    #[test]
    fn paths_with_comment_chars_are_quoted() {
        let mut ini = Ini::empty();
        ini.set("fs", "romfs", "C:/a b/x ; y");
        assert_eq!(ini.get("fs", "romfs").as_deref(), Some("C:/a b/x ; y"));
        assert!(ini.to_text().contains("\"C:/a b/x ; y\""));
        ini.set("fs", "romfs", "C:/plain/path");
        assert_eq!(ini.to_text(), "[fs]\r\nromfs = C:/plain/path\r\n");
    }

    #[test]
    fn lf_files_stay_lf_and_empty_values_work() {
        let mut ini = Ini::parse("[fs]\nsave_dir = \n");
        ini.set("fs", "save_dir", "D:/s");
        assert_eq!(ini.to_text(), "[fs]\nsave_dir = D:/s\n");
        ini.set("fs", "save_dir", "");
        assert_eq!(ini.get("fs", "save_dir").as_deref(), Some(""));
    }
}
