// SPDX-License-Identifier: GPL-3.0-or-later
// The F1 menu's translation table (declarations in include/nxrt/menu_i18n.h).
//
// [gfx] ui_language names a file next to fefw_rt.dll: locales\<name>.ini. "auto" or empty: English (no table).
// The file holds one "key = value" pair per line (UTF-8, no BOM; '#' or ';' starts a comment). The key is the
// English string as the source code shows it; the value is what the menu displays. A missing key or file falls
// back to the English string, so a partial or broken table only means some lines stay English.
#include "nxrt/menu_i18n.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>

#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::gfx {
namespace {

constexpr const char* kTag = "menu-i18n";

std::unordered_map<std::string, std::string>& table() {
    static std::unordered_map<std::string, std::string> t;
    return t;
}

std::string exe_dir() {
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::string p(buf);
    const size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// The conversions the menu actually writes: they all take exactly one argument. "%n" writes through a pointer it
// was not given, so it is not in the list and a table that uses one is refused below.
bool is_conversion(char c) { return c != '\0' && std::strchr("diouxXeEfFgGaAcsp", c) != nullptr; }

// The printf argument list of a string, in order, one token per argument that printf would read:
//   "%d"   -> "d;"      "%ls" -> "ls;"      "%*d" -> "*;d;"      "%.*f" -> "*;f;"
// A '*' width or precision takes an argument of its own, so it is recorded: a "%*d" and a "%d" read a different
// number of arguments even though they share the conversion, and swapping one for the other is as fatal as
// changing the conversion itself.
// "%%" is a literal percent and takes no argument. A '%' that is not followed by a real conversion is taken as a
// literal too: several display strings are plain prose with a percent in them ("1% low", a translated "50% off")
// and never reach printf, so they must not be dropped for it.
// A conversion that is not in the list above (notably "%n") sets *unsafe and adds a token no other string can
// produce, so the entry reads as a mismatch and is dropped.
std::string conversions(const std::string& s, bool* unsafe = nullptr) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') continue;
        const size_t pct = i;
        ++i;
        if (i >= s.size() || s[i] == '%') continue;
        const unsigned char next = static_cast<unsigned char>(s[i]);
        if (next >= 0x80 || next == ' ') continue;   // prose, not a conversion
        std::string tok;   // kept aside until the conversion proves real, so a false start adds nothing
        size_t j = i;
        while (j < s.size() && s[j] != '\0' && std::strchr("-+#0", s[j]) != nullptr) ++j;
        if (j < s.size() && s[j] == '*') {
            tok += "*;";
            ++j;
        } else {
            while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
        }
        if (j < s.size() && s[j] == '.') {
            ++j;
            if (j < s.size() && s[j] == '*') {
                tok += "*;";
                ++j;
            } else {
                while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
            }
        }
        const size_t len = j;
        while (j < s.size() && s[j] != '\0' && std::strchr("hlLjzt", s[j]) != nullptr) ++j;
        if (j < s.size() && is_conversion(s[j])) {
            tok.append(s, len, j - len);
            tok.push_back(s[j]);
            tok.push_back(';');
            out += tok;
            i = j;
            continue;
        }
        if (j < s.size() && s[j] == 'n') {   // "%n", with or without a length ("%hn", "%ln"): writes to memory
            if (unsafe) *unsafe = true;
            out += "\x01";   // a byte no ASCII token above contains: never equal to a plain string's list
            i = j;
            continue;
        }
        i = pct;   // not a conversion after all: the percent stays a literal
    }
    return out;
}

// "key = value" lines; the value keeps inner spaces, the key is trimmed. UTF-8 in, UTF-8 out (ImGui is UTF-8 aware).
void load_file(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return;
    std::string line;
    size_t n = 0;
    size_t dropped = 0;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xef && static_cast<unsigned char>(line[1]) == 0xbb &&
            static_cast<unsigned char>(line[2]) == 0xbf)
            line = line.substr(3);   // strip a BOM if one slipped in
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        // A tab splits key from value, so a key may itself contain " = " (several help texts do).
        // Without a tab the first '=' splits, as before.
        size_t eq = t.find('\t');
        size_t skip = 1;
        if (eq == std::string::npos) {
            eq = t.find('=');
            skip = 1;
        }
        if (eq == std::string::npos) continue;
        const std::string key = trim(t.substr(0, eq));
        std::string value = trim(t.substr(eq + skip));
        if (key.empty() || value.empty()) continue;
        // A translation is used as the printf format string, so its conversions have to match the English key.
        // One that does not is dropped: the line stays English instead of reading the wrong arguments. A "%n" in
        // the value is dropped whatever the key says, because nothing the menu prints has one.
        bool unsafe = false;
        const std::string key_list = conversions(key);
        const std::string value_list = conversions(value, &unsafe);
        if (unsafe) {
            ++dropped;
            NXLOG_W(kTag, "dropping the translation of \"%s\": it uses %%n, which writes to memory", key.c_str());
            continue;
        }
        if (key_list != value_list) {
            ++dropped;
            NXLOG_W(kTag, "dropping the translation of \"%s\": its format specifiers differ from the key",
                    key.c_str());
            continue;
        }
        table()[key] = value;
        ++n;
    }
    NXLOG_I(kTag, "loaded %zu translations from %s", n, path.c_str());
    if (dropped > 0) NXLOG_W(kTag, "dropped %zu translations whose format specifiers did not match", dropped);
}

} // namespace

void menu_i18n_init() {
    table().clear();
    std::string lang = config_get_str("gfx", "ui_language", "auto");
    lang = trim(lang);
    if (lang.empty() || lang == "auto" || lang == "en" || lang == "en-US") return;   // English: the source strings
    const std::string dir = exe_dir();
    if (dir.empty()) return;
    load_file(dir + "\\locales\\" + lang + ".ini");
}

const char* tr(const char* s) {
    if (!s || !*s) return s;
    const auto& t = table();
    auto it = t.find(s);
    if (it != t.end()) return it->second.c_str();
    // Some display strings carry padding (" settings", a trailing space in a wrapped help);
    // the ini keys are trimmed, so try the trimmed form before giving up.
    const std::string key = trim(std::string(s));
    if (key != s) {
        it = t.find(key);
        if (it != t.end()) return it->second.c_str();
    }
    return s;
}

} // namespace nxrt::gfx
