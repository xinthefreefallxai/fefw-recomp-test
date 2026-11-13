// SPDX-License-Identifier: GPL-3.0-or-later
// Intel XeSS Super Resolution (temporal.h). libxess.dll (Intel XeSS SDK 3.0.2: XeSS-SR 2.0.2, Intel Simplified Software License; it needs
// the Microsoft Visual C++ 2015-2022 runtime, 14.40 or newer) is loaded at run time; it runs on Intel Arc / Iris Xe through XMX or DP4a and on
// other vendors' GPUs through its DP4a path. No Intel header is compiled in: the SDK's headers are under Intel's licence, so the few types and
// entry points used are declared here from the SDK's documented C ABI (xess.h, xess_vk.h of SDK 3.0.2). The approach (and the layout checks)
// follow vk_xess.c of the Astral Chain PC recompilation (MIT; THIRD_PARTY_NOTICES.md).
//
// Search for the DLL: [upscale] xess_dll (a folder, or the file in it), then upscalers\xess\ next to the executable, next to the launcher
// (NXRT_LAUNCHER_DIR) and in the run folder, then the executable's folder.
//
// The device: XeSS names instance and device extensions and a chain of features (shaderStorageImageWriteWithoutFormat, mutableDescriptorType
// of VK_EXT_mutable_descriptor_type, and where supported shaderInt8 / shaderIntegerDotProduct). vk_device.cpp enables every core 1.0-1.3
// feature the GPU offers, so the device is fit for XeSS when the GPU supports every feature XeSS asks for, and the one extension structure among
// them (mutable descriptor types) is chained by xess_device_setup. A GPU that lacks any of them is made exactly as without XeSS (logged).
//
// Threads: XeSS is not thread-safe. The context is made, initialised and executed on the render thread (the first evaluation), and destroyed
// by xess_shutdown once the device is idle.
#include <windows.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "extras/temporal.h"
#include "nxrt/config.h"
#include "nxrt/extras.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"

namespace nxrt::extras {
namespace {

constexpr const char* kTag = "xess";

// ---- the SDK's ABI (xess.h, xess_vk.h; structures packed to 8, their natural layout on x64) ----
using XessResult = int;   // xess_result_t: 0 success, 1..2 warnings, negative errors
inline bool xess_ok(XessResult r) { return r >= 0; }
struct XessCtx;
using XessHandle = XessCtx*;
struct XessVersion { uint16_t major, minor, patch, reserved; };
struct Xess2d { uint32_t x, y; };
// xess_quality_settings_t: ULTRA_PERFORMANCE 100 ... ULTRA_QUALITY_PLUS 105, AA 106 (input and output of one size)
enum : int { kQualityFirst = 100, kQualityLastScaled = 105, kQualityAa = 106 };
// xess_init_flags_t
enum : uint32_t { kFlagInvertedDepth = 1u << 1, kFlagLdr = 1u << 6, kFlagAutoExposure = 1u << 8 };
enum : int { kLogDebug, kLogInfo, kLogWarning, kLogError };
struct XessView {   // xess_vk_image_view_info
    VkImageView imageView;
    VkImage image;
    VkImageSubresourceRange subresourceRange;
    VkFormat format;
    unsigned int width, height;
};
struct XessExec {   // xess_vk_execute_params_t
    XessView color, velocity, depth, exposure, responsive, output;
    float jitterX, jitterY, exposureScale;
    uint32_t resetHistory, inputWidth, inputHeight;
    Xess2d colorBase, mvBase, depthBase, responsiveBase, reserved0, outputBase;
};
struct XessInit {   // xess_vk_init_params_t
    Xess2d outputResolution;
    int qualitySetting;
    uint32_t initFlags, creationNodeMask, visibleNodeMask;
    VkDeviceMemory tempBufferHeap;
    uint64_t bufferHeapOffset;
    VkDeviceMemory tempTextureHeap;
    uint64_t textureHeapOffset;
    VkPipelineCache pipelineCache;
};
static_assert(sizeof(XessView) == 48, "xess_vk_image_view_info layout");
static_assert(sizeof(XessExec) == 6 * 48 + 24 + 6 * 8, "xess_vk_execute_params_t layout");
static_assert(sizeof(XessInit) == 64, "xess_vk_init_params_t layout");

using XessLogFn = void (*)(const char* message, int level);
using PfnGetVersion = XessResult (*)(XessVersion*);
using PfnInstExts = XessResult (*)(uint32_t*, const char* const**, uint32_t*);
using PfnDevExts = XessResult (*)(VkInstance, VkPhysicalDevice, uint32_t*, const char* const**);
using PfnDevFeats = XessResult (*)(VkInstance, VkPhysicalDevice, void**);
using PfnCreate = XessResult (*)(VkInstance, VkPhysicalDevice, VkDevice, XessHandle*);
using PfnBuild = XessResult (*)(XessHandle, VkPipelineCache, bool, uint32_t);
using PfnInit = XessResult (*)(XessHandle, const XessInit*);
using PfnExecute = XessResult (*)(XessHandle, VkCommandBuffer, const XessExec*);
using PfnDestroy = XessResult (*)(XessHandle);
using PfnSetLog = XessResult (*)(XessHandle, int, XessLogFn);
using PfnSetScale = XessResult (*)(XessHandle, float, float);
using PfnOptimal = XessResult (*)(XessHandle, const Xess2d*, int, Xess2d*, Xess2d*, Xess2d*);

// ---- the DLL ----
constexpr const wchar_t* kDll = L"libxess.dll";

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
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

struct Found {
    std::wstring dll;      // empty: not found
    std::string searched;  // the folders looked in (log)
};

const Found& find_dll() {
    static const Found v = [] {
        std::vector<std::wstring> dirs;
        auto add = [&dirs](const std::wstring& d) {
            if (d.empty()) return;
            for (const std::wstring& o : dirs)
                if (_wcsicmp(o.c_str(), d.c_str()) == 0) return;
            dirs.push_back(d);
        };
        const std::wstring set = widen(settings().upscale.xess_dll);   // a folder, or the DLL itself
        if (!set.empty()) add(is_dir(set) ? slash(set) : set.substr(0, set.find_last_of(L"\\/") + 1));
        wchar_t buf[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        std::wstring exe(buf, n);
        exe = exe.substr(0, exe.find_last_of(L"\\/") + 1);
        wchar_t ld[MAX_PATH];
        const DWORD ln = GetEnvironmentVariableW(L"NXRT_LAUNCHER_DIR", ld, MAX_PATH);
        wchar_t cwd[MAX_PATH];
        const std::wstring run = slash(std::wstring(cwd, GetCurrentDirectoryW(MAX_PATH, cwd)));
        add(exe + L"upscalers\\xess\\");
        if (ln && ln < MAX_PATH) add(slash(std::wstring(ld, ln)) + L"upscalers\\xess\\");
        add(run + L"upscalers\\xess\\");
        add(exe);
        Found f;
        for (const std::wstring& d : dirs) {
            f.searched += (f.searched.empty() ? "" : ", ") + narrow(d);
            if (f.dll.empty() && exists(d + kDll)) f.dll = d + kDll;
        }
        return f;
    }();
    return v;
}

// The setting that asks for XeSS: [dlss] upscaler = xess or auto (the game's DLSS calls), or [upscale] mode = xess (the scene upscaler).
bool wanted() {
    static const bool w = [] {
        std::string u = config_get_str("dlss", "upscaler", "taau");
        for (char& c : u) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return u == "xess" || (u == "auto" && !find_dll().dll.empty()) || settings().upscale.mode == UpscaleMode::Xess;
    }();
    return w;
}

struct Lib {
    HMODULE mod = nullptr;
    PfnGetVersion version = nullptr;
    PfnInstExts inst_exts = nullptr;
    PfnDevExts dev_exts = nullptr;
    PfnDevFeats dev_feats = nullptr;
    PfnCreate create = nullptr;
    PfnBuild build = nullptr;
    PfnInit init = nullptr;
    PfnExecute execute = nullptr;
    PfnDestroy destroy = nullptr;
    PfnSetLog set_log = nullptr;
    PfnSetScale jitter_scale = nullptr, velocity_scale = nullptr;
    PfnOptimal optimal = nullptr;   // xessGetOptimalInputResolution
    bool tried = false, loaded = false;
    bool instance_ok = false;   // the instance was made with XeSS's extensions
    bool device_ok = false;     // the device was made with XeSS's extensions and features
    std::string version_name = "XeSS";
} g;

bool lib_load() {
    if (g.tried) return g.loaded;
    g.tried = true;
    const Found& f = find_dll();
    if (f.dll.empty()) {
        NXLOG_W(kTag, "XeSS is not available: libxess.dll is in none of %s. Put it in upscalers\\xess next to the launcher (the release package has "
                      "it there) or name its folder in [upscale] xess_dll",
                f.searched.c_str());
        return false;
    }
    // LOAD_WITH_ALTERED_SEARCH_PATH: the DLL's own folder is searched for what it imports (an app-local Visual C++ runtime)
    g.mod = LoadLibraryExW(f.dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g.mod) {
        const DWORD e = GetLastError();
        NXLOG_E(kTag, "LoadLibrary of %s failed (error %lu)%s: XeSS off", narrow(f.dll).c_str(), e,
                e == ERROR_MOD_NOT_FOUND ? ": the Microsoft Visual C++ 2015-2022 Redistributable (x64, 14.40 or newer) is probably not installed"
                                         : "");
        return false;
    }
    auto sym = [](const char* n) { return reinterpret_cast<void*>(GetProcAddress(g.mod, n)); };
    g.version = reinterpret_cast<PfnGetVersion>(sym("xessGetVersion"));
    g.inst_exts = reinterpret_cast<PfnInstExts>(sym("xessVKGetRequiredInstanceExtensions"));
    g.dev_exts = reinterpret_cast<PfnDevExts>(sym("xessVKGetRequiredDeviceExtensions"));
    g.dev_feats = reinterpret_cast<PfnDevFeats>(sym("xessVKGetRequiredDeviceFeatures"));
    g.create = reinterpret_cast<PfnCreate>(sym("xessVKCreateContext"));
    g.build = reinterpret_cast<PfnBuild>(sym("xessVKBuildPipelines"));
    g.init = reinterpret_cast<PfnInit>(sym("xessVKInit"));
    g.execute = reinterpret_cast<PfnExecute>(sym("xessVKExecute"));
    g.destroy = reinterpret_cast<PfnDestroy>(sym("xessDestroyContext"));
    g.set_log = reinterpret_cast<PfnSetLog>(sym("xessSetLoggingCallback"));
    g.jitter_scale = reinterpret_cast<PfnSetScale>(sym("xessSetJitterScale"));
    g.velocity_scale = reinterpret_cast<PfnSetScale>(sym("xessSetVelocityScale"));
    g.optimal = reinterpret_cast<PfnOptimal>(sym("xessGetOptimalInputResolution"));
    if (!g.version || !g.inst_exts || !g.dev_exts || !g.dev_feats || !g.create || !g.init || !g.execute || !g.destroy || !g.velocity_scale) {
        NXLOG_E(kTag, "%s lacks the XeSS Vulkan entry points: XeSS off", narrow(f.dll).c_str());
        return false;
    }
    XessVersion v = {};
    g.version(&v);
    char name[48];
    std::snprintf(name, sizeof name, "XeSS %u.%u.%u", v.major, v.minor, v.patch);
    g.version_name = name;
    NXLOG_I(kTag, "%s loaded from %s", name, narrow(f.dll).c_str());
    return g.loaded = true;
}

bool listed(const std::vector<const char*>& list, const char* s) {
    for (const char* e : list)
        if (!std::strcmp(e, s)) return true;
    return false;
}

// The feature structures XeSS may ask for: the core ones (vk_device.cpp enables every core feature the GPU supports through the 1.1, 1.2 and
// 1.3 structures, robustness aside) and the one extension structure xess_device_setup chains. Anything else is a feature the runtime would
// not enable, so XeSS stays off.
struct FeatStruct {
    VkStructureType s;
    size_t size;
};
#define FS(stype, type) {VK_STRUCTURE_TYPE_##stype, sizeof(type)}
const FeatStruct kFeats[] = {
    FS(PHYSICAL_DEVICE_FEATURES_2, VkPhysicalDeviceFeatures2),
    FS(PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, VkPhysicalDeviceVulkan11Features),
    FS(PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, VkPhysicalDeviceVulkan12Features),
    FS(PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, VkPhysicalDeviceVulkan13Features),
    FS(PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, VkPhysicalDevice16BitStorageFeatures),
    FS(PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, VkPhysicalDevice8BitStorageFeatures),
    FS(PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, VkPhysicalDeviceShaderFloat16Int8Features),
    FS(PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, VkPhysicalDeviceShaderIntegerDotProductFeatures),
    FS(PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, VkPhysicalDeviceSubgroupSizeControlFeatures),
    FS(PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures),
    FS(PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, VkPhysicalDeviceDescriptorIndexingFeatures),
    FS(PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, VkPhysicalDeviceScalarBlockLayoutFeatures),
    FS(PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, VkPhysicalDeviceTimelineSemaphoreFeatures),
    FS(PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, VkPhysicalDeviceBufferDeviceAddressFeatures),
    FS(PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, VkPhysicalDeviceVulkanMemoryModelFeatures),
    FS(PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, VkPhysicalDeviceHostQueryResetFeatures),
    FS(PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, VkPhysicalDeviceSynchronization2Features),
    FS(PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, VkPhysicalDeviceDynamicRenderingFeatures),
    FS(PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, VkPhysicalDeviceMaintenance4Features),
    FS(PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures),
    FS(PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT),
};
#undef FS

// True when every VkBool32 XeSS sets in `want` is supported by the GPU (the structure queried on its own), robustness excepted: vk_device.cpp
// leaves robustBufferAccess / robustImageAccess off.
bool feature_supported(VkPhysicalDevice pd, const VkBaseOutStructure* want, size_t size) {
    alignas(16) uint8_t have[1024];
    if (size > sizeof have) return false;
    std::memset(have, 0, size);
    reinterpret_cast<VkBaseOutStructure*>(have)->sType = want->sType;
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    const bool is_f2 = want->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    if (!is_f2) f2.pNext = have;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    const auto* w = reinterpret_cast<const VkBool32*>(reinterpret_cast<const uint8_t*>(want) + sizeof(VkBaseOutStructure));
    const auto* h = is_f2 ? reinterpret_cast<const VkBool32*>(&f2.features) : reinterpret_cast<const VkBool32*>(have + sizeof(VkBaseOutStructure));
    const size_t n = (size - sizeof(VkBaseOutStructure)) / sizeof(VkBool32);
    for (size_t i = 0; i < n; ++i) {
        const bool robust = (is_f2 && i == 0) ||   // VkPhysicalDeviceFeatures::robustBufferAccess
                            (want->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES && i == 0);   // robustImageAccess
        if (w[i] && (!h[i] || robust)) {
            NXLOG_W(kTag, "the GPU lacks feature %zu of structure %d (or the runtime keeps it off), which XeSS asks for: XeSS off", i, static_cast<int>(want->sType));
            return false;
        }
    }
    return true;
}

VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT g_mutable = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};

void on_log(const char* message, int level) {
    if (level >= kLogError) NXLOG_E(kTag, "%s", message ? message : "");
    else if (level >= kLogWarning) NXLOG_W(kTag, "%s", message ? message : "");
}

class Xess final : public TemporalBackend {
public:
    const char* name() const override { return g.version_name.c_str(); }
    const char* last_failure() const override { return why_.c_str(); }

    explicit Xess(gfx::Device& dev) : dev_(&dev) {}

    bool evaluate(VkCommandBuffer cb, const TemporalFrame& f) override {
        if (failed_) return false;
        if (!context() || !init_size(f.render, f.out)) return false;
        if (f.render.width != vel_w_ || f.render.height != vel_h_) {
            // the motion vectors are in UV units of the render size; XeSS wants render pixels
            const XessResult r = g.velocity_scale(ctx_, static_cast<float>(f.render.width) * mv_sign_x_, static_cast<float>(f.render.height) * mv_sign_y_);
            if (!xess_ok(r)) return fail("xessSetVelocityScale", r);
            vel_w_ = f.render.width;
            vel_h_ = f.render.height;
        }
        XessExec p = {};
        p.color = view(f.color, f.render);
        p.velocity = view(f.motion, f.render);
        p.depth = view(f.depth, f.render);
        p.output = view(f.output, f.out);
        // XeSS's jitter is DLSS's: the sub-pixel offset of the projection in render pixels, +x right, +y down
        p.jitterX = f.jitter_px[0];
        p.jitterY = f.jitter_px[1];
        p.exposureScale = 1.0f;
        p.resetHistory = f.reset ? 1u : 0u;
        p.inputWidth = f.render.width;
        p.inputHeight = f.render.height;
        inputs_layout(cb, f, true);
        const XessResult r = g.execute(ctx_, cb, &p);
        inputs_layout(cb, f, false);
        if (!xess_ok(r)) return fail("xessVKExecute", r);
        ++evals_;
        return true;
    }

    void release() {
        if (ctx_) g.destroy(ctx_);
        ctx_ = nullptr;
    }

private:
    bool fail(const char* what, XessResult r) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s failed (%d): XeSS off", what, r);
        why_ = buf;
        NXLOG_E(kTag, "%s", buf);
        failed_ = true;
        return false;
    }

    // The context and its pipelines (the JIT of XeSS's kernels: a hitch the first time, logged with its length).
    bool context() {
        if (ctx_) return true;
        const auto t0 = std::chrono::steady_clock::now();
        XessResult r = g.create(dev_->instance(), dev_->physical(), dev_->device(), &ctx_);
        if (!xess_ok(r) || !ctx_) {
            ctx_ = nullptr;
            return fail("xessVKCreateContext", r);
        }
        if (g.set_log) g.set_log(ctx_, kLogWarning, &on_log);
        if (g.build && !xess_ok(r = g.build(ctx_, VK_NULL_HANDLE, true, flags_))) return fail("xessVKBuildPipelines", r);
        // test knobs: [dlss] xess_jitter_scale / xess_mv_sign (1 or -1)
        const float js = static_cast<float>(config_get_double("dlss", "xess_jitter_scale", 1.0));
        if (g.jitter_scale && js != 1.0f) g.jitter_scale(ctx_, js, js);
        const int ms = static_cast<int>(config_get_int("dlss", "xess_mv_sign", 0));   // bit 0 flips x, bit 1 flips y
        mv_sign_x_ = (ms & 1) ? -1.0f : 1.0f;
        mv_sign_y_ = (ms & 2) ? -1.0f : 1.0f;
        NXLOG_I(kTag, "context made and its pipelines built in %.0f ms",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        return true;
    }

    // The quality setting for input w x h and output ow x oh: Native AA at one size; else the setting whose input range (XeSS takes any
    // input between its minimum and maximum) holds the render size, the one whose optimal input is nearest when several do; without the
    // query, by the ratio and Intel's documented factors.
    int pick_quality(VkExtent2D in, VkExtent2D out) const {
        if (in.width >= out.width && in.height >= out.height) return kQualityAa;
        int best = 0;
        uint32_t best_d = ~0u;
        const Xess2d o = {out.width, out.height};
        for (int q = kQualityFirst; g.optimal && q <= kQualityLastScaled; ++q) {
            Xess2d opt = {}, lo = {}, hi = {};
            if (!xess_ok(g.optimal(ctx_, &o, q, &opt, &lo, &hi))) continue;
            if (in.width < lo.x || in.height < lo.y || in.width > hi.x || in.height > hi.y) continue;
            const uint32_t d = opt.x > in.width ? opt.x - in.width : in.width - opt.x;
            if (d < best_d) best = q, best_d = d;
        }
        if (best) return best;
        const double r = static_cast<double>(out.width) / in.width;
        return r >= 2.65 ? 100 : r >= 2.15 ? 101 : r >= 1.85 ? 102 : r >= 1.6 ? 103 : r >= 1.4 ? 104 : 105;
    }

    // xessVKInit for the output size and quality setting. A new size needs the GPU done with XeSS's previous work; XeSS may submit copies of
    // its own while it initialises, so it runs with the queue locked.
    bool init_size(VkExtent2D in, VkExtent2D out) {
        if (out.width == out_.width && out.height == out_.height && in.width == in_.width && in.height == in_.height) return true;
        const int q = pick_quality(in, out);
        if (out.width == out_.width && out.height == out_.height && q == quality_) {   // another input in the same range
            in_ = in;
            return true;
        }
        const auto t0 = std::chrono::steady_clock::now();
        XessInit ip = {};
        ip.outputResolution = {out.width, out.height};
        ip.qualitySetting = q;
        ip.initFlags = flags_;
        if (out_.width) dev_->wait_idle();
        XessResult r;
        {
            std::lock_guard<std::mutex> lk(dev_->queue_mutex());
            r = g.init(ctx_, &ip);
        }
        if (!xess_ok(r)) return fail("xessVKInit", r);
        in_ = in;
        out_ = out;
        quality_ = q;
        vel_w_ = vel_h_ = 0;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (q == kQualityAa) NXLOG_I(kTag, "%s: native AA at %ux%u, HDR, inverted depth, auto exposure (initialised in %.0f ms)", name(), out.width, out.height, ms);
        else
            NXLOG_I(kTag, "%s: %ux%u -> %ux%u (%.2fx), quality setting %d%s, HDR, inverted depth, auto exposure (initialised in %.0f ms)", name(), in.width,
                    in.height, out.width, out.height, static_cast<double>(out.width) / in.width, q, g.optimal ? "" : " by the ratio", ms);
        return true;
    }

    static XessView view(const TemporalImage& t, VkExtent2D size) {
        XessView v = {};
        v.imageView = t.view;
        v.image = t.image;
        v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        v.format = t.format;
        v.width = size.width;
        v.height = size.height;
        return v;
    }

    // XeSS reads its inputs in SHADER_READ_ONLY_OPTIMAL and writes its output in GENERAL (the temporal pass keeps every image in GENERAL), and
    // does no synchronisation of its own: the inputs go there and back around the execution, ordered against everything before and after.
    static void inputs_layout(VkCommandBuffer cb, const TemporalFrame& f, bool to_read) {
        const VkImageLayout a = to_read ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const VkImageLayout b = to_read ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        const VkImage imgs[3] = {f.color.image, f.depth.image, f.motion.image};
        VkImageMemoryBarrier2 ib[3];
        for (int i = 0; i < 3; ++i) {
            ib[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            ib[i].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            ib[i].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            ib[i].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            ib[i].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            ib[i].oldLayout = a;
            ib[i].newLayout = b;
            ib[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib[i].image = imgs[i];
            ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        di.imageMemoryBarrierCount = 3;
        di.pImageMemoryBarriers = ib;
        vkCmdPipelineBarrier2(cb, &di);
    }

    gfx::Device* dev_;
    XessHandle ctx_ = nullptr;
    // HDR colour (the game's scene light, pre-exposed) with XeSS's auto exposure; reversed-Z depth, as the FSR 3 and DLSS hosts take it
    uint32_t flags_ = kFlagInvertedDepth | kFlagAutoExposure;
    VkExtent2D in_ = {}, out_ = {};
    int quality_ = 0;
    uint32_t vel_w_ = 0, vel_h_ = 0;
    float mv_sign_x_ = 1.0f, mv_sign_y_ = 1.0f;
    uint64_t evals_ = 0;
    bool failed_ = false;
    std::string why_;
};

Xess* g_inst = nullptr;

}  // namespace

bool temporal_xess_available() { return !find_dll().dll.empty(); }

bool temporal_xess_ready() { return g.loaded && g.instance_ok && g.device_ok; }

void xess_instance_setup(const std::vector<VkExtensionProperties>& have, std::vector<const char*>& exts) {
    if (!wanted() || !lib_load()) return;
    uint32_t n = 0, api = 0;
    const char* const* list = nullptr;
    if (!xess_ok(g.inst_exts(&n, &list, &api))) {
        NXLOG_W(kTag, "XeSS gave no instance extension list: XeSS off");
        return;
    }
    if (api > VK_API_VERSION_1_3) {
        NXLOG_W(kTag, "XeSS asks for Vulkan %u.%u; the runtime makes a 1.3 instance: XeSS off", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api));
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        bool ok = false;
        for (const VkExtensionProperties& e : have) ok = ok || !std::strcmp(e.extensionName, list[i]);
        if (!ok) {
            NXLOG_W(kTag, "the Vulkan loader lacks instance extension %s, which XeSS asks for: XeSS off", list[i]);
            return;
        }
    }
    for (uint32_t i = 0; i < n; ++i)
        if (!listed(exts, list[i])) exts.push_back(list[i]);
    g.instance_ok = true;
}

void xess_device_setup(VkInstance instance, VkPhysicalDevice physical, const std::vector<VkExtensionProperties>& have, std::vector<const char*>& exts,
                       void** chain) {
    if (!g.instance_ok) return;
    uint32_t n = 0;
    const char* const* list = nullptr;
    if (!xess_ok(g.dev_exts(instance, physical, &n, &list))) {
        NXLOG_W(kTag, "XeSS gave no device extension list for this GPU: XeSS off");
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        bool ok = false;
        for (const VkExtensionProperties& e : have) ok = ok || !std::strcmp(e.extensionName, list[i]);
        if (!ok) {
            NXLOG_W(kTag, "the GPU lacks %s, which XeSS asks for: XeSS off", list[i]);
            return;
        }
    }
    void* feats = nullptr;
    if (!xess_ok(g.dev_feats(instance, physical, &feats))) {
        NXLOG_W(kTag, "XeSS gave no device feature list for this GPU: XeSS off");
        return;
    }
    bool want_mutable = false;
    for (const auto* s = static_cast<const VkBaseOutStructure*>(feats); s; s = s->pNext) {
        size_t k = 0;
        while (k < std::size(kFeats) && kFeats[k].s != s->sType) ++k;
        if (k == std::size(kFeats)) {
            NXLOG_W(kTag, "XeSS asks for features in structure %d, which the runtime does not enable: XeSS off", static_cast<int>(s->sType));
            return;
        }
        if (!feature_supported(physical, s, kFeats[k].size)) return;
        want_mutable = want_mutable || s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT;
    }
    std::string names;
    for (uint32_t i = 0; i < n; ++i) {
        if (!listed(exts, list[i])) exts.push_back(list[i]);
        names += (names.empty() ? "" : ", ") + std::string(list[i]);
    }
    if (want_mutable) {   // filled by the availability query of vk_device.cpp and enabled with the rest of its chain
        g_mutable.pNext = *chain;
        *chain = &g_mutable;
    }
    g.device_ok = true;
    NXLOG_I(kTag, "the device is made with what XeSS needs (extensions: %s)", names.empty() ? "none" : names.c_str());
}

void xess_shutdown() {
    if (g_inst) g_inst->release();
}

TemporalBackend* temporal_xess(void* gfx_device) {
    static bool told = false;
    if (!g_inst && gfx_device && temporal_xess_ready()) g_inst = new Xess(*static_cast<gfx::Device*>(gfx_device));
    if (!g_inst && !told) {
        told = true;
        NXLOG_W(kTag, "XeSS cannot run: %s", !g.tried ? "it was not chosen when the game started (applies at the next start)"
                                           : !g.loaded ? "libxess.dll was not loaded (see above)"
                                                       : "the Vulkan device lacks what it needs (see above)");
    }
    return g_inst;
}

}  // namespace nxrt::extras
