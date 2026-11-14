// SPDX-License-Identifier: GPL-3.0-or-later
// The FidelityFX DLL search (ffx_files.h).
#include "extras/ffx_files.h"

#include <windows.h>

#include <vector>

#include "nxrt/config.h"
#include "nxrt/extras.h"
#include "nxrt/log.h"

namespace nxrt::extras {
namespace {

constexpr const wchar_t* kLoader = L"amd_fidelityfx_loader_vk.dll";
constexpr const wchar_t* kUpscaler = L"amd_fidelityfx_upscaler_vk.dll";
constexpr const wchar_t* kV114 = L"amd_fidelityfx_vk.dll";

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
bool exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool is_dir(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
std::wstring slash(std::wstring d) {
    if (!d.empty() && d.back() != L'\\' && d.back() != L'/') d += L'\\';
    return d;
}

// A setting names a folder, or a file in it (the DLL itself, as players often paste): the folder.
std::wstring setting_dir(const std::string& v) {
    if (v.empty()) return {};
    const std::wstring p = widen(v);
    if (is_dir(p)) return slash(p);
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s + 1);
}

std::vector<std::wstring> default_dirs() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring exe(buf, n);
    exe = exe.substr(0, exe.find_last_of(L"\\/") + 1);
    wchar_t cwd[MAX_PATH];
    const std::wstring run = slash(std::wstring(cwd, GetCurrentDirectoryW(MAX_PATH, cwd)));
    // the launcher names its folder (the release package's upscalers\fsr is next to it; the run folder may be elsewhere)
    wchar_t ld[MAX_PATH];
    const DWORD ln = GetEnvironmentVariableW(L"NXRT_LAUNCHER_DIR", ld, MAX_PATH);
    const std::wstring launcher = ln && ln < MAX_PATH ? slash(std::wstring(ld, ln)) : std::wstring();
    std::vector<std::wstring> v;
    auto add = [&v](const std::wstring& d) {
        for (const std::wstring& o : v)
            if (_wcsicmp(o.c_str(), d.c_str()) == 0) return;
        v.push_back(d);
    };
    add(exe + L"upscalers\\fsr\\");
    if (!launcher.empty()) add(launcher + L"upscalers\\fsr\\");
    add(run + L"upscalers\\fsr\\");
    add(exe + L"upscalers\\fsr3\\");
    add(exe);
    return v;
}

std::string list(const std::vector<std::wstring>& dirs) {
    std::string s;
    for (const std::wstring& d : dirs) s += (s.empty() ? "" : ", ") + ffx_narrow(d);
    return s;
}

// The upscaler in one folder: the v2.4.0 upscaler (through its loader when present), else v1.1.4. missing: what the folder lacks when it
// holds part of a set.
FfxFind upscaler_in(const std::wstring& d, std::string* partial) {
    FfxFind f;
    if (exists(d + kUpscaler)) {
        f.dll = exists(d + kLoader) ? d + kLoader : d + kUpscaler;
        f.version = "v2.4.0";
        return f;
    }
    if (exists(d + kV114)) {
        f.dll = d + kV114;
        f.version = "v1.1.4";
        if (exists(d + kLoader) && partial)
            *partial = ffx_narrow(d) + " has amd_fidelityfx_loader_vk.dll but not amd_fidelityfx_upscaler_vk.dll (FSR SDK v2.4.0): the v1.1.4 "
                                       "amd_fidelityfx_vk.dll there is used instead";
        return f;
    }
    if (exists(d + kLoader) && partial && partial->empty())
        *partial = ffx_narrow(d) + " has amd_fidelityfx_loader_vk.dll but lacks amd_fidelityfx_upscaler_vk.dll (FSR SDK v2.4.0, signedbin)";
    return f;
}

// The folders to search: the one a setting names first, then the default ones. A setting that names a folder without the files is logged
// (with what it lacks) and the default folders are searched after it, so an old path left in ObeliskChamber.ini cannot hide the release package's files.
std::vector<std::wstring> search_dirs(const std::wstring& set) {
    std::vector<std::wstring> v;
    if (!set.empty()) v.push_back(set);
    for (const std::wstring& d : default_dirs())
        if (set.empty() || _wcsicmp(d.c_str(), set.c_str()) != 0) v.push_back(d);
    return v;
}

}  // namespace

std::string ffx_narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

FfxFind ffx_find_upscaler() {
    static const FfxFind v = [] {
        const std::wstring set = setting_dir(settings().upscale.fsr_dll);
        const std::vector<std::wstring> dirs = search_dirs(set);
        std::string partial;
        for (const std::wstring& d : dirs) {
            FfxFind f = upscaler_in(d, &partial);
            if (!f.dll.empty()) {
                if (!partial.empty()) NXLOG_W("fsr3", "%s", partial.c_str());
                if (!set.empty() && d != set)
                    NXLOG_W("fsr3", "[upscale] fsr_dll names %s, which has neither amd_fidelityfx_upscaler_vk.dll nor amd_fidelityfx_vk.dll: %s is used",
                            ffx_narrow(set).c_str(), ffx_narrow(f.dll).c_str());
                return f;
            }
        }
        FfxFind f;
        f.missing = !partial.empty() ? partial
                                     : "amd_fidelityfx_upscaler_vk.dll (FSR SDK v2.4.0, with amd_fidelityfx_loader_vk.dll) or amd_fidelityfx_vk.dll "
                                       "(v1.1.4) is in none of " + list(dirs);
        return f;
    }();
    return v;
}

FfxFind ffx_find_framegen() {
    static const FfxFind v = [] {
        std::wstring set = setting_dir(config_get_str("dlss", "frame_gen_dll", ""));
        if (set.empty()) set = setting_dir(settings().upscale.fsr_dll);   // the one FSR folder of the launcher
        const std::vector<std::wstring> dirs = search_dirs(set);
        for (const std::wstring& d : dirs)
            if (exists(d + kV114)) {
                if (!set.empty() && d != set)
                    NXLOG_W("framegen", "%s has no amd_fidelityfx_vk.dll: %s is used", ffx_narrow(set).c_str(), ffx_narrow(d + kV114).c_str());
                FfxFind f;
                f.dll = d + kV114;
                f.version = "v1.1.4";
                return f;
            }
        FfxFind f;
        f.missing = "amd_fidelityfx_vk.dll (FidelityFX SDK v1.1.4, PrebuiltSignedDLL) is in none of " + list(dirs);
        for (const std::wstring& d : dirs)
            if (exists(d + kUpscaler) || exists(d + kLoader)) {
                f.missing += " (" + ffx_narrow(d) + " has the v2.4.0 upscaler; frame generation here uses the v1.1.4 DLL)";
                break;
            }
        return f;
    }();
    return v;
}

}  // namespace nxrt::extras
