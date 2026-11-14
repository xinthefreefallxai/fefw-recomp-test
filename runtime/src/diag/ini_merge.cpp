// SPDX-License-Identifier: GPL-3.0-or-later
// Writing a player's ini back without losing what they wrote (nxrt/config.h ini_merge; config_write in config.cpp uses it).
// Used by the F1 menu for the game's ini (ObeliskChamber.ini) and by play mode's controls.ini writer: values change in place, comments, blank lines, key order and unknown keys stay.
#include "nxrt/config.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace nxrt {
namespace {

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && is_ws(s[b])) ++b;
    while (e > b && is_ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        start = nl + 1;
    }
    return out;
}

// "[name]" -> true and the lower-case name.
bool header_of(const std::string& line, std::string* name) {
    const std::string t = trim(line);
    if (t.empty() || t[0] != '[') return false;
    const size_t close = t.find(']');
    if (close == std::string::npos) return false;
    *name = lower(trim(t.substr(1, close - 1)));
    return true;
}

// "key = value" -> true, the lower-case key and the position of the '='. Comments, headers and garbage -> false.
bool key_of(const std::string& line, std::string* key, size_t* eq) {
    const std::string t = trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') return false;
    const size_t e = line.find('=');
    if (e == std::string::npos) return false;
    *key = lower(trim(line.substr(0, e)));
    *eq = e;
    return !key->empty();
}

// The trailing comment of a key line's value part (with the whitespace before it), as config.cpp reads it: after a closing quote, or a ';'
// or '#' that follows whitespace. Empty when there is none.
std::string trailing_comment(const std::string& rest) {
    const size_t first = rest.find_first_not_of(" \t");
    size_t from = 0;
    if (first != std::string::npos && rest[first] == '"') {
        const size_t close = rest.find('"', first + 1);
        if (close == std::string::npos) return {};
        from = close + 1;
    }
    for (size_t i = std::max<size_t>(from, 1); i < rest.size(); ++i) {
        if ((rest[i] == ';' || rest[i] == '#') && is_ws(rest[i - 1])) {
            size_t ws = i;
            while (ws > from && is_ws(rest[ws - 1])) --ws;
            return rest.substr(ws);
        }
    }
    return {};
}

struct Update {
    std::string key_text;   // as the update spells it
    std::string value;
    bool done = false;
};
struct UpdateSection {
    std::string header;                 // the header line as the update spells it
    std::vector<std::string> keys;      // lower-case keys in update order
    std::map<std::string, Update> kv;
    std::vector<std::string> raw;       // every non-empty line of the section (replace sections)
    bool emitted = false;               // a replace section's lines are out
};

}  // namespace

std::string ini_merge(const std::string& base, const std::string& updates, const std::vector<std::string>& replace_sections) {
    if (trim(base).empty()) return updates;
    const std::set<std::string> replace(replace_sections.begin(), replace_sections.end());

    // the updates, by section, in their order
    std::vector<std::string> order;
    std::map<std::string, UpdateSection> up;
    {
        std::string sec;
        for (const std::string& line : split_lines(updates)) {
            std::string name, key;
            size_t eq = 0;
            if (header_of(line, &name)) {
                sec = name;
                if (!up.count(sec)) order.push_back(sec);
                up[sec].header = trim(line);
                continue;
            }
            if (trim(line).empty()) continue;
            if (!up.count(sec)) {
                order.push_back(sec);
                up[sec].header = sec.empty() ? "" : "[" + sec + "]";
            }
            UpdateSection& u = up[sec];
            u.raw.push_back(line);
            if (key_of(line, &key, &eq)) {
                if (!u.kv.count(key)) u.keys.push_back(key);
                u.kv[key] = Update{trim(line.substr(0, eq)), trim(line.substr(eq + 1)), false};
            }
        }
    }

    const std::vector<std::string> lines = split_lines(base);
    const bool crlf = base.find("\r\n") != std::string::npos;
    const char* eol = crlf ? "\r\n" : "\n";

    // the last key line (or the header) of each section's last block: missing keys go after it
    std::map<std::string, size_t> last_line;
    {
        std::string sec;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string name, key;
            size_t eq = 0;
            if (header_of(lines[i], &name)) {
                sec = name;
                last_line[sec] = i;
            } else if (key_of(lines[i], &key, &eq)) {
                last_line[sec] = i;
            }
        }
    }

    std::string out;
    auto put = [&](const std::string& l) { out += l; out += eol; };
    auto flush_missing = [&](UpdateSection& u) {
        for (const std::string& k : u.keys) {
            Update& v = u.kv[k];
            if (v.done) continue;
            put(v.key_text + " = " + v.value);
            v.done = true;
        }
    };
    std::string sec;
    bool in_replace = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        std::string name, key;
        size_t eq = 0;
        if (header_of(line, &name)) {
            sec = name;
            in_replace = replace.count(sec) != 0;
            put(line);
            if (in_replace) {   // the update's lines (once, at the first block of the section); the old block is dropped
                auto it = up.find(sec);
                if (it != up.end() && !it->second.emitted) {
                    for (const std::string& r : it->second.raw) put(r);
                    it->second.emitted = true;
                    for (auto& kv : it->second.kv) kv.second.done = true;
                }
            }
        } else if (in_replace) {
            if (trim(line).empty()) put(line);   // keep the spacing between sections
            continue;
        } else if (key_of(line, &key, &eq)) {
            Update* hit = nullptr;
            if (auto it = up.find(sec); it != up.end())
                if (auto kv = it->second.kv.find(key); kv != it->second.kv.end()) hit = &kv->second;
            if (hit) {
                // keep the key as written and the spacing around '=', change the value, keep a trailing comment
                const std::string rest = line.substr(eq + 1);
                const size_t vpos = rest.find_first_not_of(" \t");
                const std::string pad = vpos == std::string::npos ? std::string(" ") : rest.substr(0, vpos);
                put(line.substr(0, eq + 1) + pad + hit->value + trailing_comment(rest));
                hit->done = true;
            } else {
                put(line);
            }
        } else {
            put(line);
        }
        if (!in_replace) {
            auto ll = last_line.find(sec);
            auto it = up.find(sec);
            if (ll != last_line.end() && ll->second == i && it != up.end()) flush_missing(it->second);
        }
    }
    // sections the file does not have yet
    for (const std::string& s : order) {
        UpdateSection& u = up[s];
        if (u.emitted) continue;
        bool any = false;
        for (const auto& kv : u.kv) any = any || !kv.second.done;
        if (!any && !(replace.count(s) && !last_line.count(s))) continue;
        const std::string blank = std::string(eol) + eol;   // a blank line before the new section
        if (!out.empty() && (out.size() < blank.size() || out.compare(out.size() - blank.size(), blank.size(), blank) != 0)) out += eol;
        if (!u.header.empty()) put(u.header);
        if (replace.count(s)) {
            for (const std::string& r : u.raw) put(r);
            u.emitted = true;
        } else {
            flush_missing(u);
        }
    }
    return out;
}

}  // namespace nxrt
