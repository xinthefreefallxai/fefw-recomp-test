// SPDX-License-Identifier: GPL-3.0-or-later
// Overwolf installs Vulkan layers (VK_LAYER_OW_*, ow-graphics-vulkan.dll, ow-graphics-hook64.dll) in the registry. The Vulkan loader loads
// them into every Vulkan process on the PC, whether or not Overwolf is running, and their driver hooks collide with the ones Streamline and
// NGX install while DLSS starts (a heap corruption, 0xC0000374, in a random one of several launches). This reads the registered layer
// manifests before the loader is opened and disables the Overwolf ones for this process only, through the "disable_environment" controls of
// their manifests and VK_LOADER_LAYERS_DISABLE (Khronos Vulkan-Loader, docs/LoaderLayerInterface.md). Registry entries and files are not
// touched; other programs keep their overlay.
#include "vk_layer_filter.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace nxrt::gfx {
namespace {

constexpr const char* kTag = "gfx";

// ---- a small JSON reader: just enough for layer manifests (objects, arrays, strings; other values are skipped) -------------------------
struct Json {
    enum Type { Other, Str, Arr, Obj } type = Other;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json* find(const char* key) const {
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

struct Reader {
    const std::string& s;
    size_t i = 0;
    bool ok = true;
    explicit Reader(const std::string& text) : s(text) {}
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    static void utf8(std::string& out, uint32_t c) {
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
        else { out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    }
    std::string string() {
        std::string out;
        ++i;   // opening quote
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) break;
            c = s[i++];
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    uint32_t v = 0;
                    for (int k = 0; k < 4 && i < s.size(); ++k, ++i) {
                        const char h = s[i];
                        v = v * 16 + static_cast<uint32_t>(h >= '0' && h <= '9' ? h - '0' : (h | 0x20) >= 'a' && (h | 0x20) <= 'f' ? (h | 0x20) - 'a' + 10 : 0);
                    }
                    utf8(out, v);
                    break;
                }
                default: out += c; break;   // " \ /
            }
        }
        if (i >= s.size()) ok = false;
        else ++i;   // closing quote
        return out;
    }
    Json value(int depth) {
        Json j;
        ws();
        if (i >= s.size() || depth > 32) { ok = false; return j; }
        const char c = s[i];
        if (c == '"') { j.type = Json::Str; j.str = string(); }
        else if (c == '{') {
            j.type = Json::Obj;
            ++i;
            ws();
            if (i < s.size() && s[i] == '}') { ++i; return j; }
            while (ok) {
                ws();
                if (i >= s.size() || s[i] != '"') { ok = false; break; }
                std::string key = string();
                ws();
                if (i >= s.size() || s[i] != ':') { ok = false; break; }
                ++i;
                Json v = value(depth + 1);
                j.obj.emplace_back(std::move(key), std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                ok = false;
            }
        } else if (c == '[') {
            j.type = Json::Arr;
            ++i;
            ws();
            if (i < s.size() && s[i] == ']') { ++i; return j; }
            while (ok) {
                j.arr.push_back(value(depth + 1));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; break; }
                ok = false;
            }
        } else {   // number, true, false, null
            const size_t b = i;
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') ++i;
            if (i == b) ok = false;
        }
        return j;
    }
};

std::string lower(std::string t) {
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return t;
}

bool is_overwolf(const std::string& text) {
    const std::string t = lower(text);
    return t.find("overwolf") != std::string::npos || t.find("ow-graphics") != std::string::npos;
}

void collect_layer(const Json& layer, bool path_matches, std::set<std::string>& names, std::map<std::string, std::string>& environment) {
    if (layer.type != Json::Obj) return;
    const Json* name = layer.find("name");
    const Json* library = layer.find("library_path");
    const bool by_name = name && name->type == Json::Str && is_overwolf(name->str);
    const bool by_library = library && library->type == Json::Str && is_overwolf(library->str);
    if (!path_matches && !by_name && !by_library) return;
    if (name && name->type == Json::Str && !name->str.empty()) names.insert(name->str);
    const Json* disable = layer.find("disable_environment");
    if (!disable || disable->type != Json::Obj) return;
    for (const auto& kv : disable->obj)
        if (!kv.first.empty() && kv.first.find('=') == std::string::npos && kv.second.type == Json::Str)
            environment[kv.first] = kv.second.str.empty() ? "1" : kv.second.str;   // the loader needs a non-empty value
}

#ifdef _WIN32
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool read_small_file(const std::wstring& path, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    char buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
        if (out.size() > (1u << 20)) { ok = false; break; }   // manifests are small
    }
    fclose(f);
    return ok;
}
#endif

}  // namespace

void collect_overwolf_layers(const std::string& manifest_text, const std::string& manifest_path, std::set<std::string>& names,
                             std::map<std::string, std::string>& environment) {
    Reader r(manifest_text);
    const Json root = r.value(0);
    if (!r.ok || root.type != Json::Obj) return;
    const bool path_matches = is_overwolf(manifest_path);
    if (const Json* layer = root.find("layer")) collect_layer(*layer, path_matches, names, environment);
    if (const Json* layers = root.find("layers"); layers && layers->type == Json::Arr)
        for (const Json& l : layers->arr) collect_layer(l, path_matches, names, environment);
}

void disable_overwolf_vulkan_layers() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        if (!config_get_bool("gfx", "disable_overwolf_layers", true)) return;
        std::set<std::string> names;
        std::map<std::string, std::string> environment;
        for (const HKEY hive : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
            for (const REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
                for (const wchar_t* subkey : {L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers", L"SOFTWARE\\Khronos\\Vulkan\\ExplicitLayers"}) {
                    HKEY key = nullptr;
                    if (RegOpenKeyExW(hive, subkey, 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) continue;
                    for (DWORD index = 0;; ++index) {
                        std::wstring path(32768, L'\0');
                        DWORD length = static_cast<DWORD>(path.size()), type = 0, enabled = 0, size = sizeof enabled;
                        const LONG res = RegEnumValueW(key, index, path.data(), &length, nullptr, &type, reinterpret_cast<BYTE*>(&enabled), &size);
                        if (res == ERROR_NO_MORE_ITEMS) break;
                        if (res != ERROR_SUCCESS || type != REG_DWORD || size != sizeof enabled || enabled != 0) continue;   // 0 = enabled
                        path.resize(length);
                        std::string text;
                        if (!read_small_file(path, text)) continue;
                        collect_overwolf_layers(text, narrow(path), names, environment);
                    }
                    RegCloseKey(key);
                }
            }
        }
        if (names.empty() && environment.empty()) return;
        std::string disabled;
        char current[4096];
        const DWORD got = GetEnvironmentVariableA("VK_LOADER_LAYERS_DISABLE", current, sizeof current);
        if (got > 0 && got < sizeof current) disabled = current;   // existing exclusions stay
        for (const std::string& n : names) {
            if (!disabled.empty()) disabled += ',';
            disabled += n;
        }
        if (!names.empty()) environment["VK_LOADER_LAYERS_DISABLE"] = disabled;
        for (const auto& kv : environment) {
            _putenv_s(kv.first.c_str(), kv.second.c_str());
            SetEnvironmentVariableA(kv.first.c_str(), kv.second.c_str());
        }
        NXLOG_I(kTag, "disabled %zu installed Overwolf Vulkan layer(s) for this process (VK_LOADER_LAYERS_DISABLE=%s)", names.size(),
                environment.count("VK_LOADER_LAYERS_DISABLE") ? environment["VK_LOADER_LAYERS_DISABLE"].c_str() : "");
    });
#endif
}

}  // namespace nxrt::gfx
