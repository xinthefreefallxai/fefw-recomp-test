// SPDX-License-Identifier: GPL-3.0-or-later
// NVIDIA DLSS Super Resolution / DLAA through Streamline (temporal.h). Streamline (MIT headers in third_party/nvidia_streamline) is loaded at
// run time from sl.interposer.dll; its plugins (sl.common.dll, sl.dlss.dll) and nvngx_dlss.dll sit next to it. None of NVIDIA's files is part of
// this project: the player puts the four DLLs of NVIDIA's Streamline SDK (bin/x64) in upscalers\streamline\ next to the executable (the launcher's
// folder in the release package), or in the run folder, or names their folder in [upscale] dlss_dll.
// Nothing is linked. Streamline has to see vkCreateInstance / vkCreateDevice / the swapchain calls, so with DLSS or DLAA selected at start-up
// the Vulkan loader functions come from its vkGetInstanceProcAddr (streamline_preinit, called by vk_device.cpp before volk is initialised).
// Choosing DLSS later in the F1 page takes effect at the next start.
#include <windows.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

#include "extras/framegen.h"
#include "extras/temporal.h"
#include "nxrt/config.h"
#include "nxrt/extras.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"

#include "../../../third_party/nvidia_streamline/include/sl.h"
#include "../../../third_party/nvidia_streamline/include/sl_consts.h"
#include "../../../third_party/nvidia_streamline/include/sl_dlss.h"
#include "../../../third_party/nvidia_streamline/include/sl_security.h"

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace nxrt::extras {
namespace {

constexpr const char* kTag = "dlss";

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    char buf[1024];
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, buf, sizeof buf, nullptr, nullptr);
    return buf;
}
bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The Streamline files a DLSS run needs (frame generation adds more, see streamline_preinit).
constexpr const wchar_t* kRequired[] = {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss.dll", L"nvngx_dlss.dll"};
// ... and the ones DLSS Frame Generation adds (Reflex and PCL are required by DLSS-G; NvLowLatencyVk.dll is Reflex on Vulkan)
constexpr const wchar_t* kRequiredFg[] = {L"sl.dlss_g.dll", L"sl.reflex.dll", L"sl.pcl.dll", L"nvngx_dlssg.dll", L"NvLowLatencyVk.dll"};

// The folder holding sl.interposer.dll: [upscale] dlss_dll (a folder, or the path of sl.interposer.dll / any file in it); unset: the folder
// upscalers\streamline next to the executable, else next to the launcher (NXRT_LAUNCHER_DIR), else in the run folder (the working directory), else
// the executable's own folder.
std::wstring sl_dir() {
    std::wstring p = widen(settings().upscale.dlss_dll);
    if (p.empty()) {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        p.assign(buf, n);
        const size_t s = p.find_last_of(L"\\/");
        const std::wstring exe = s == std::wstring::npos ? std::wstring() : p.substr(0, s + 1);
        wchar_t cwd[MAX_PATH];
        const DWORD cn = GetCurrentDirectoryW(MAX_PATH, cwd);
        std::wstring run(cwd, cn);
        if (!run.empty() && run.back() != L'\\') run += L'\\';
        wchar_t ld[MAX_PATH];   // the launcher's folder (NXRT_LAUNCHER_DIR, set by the launcher): the run folder may be elsewhere
        const DWORD ln = GetEnvironmentVariableW(L"NXRT_LAUNCHER_DIR", ld, MAX_PATH);
        std::wstring launcher = ln && ln < MAX_PATH ? std::wstring(ld, ln) : std::wstring();
        if (!launcher.empty() && launcher.back() != L'\\') launcher += L'\\';
        for (const std::wstring& base : {exe, launcher, run}) {
            if (base.empty()) continue;
            const std::wstring pkg = base + L"upscalers\\streamline\\";
            if (exists(pkg + L"sl.interposer.dll")) return pkg;
        }
        return exe;
    }
    const DWORD a = GetFileAttributesW(p.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return p.back() == L'\\' || p.back() == L'/' ? p : p + L"\\";
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s + 1);
}

struct Sl {
    HMODULE mod = nullptr;
    bool started = false;
    bool framegen = false;
    PFun_slInit* init = nullptr;
    PFun_slIsFeatureSupported* supported = nullptr;
    PFun_slGetNewFrameToken* token = nullptr;
    PFun_slSetConstants* constants = nullptr;
    PFun_slSetTagForFrame* tag = nullptr;
    PFun_slEvaluateFeature* evaluate = nullptr;
    PFun_slGetFeatureFunction* feature_fn = nullptr;
    PFun_slDLSSSetOptions* set_options = nullptr;
    PFun_slDLSSGetOptimalSettings* optimal = nullptr;
};
Sl& sl_state() {
    static Sl s;
    return s;
}

void on_log(sl::LogType type, const char* msg) {
    std::string m = msg ? msg : "";
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    if (type == sl::LogType::eError) NXLOG_E(kTag, "streamline: %s", m.c_str());
    else if (type == sl::LogType::eWarn) NXLOG_W(kTag, "streamline: %s", m.c_str());
    else if (config_get_bool("dlss", "verbose", false)) NXLOG_I(kTag, "streamline: %s", m.c_str());
}

sl::Resource resource(const TemporalImage& t) {
    sl::Resource r(sl::ResourceType::eTex2d, reinterpret_cast<void*>(t.image), nullptr, reinterpret_cast<void*>(t.view),
                   static_cast<uint32_t>(VK_IMAGE_LAYOUT_GENERAL));
    r.width = t.width;
    r.height = t.height;
    r.nativeFormat = static_cast<uint32_t>(t.format);
    r.mipLevels = 1;
    r.arrayLayers = 1;
    r.flags = 0;
    r.usage = t.usage;
    return r;
}

sl::float4x4 mat(const float* cm) {   // column-major column-vector matrix -> Streamline's row-major row-vector matrix (the same 16 floats)
    sl::float4x4 m;
    for (int i = 0; i < 4; ++i) m.row[i] = sl::float4(cm[i * 4 + 0], cm[i * 4 + 1], cm[i * 4 + 2], cm[i * 4 + 3]);
    return m;
}

class Dlss final : public TemporalBackend {
public:
    explicit Dlss(gfx::Device& dev) : dev_(&dev) {}
    const char* name() const override { return dlaa_ ? "DLAA" : "DLSS"; }
    void set_dlaa(bool on) { dlaa_ = on; }

    bool evaluate(VkCommandBuffer cb, const TemporalFrame& f) override {
        Sl& s = sl_state();
        if (failed_) return false;
        // DLSS-G consumes viewport 0 at present; keep SR tags/constants separate.
        const sl::ViewportHandle vp(framegen_dlss() ? 1u : 0u);
        sl::FrameToken* frame = static_cast<sl::FrameToken*>(sl_frame_token());
        if (!frame) return fail("slGetNewFrameToken");
        // The mode: kept while the input size stays in its dynamic range (the game may change its render size per frame), else the one
        // whose scale is nearest the actual ratio and whose range holds the input (DLAA at the same size).
        const bool same_out = f.out.width == last_out_.width && f.out.height == last_out_.height;
        if (!same_out || !in_range(range_, f.render) || extras::dlss_preset() != last_preset_) {   // also when the model preset changed (F1)
            sl::DLSSMode m = sl::DLSSMode::eDLAA;
            Range rg{f.out.width, f.out.height, f.out.width, f.out.height};
            const double r = static_cast<double>(f.out.width) / static_cast<double>(f.render.width);
            if (!dlaa_ && r > 1.0005) {   // any smaller input is a scaling mode's: DLAA's range is the output size alone ([nvn] dynres renders 1/120 under it)
                static constexpr struct { sl::DLSSMode m; double scale; } kModes[] = {
                    {sl::DLSSMode::eUltraQuality, 1.3}, {sl::DLSSMode::eMaxQuality, 1.5}, {sl::DLSSMode::eBalanced, 1.0 / 0.58},
                    {sl::DLSSMode::eMaxPerformance, 2.0}, {sl::DLSSMode::eUltraPerformance, 3.0}};
                double best = 1e9;
                bool best_holds = false;
                for (const auto& k : kModes) {
                    Range kr;
                    if (!range_for(s, k.m, f.out, &kr)) continue;
                    const bool holds = in_range(kr, f.render);
                    const double d = std::fabs(std::log(r / k.scale));
                    if ((holds && !best_holds) || (holds == best_holds && d < best)) {
                        best = d;
                        best_holds = holds;
                        m = k.m;
                        rg = kr;
                    }
                }
            }
            const int preset = extras::dlss_preset();
            const bool new_opts = m != last_mode_ || !same_out || preset != last_preset_;
            if (new_opts && s.set_options(vp, options(m, f.out)) != sl::Result::eOk) return fail("slDLSSSetOptions");
            if (new_opts)
                NXLOG_I(kTag, "%s: mode %s, model preset %s, %ux%u -> %ux%u (input range %ux%u .. %ux%u)", name(), mode_name(m), extras::dlss_preset_word(preset),
                        f.render.width, f.render.height, f.out.width, f.out.height, rg.min_w, rg.min_h, rg.max_w, rg.max_h);
            last_preset_ = preset;
            if (!in_range(rg, f.render) && !same_out)
                NXLOG_W(kTag, "%s: input %ux%u is outside every mode's range for %ux%u", name(), f.render.width, f.render.height, f.out.width, f.out.height);
            last_mode_ = m;
            last_out_ = f.out;
            range_ = rg;
        }
        sl::Constants c;
        c.cameraViewToClip = mat(f.view_to_clip);
        c.clipToCameraView = mat(f.clip_to_view);
        c.clipToLensClip = mat(kIdentity);
        c.clipToPrevClip = mat(f.clip_to_prev_clip);
        c.prevClipToClip = mat(f.prev_clip_to_clip);
        c.jitterOffset = sl::float2(f.jitter_px[0], f.jitter_px[1]);
        c.mvecScale = sl::float2(1.0f, 1.0f);   // the motion vectors are already in UV units ([-1, 1] of the render size)
        c.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
        c.cameraPos = sl::float3(f.cam_pos[0], f.cam_pos[1], f.cam_pos[2]);
        c.cameraUp = sl::float3(f.cam_up[0], f.cam_up[1], f.cam_up[2]);
        c.cameraRight = sl::float3(f.cam_right[0], f.cam_right[1], f.cam_right[2]);
        c.cameraFwd = sl::float3(f.cam_fwd[0], f.cam_fwd[1], f.cam_fwd[2]);
        c.cameraNear = f.near_z;
        c.cameraFar = f.far_z;
        c.cameraFOV = f.fov_y;
        c.cameraAspectRatio = f.aspect;
        c.depthInverted = sl::Boolean::eTrue;
        c.cameraMotionIncluded = sl::Boolean::eTrue;
        c.motionVectors3D = sl::Boolean::eFalse;
        c.reset = f.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        c.motionVectorsJittered = sl::Boolean::eFalse;
        if (s.constants(c, *frame, vp) != sl::Result::eOk) return fail("slSetConstants");
        sl::Resource col = resource(f.color), dep = resource(f.depth), mv = resource(f.motion), out = resource(f.output);
        const sl::Extent in_ext{0, 0, f.render.width, f.render.height}, out_ext{0, 0, f.out.width, f.out.height};
        sl::ResourceTag tags[4] = {sl::ResourceTag(&col, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &in_ext),
                                   sl::ResourceTag(&dep, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &in_ext),
                                   sl::ResourceTag(&mv, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &in_ext),
                                   sl::ResourceTag(&out, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &out_ext)};
        auto* slcb = reinterpret_cast<sl::CommandBuffer*>(cb);
        if (s.tag(*frame, vp, tags, 4, slcb) != sl::Result::eOk) return fail("slSetTagForFrame");
        const sl::BaseStructure* inputs[] = {&vp};
        const sl::Result er = s.evaluate(sl::kFeatureDLSS, *frame, inputs, 1, slcb);
        if (er != sl::Result::eOk) return fail("slEvaluateFeature", static_cast<int>(er));
        return true;
    }

private:
    struct Range {
        uint32_t min_w = 0, min_h = 0, max_w = 0, max_h = 0;
    };
    static bool in_range(const Range& r, VkExtent2D e) {
        return e.width >= r.min_w && e.height >= r.min_h && e.width <= r.max_w && e.height <= r.max_h && r.max_w;
    }
    static sl::DLSSOptions options(sl::DLSSMode m, VkExtent2D out) {
        sl::DLSSOptions o;
        o.mode = m;
        o.outputWidth = out.width;
        o.outputHeight = out.height;
        o.colorBuffersHDR = sl::Boolean::eTrue;
        o.useAutoExposure = sl::Boolean::eTrue;
        // [upscale] dlss_preset (F1: DLSS model): the same model for every mode; 0 leaves the library's default (K for DLAA and Quality)
        const sl::DLSSPreset pr = static_cast<sl::DLSSPreset>(extras::dlss_preset());
        o.dlaaPreset = o.qualityPreset = o.balancedPreset = o.performancePreset = o.ultraPerformancePreset = o.ultraQualityPreset = pr;
        return o;
    }
    static bool range_for(Sl& s, sl::DLSSMode m, VkExtent2D out, Range* r) {
        sl::DLSSOptimalSettings os;
        if (!s.optimal || s.optimal(options(m, out), os) != sl::Result::eOk || !os.optimalRenderWidth) return false;
        r->min_w = os.renderWidthMin ? os.renderWidthMin : os.optimalRenderWidth;
        r->min_h = os.renderHeightMin ? os.renderHeightMin : os.optimalRenderHeight;
        r->max_w = os.renderWidthMax ? os.renderWidthMax : os.optimalRenderWidth;
        r->max_h = os.renderHeightMax ? os.renderHeightMax : os.optimalRenderHeight;
        return true;
    }
    static const char* mode_name(sl::DLSSMode m) {
        switch (m) {
            case sl::DLSSMode::eMaxPerformance: return "Performance";
            case sl::DLSSMode::eBalanced: return "Balanced";
            case sl::DLSSMode::eMaxQuality: return "Quality";
            case sl::DLSSMode::eUltraPerformance: return "Ultra Performance";
            case sl::DLSSMode::eUltraQuality: return "Ultra Quality";
            case sl::DLSSMode::eDLAA: return "DLAA";
            default: return "?";
        }
    }
    static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool fail(const char* what, int code = -1) {
        NXLOG_E(kTag, "%s failed (%d): DLSS off", what, code);
        // The F1 page shows this beside the fallback: Streamline's own wording, not a guess about the DLLs.
        if (code >= 0) std::snprintf(failure_, sizeof(failure_), "%s failed (%d): DLSS off", what, code);
        else std::snprintf(failure_, sizeof(failure_), "%s failed: DLSS off", what);
        failed_ = true;
        return false;
    }
    const char* last_failure() const override { return failure_; }
    [[maybe_unused]] gfx::Device* dev_;
    bool dlaa_ = false, failed_ = false;
    char failure_[96] = {};
    sl::DLSSMode last_mode_ = sl::DLSSMode::eCount;
    int last_preset_ = -1;
    VkExtent2D last_out_ = {};
    Range range_;
};

}  // namespace

bool temporal_dlss_available() {
    const std::wstring dir = sl_dir();
    for (const wchar_t* f : kRequired)
        if (!exists(dir + f)) return false;
    return true;
}
bool temporal_dlss_started() { return sl_state().started; }
bool streamline_framegen_files() {
    if (!temporal_dlss_available()) return false;
    const std::wstring dir = sl_dir();
    for (const wchar_t* f : kRequiredFg)
        if (!exists(dir + f)) return false;
    return true;
}

// The NVN2 DLSS API's host upscaler (src/gfx/nvn/api_dlss_replay.cpp) wants Streamline with [dlss] upscaler = dlss, and with auto when the
// DLLs are there.
static bool nvn_dlss_wants_streamline() {
    std::string u = config_get_str("dlss", "upscaler", "taau");
    for (char& c : u) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return u == "dlss" || (u == "auto" && temporal_dlss_available()) || framegen_dlss();
}

void* streamline_proc(const char* name) {
    Sl& s = sl_state();
    return s.started && s.mod ? reinterpret_cast<void*>(GetProcAddress(s.mod, name)) : nullptr;
}

PFN_vkGetInstanceProcAddr streamline_preinit() {
    const UpscaleMode m = settings().upscale.mode;
    if (m != UpscaleMode::Dlss && m != UpscaleMode::Dlaa && !nvn_dlss_wants_streamline()) return nullptr;
    Sl& s = sl_state();
    const std::wstring dir = sl_dir();
    const std::wstring dll = dir + L"sl.interposer.dll";
    if (!exists(dll)) {
        NXLOG_W(kTag, "DLSS selected but %s is missing: put the Streamline DLLs (sl.interposer.dll, sl.common.dll, sl.dlss.dll, nvngx_dlss.dll) in "
                      "upscalers\\streamline next to the launcher, or name their folder in [upscale] dlss_dll", narrow(dll).c_str());
        return nullptr;
    }
    {
        std::string missing;
        for (const wchar_t* f : kRequired)
            if (!exists(dir + f)) missing += (missing.empty() ? "" : ", ") + narrow(f);
        if (!missing.empty()) {
            NXLOG_W(kTag, "DLSS selected but %s lacks %s: copy all four Streamline files (sl.interposer.dll, sl.common.dll, sl.dlss.dll, nvngx_dlss.dll) "
                          "there; the built-in upscaler is used", narrow(dir).c_str(), missing.c_str());
            return nullptr;
        }
    }
    if (!sl::security::verifyEmbeddedSignature(dll.c_str())) {
        NXLOG_E(kTag, "%s has no valid NVIDIA signature: not loaded", narrow(dll).c_str());
        return nullptr;
    }
    s.mod = LoadLibraryW(dll.c_str());
    if (!s.mod) {
        NXLOG_E(kTag, "LoadLibrary(%s) failed (error %lu)", narrow(dll).c_str(), GetLastError());
        return nullptr;
    }
    auto get = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(s.mod, n)); };
    s.init = reinterpret_cast<PFun_slInit*>(get("slInit"));
    s.supported = reinterpret_cast<PFun_slIsFeatureSupported*>(get("slIsFeatureSupported"));
    s.token = reinterpret_cast<PFun_slGetNewFrameToken*>(get("slGetNewFrameToken"));
    s.constants = reinterpret_cast<PFun_slSetConstants*>(get("slSetConstants"));
    s.tag = reinterpret_cast<PFun_slSetTagForFrame*>(get("slSetTagForFrame"));
    s.evaluate = reinterpret_cast<PFun_slEvaluateFeature*>(get("slEvaluateFeature"));
    s.feature_fn = reinterpret_cast<PFun_slGetFeatureFunction*>(get("slGetFeatureFunction"));
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(get("vkGetInstanceProcAddr"));
    if (!s.init || !s.supported || !s.token || !s.constants || !s.tag || !s.evaluate || !s.feature_fn || !gipa) {
        NXLOG_E(kTag, "sl.interposer.dll lacks the Streamline API");
        return nullptr;
    }
    static const wchar_t* paths[1];
    static std::wstring plugin_dir;
    plugin_dir = dir;
    paths[0] = plugin_dir.c_str();
    static const sl::Feature features[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    bool fg = framegen_dlss();
    if (fg) {
        std::string missing;
        for (const wchar_t* f : kRequiredFg)
            if (!exists(dir + f)) missing += (missing.empty() ? "" : ", ") + narrow(f);
        if (!missing.empty()) {
            NXLOG_W(kTag, "DLSS Frame Generation off: %s lacks %s (Streamline SDK bin/x64; frame generation needs sl.dlss_g.dll, sl.reflex.dll, "
                          "sl.pcl.dll, nvngx_dlssg.dll and NvLowLatencyVk.dll beside the four DLSS files)", narrow(dir).c_str(), missing.c_str());
            fg = false;
        }
    }
    sl::Preferences pref;
    pref.showConsole = false;
    pref.logLevel = config_get_bool("dlss", "verbose", false) ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = nullptr;
    pref.logMessageCallback = &on_log;
    // no over-the-air downloads; the host restores no command buffer state (the replay rebinds everything after the evaluation)
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = fg ? 4 : 1;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0";
    pref.projectId = "5a1f0c2e-8f3b-4e61-9d4c-7b2e6a90c3d5";
    pref.renderAPI = sl::RenderAPI::eVulkan;
    const sl::Result r = s.init(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        NXLOG_E(kTag, "slInit failed (%d): DLSS off", static_cast<int>(r));
        return nullptr;
    }
    s.started = true;
    s.framegen = fg;
    NXLOG_I(kTag, "Streamline %d.%d.%d started from %s: the Vulkan instance and device are created through it", SL_VERSION_MAJOR, SL_VERSION_MINOR,
            SL_VERSION_PATCH, narrow(dir).c_str());
    return gipa;
}

bool streamline_framegen_available() { return sl_state().started && sl_state().framegen; }

void streamline_check_framegen(VkPhysicalDevice physical) {
    Sl& s = sl_state();
    if (!s.started || !s.framegen) return;
    sl::AdapterInfo ai;
    ai.vkPhysicalDevice = reinterpret_cast<void*>(physical);
    const sl::Result r = s.supported(sl::kFeatureDLSS_G, ai);
    if (r == sl::Result::eOk) return;
    if (framegen_support_override(static_cast<int>(r))) {   // player-supplied DLLs for older GPUs: fg_init asks the plugin itself
        NXLOG_W(kTag, "DLSS-G: slIsFeatureSupported %d on this GPU; kept loaded ([dlss] frame_gen_any_gpu)", static_cast<int>(r));
        return;
    }
    NXLOG_W(kTag, "DLSS-G unavailable (slIsFeatureSupported %d)%s: using ordinary presentation", static_cast<int>(r),
            r == sl::Result::eErrorOSDisabledHWS ? "; enable Windows hardware-accelerated GPU scheduling (HAGS) and restart Windows" :
            "; check GPU/driver, Windows HAGS and Streamline logs");
    auto unload = reinterpret_cast<PFun_slSetFeatureLoaded*>(streamline_proc("slSetFeatureLoaded"));
    auto is_loaded = reinterpret_cast<PFun_slIsFeatureLoaded*>(streamline_proc("slIsFeatureLoaded"));
    bool loaded = true;
    if (is_loaded) is_loaded(sl::kFeatureDLSS_G, loaded);
    if (loaded && unload) {
        const sl::Result u = unload(sl::kFeatureDLSS_G, false);
        if (u != sl::Result::eOk) NXLOG_W(kTag, "could not unload unsupported DLSS-G (%d)", static_cast<int>(u));
    }
    s.framegen = false;
}

void streamline_shutdown() {
    Sl& s = sl_state();
    if (!s.started) return;
    auto shutdown = reinterpret_cast<PFun_slShutdown*>(streamline_proc("slShutdown"));
    if (shutdown) {
        const sl::Result r = shutdown();
        if (r != sl::Result::eOk) NXLOG_W(kTag, "slShutdown failed (%d)", static_cast<int>(r));
        else NXLOG_I(kTag, "Streamline shut down");
    }
    s.started = s.framegen = false;
    // Vulkan dispatch pointers still refer into the interposer until instance destruction.
}

TemporalBackend* temporal_dlss(void* gfx_device, bool dlaa) {
    static Dlss* inst = nullptr;
    static bool tried = false;
    Sl& s = sl_state();
    if (!tried && gfx_device) {
        tried = true;
        if (!s.started) {
            NXLOG_W(kTag, "DLSS needs Streamline from the start: choose DLSS or DLAA ([upscale] mode, or [dlss] upscaler for the NVN2 DLSS API) and restart (the DLLs must be in upscalers\\streamline next to the executable or in [upscale] dlss_dll)");
            return nullptr;
        }
        gfx::Device& dev = *static_cast<gfx::Device*>(gfx_device);
        sl::AdapterInfo ai;
        ai.vkPhysicalDevice = reinterpret_cast<void*>(dev.physical());
        const sl::Result sup = s.supported(sl::kFeatureDLSS, ai);
        if (sup != sl::Result::eOk) {
            NXLOG_W(kTag, "DLSS is not supported on this GPU / driver (slIsFeatureSupported %d)", static_cast<int>(sup));
            return nullptr;
        }
        if (s.feature_fn(sl::kFeatureDLSS, "slDLSSSetOptions", reinterpret_cast<void*&>(s.set_options)) != sl::Result::eOk || !s.set_options ||
            s.feature_fn(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", reinterpret_cast<void*&>(s.optimal)) != sl::Result::eOk) {
            NXLOG_E(kTag, "the DLSS plugin functions are missing");
            return nullptr;
        }
        inst = new Dlss(dev);
        NXLOG_I(kTag, "DLSS available");
    }
    if (inst) inst->set_dlaa(dlaa);
    return inst;
}

}  // namespace nxrt::extras
