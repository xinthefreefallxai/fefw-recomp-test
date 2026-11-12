// SPDX-License-Identifier: GPL-3.0-or-later
// Vulkan device layer (declarations in include/nxrt/gfx.h): instance + validation, GPU choice, logical device, VMA, image-format
// checks, buffers, one-shot submits.
// Ideas taken from SuperMarioWonderRecomp (MIT) docs/nvn/DEBUG-2.md #69 (ASTC on NVIDIA desktop) and #70 (iGPU listed first):
// the checks and the GPU rule are written from scratch, see docs/S4b.md.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>

#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/memguard.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"
#include "nxrt/platform.h"
#include "vk_gauges.h"
#include <unordered_map>
#include "extras/framegen.h"
#include "extras/framegen_ffx.h"
#include "extras/temporal.h"

#include <vk_mem_alloc.h>

namespace nxrt {

// Count actual driver allocations, including VMA blocks and the overlay backend.
namespace {
PFN_vkAllocateMemory driver_allocate_memory;
PFN_vkFreeMemory driver_free_memory;
std::mutex memory_count_mu;
std::unordered_map<VkDeviceMemory, VkDeviceSize> memory_sizes;
VKAPI_ATTR VkResult VKAPI_CALL counted_allocate_memory(VkDevice d, const VkMemoryAllocateInfo* info,
                                                      const VkAllocationCallbacks* cb, VkDeviceMemory* out) {
    *out = VK_NULL_HANDLE;
    static const bool inject = config_get_bool("diag", "fail_small_vk_allocation", false);
    static std::atomic<bool> injected{false};
    const bool fail = inject && !info->pNext && info->allocationSize <= 65536 && !injected.exchange(true);
    if (fail) NXLOG_W("vk.memory", "diagnostic small allocation failure injected");
    const VkResult r = fail ? VK_ERROR_OUT_OF_DEVICE_MEMORY : driver_allocate_memory(d, info, cb, out);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(memory_count_mu);
        memory_sizes[*out] = info->allocationSize;
        vk_gauge_memory_allocs().add(1);
        vk_gauge_memory_bytes().add(static_cast<int64_t>(info->allocationSize));
    } else {
        *out = VK_NULL_HANDLE;
        // At most one line a second, with the failures since the last line: under memory pressure VMA retries every allocation and the
        // game streams on, so a failure can repeat thousands of times a second and the log writes alone stalled the render thread.
        static std::atomic<uint64_t> failures{0}, reported{0}, last_ms{0};
        const uint64_t n = failures.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::steady_clock::now().time_since_epoch()).count()) + 1000;   // never 0
        uint64_t last = last_ms.load(std::memory_order_relaxed);
        if ((last == 0 || now - last >= 1000) && last_ms.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
            const uint64_t before = reported.exchange(n, std::memory_order_relaxed);
            NXLOG_E("vk.memory", "allocation failed (%d), bytes %llu, live %lld (%llu failures since the last line, %llu in all)", int(r),
                    (unsigned long long)info->allocationSize, (long long)vk_gauge_memory_allocs().get(), (unsigned long long)(n - before),
                    (unsigned long long)n);
        }
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL counted_free_memory(VkDevice d, VkDeviceMemory memory, const VkAllocationCallbacks* cb) {
    {
        std::lock_guard<std::mutex> lock(memory_count_mu);
        auto i = memory_sizes.find(memory);
        if (i != memory_sizes.end()) {
            vk_gauge_memory_bytes().sub(static_cast<int64_t>(i->second));
            vk_gauge_memory_allocs().sub(1);
            memory_sizes.erase(i);
        }
    }
    driver_free_memory(d, memory, cb);
}
}

// ---- live-object gauges (declared in vk_gauges.h) -------------------------------------------------------------------------------
nxrt::GaugeCounter& vk_gauge_images() { static nxrt::GaugeCounter g("vk.images"); return g; }
nxrt::GaugeCounter& vk_gauge_image_bytes() { static nxrt::GaugeCounter g("vk.images.bytes"); return g; }
nxrt::GaugeCounter& vk_gauge_image_views() { static nxrt::GaugeCounter g("vk.image_views"); return g; }
nxrt::GaugeCounter& vk_gauge_buffers() { static nxrt::GaugeCounter g("vk.buffers"); return g; }
nxrt::GaugeCounter& vk_gauge_buffer_bytes() { static nxrt::GaugeCounter g("vk.buffers.bytes"); return g; }
nxrt::GaugeCounter& vk_gauge_samplers() { static nxrt::GaugeCounter g("vk.samplers"); return g; }
nxrt::GaugeCounter& vk_gauge_shader_modules() { static nxrt::GaugeCounter g("vk.shader_modules"); return g; }
nxrt::GaugeCounter& vk_gauge_pipelines() { static nxrt::GaugeCounter g("vk.pipelines"); return g; }
nxrt::GaugeCounter& vk_gauge_pipeline_layouts() { static nxrt::GaugeCounter g("vk.pipeline_layouts"); return g; }
nxrt::GaugeCounter& vk_gauge_descriptor_set_layouts() { static nxrt::GaugeCounter g("vk.descriptor_set_layouts"); return g; }
nxrt::GaugeCounter& vk_gauge_descriptor_pools() { static nxrt::GaugeCounter g("vk.descriptor_pools"); return g; }
nxrt::GaugeCounter& vk_gauge_descriptor_sets() { static nxrt::GaugeCounter g("vk.descriptor_sets"); return g; }
nxrt::GaugeCounter& vk_gauge_command_pools() { static nxrt::GaugeCounter g("vk.command_pools"); return g; }
nxrt::GaugeCounter& vk_gauge_command_buffers() { static nxrt::GaugeCounter g("vk.command_buffers"); return g; }
nxrt::GaugeCounter& vk_gauge_fences() { static nxrt::GaugeCounter g("vk.fences"); return g; }
nxrt::GaugeCounter& vk_gauge_semaphores() { static nxrt::GaugeCounter g("vk.semaphores"); return g; }
nxrt::GaugeCounter& vk_gauge_query_pools() { static nxrt::GaugeCounter g("vk.query_pools"); return g; }
nxrt::GaugeCounter& vk_gauge_swapchains() { static nxrt::GaugeCounter g("vk.swapchains"); return g; }
nxrt::GaugeCounter& vk_gauge_memory_allocs() { static nxrt::GaugeCounter g("vk.memory_allocs"); return g; }
nxrt::GaugeCounter& vk_gauge_memory_bytes() { static nxrt::GaugeCounter g("vk.memory_bytes"); return g; }

}  // namespace nxrt

namespace nxrt::gfx {

namespace {

constexpr const char* kTag = "gfx";
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const char* type_name(int t) {
    static const char* const names[] = {"other", "integrated", "discrete", "virtual", "cpu"};
    return t >= 0 && t <= 4 ? names[t] : "?";
}

// Higher is better: discrete, integrated, virtual, other, cpu.
int type_rank(int t) {
    switch (t) {
        case 2: return 4;
        case 1: return 3;
        case 3: return 2;
        case 0: return 1;
        default: return 0;
    }
}

size_t best_of_rank(const std::vector<GpuCandidate>& g) {
    size_t best = 0;
    for (size_t i = 1; i < g.size(); ++i) {
        const int ri = type_rank(g[i].type), rb = type_rank(g[best].type);
        if (ri > rb || (ri == rb && g[i].vram_bytes > g[best].vram_bytes)) best = i;
    }
    return best;
}

size_t first_of_type(const std::vector<GpuCandidate>& g, int type) {
    size_t best = SIZE_MAX;
    for (size_t i = 0; i < g.size(); ++i)
        if (g[i].type == type && (best == SIZE_MAX || g[i].vram_bytes > g[best].vram_bytes)) best = i;
    return best;
}

bool is_bc(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }
bool is_etc2(VkFormat f) { return f >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && f <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK; }
bool is_astc(VkFormat f) {
    return (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) ||
           (f >= VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK && f <= VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK);
}

VkImageAspectFlags aspect_of(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

}  // namespace

// ---- GPU rule ---------------------------------------------------------------------------------------------------------
size_t pick_gpu(const std::vector<GpuCandidate>& gpus, const char* want, bool* matched) {
    if (matched) *matched = true;
    if (gpus.empty()) return 0;
    const size_t fallback = best_of_rank(gpus);
    const std::string w = want ? lower(want) : "";
    if (w.empty()) return fallback;
    size_t k = SIZE_MAX;
    if (w == "discrete") {
        k = first_of_type(gpus, 2);
    } else if (w == "integrated") {
        k = first_of_type(gpus, 1);
    } else if (w.size() <= 2 && std::isdigit(static_cast<unsigned char>(w[0]))) {   // an index; "3080" is a name part
        char* end = nullptr;
        const unsigned long n = std::strtoul(w.c_str(), &end, 10);
        if (end && !*end && n < gpus.size()) k = static_cast<size_t>(n);
    } else {
        for (size_t i = 0; i < gpus.size() && k == SIZE_MAX; ++i)
            if (lower(gpus[i].name).find(w) != std::string::npos) k = i;
    }
    if (k != SIZE_MAX) return k;
    if (matched) *matched = false;
    return fallback;
}

// ---- private state ----------------------------------------------------------------------------------------------------
struct Device::Impl {
    // validation counters (written from any thread by the layer callback)
    std::atomic<uint64_t> errors{0}, warnings{0}, infos{0}, verbose{0};
    std::mutex first_error_mu;
    std::string first_error;

    // image-format cache
    struct Key {
        VkFormat format; VkImageType type; VkImageTiling tiling; VkImageUsageFlags usage; VkImageCreateFlags flags;
        bool operator<(const Key& o) const {
            return std::tie(format, type, tiling, usage, flags) < std::tie(o.format, o.type, o.tiling, o.usage, o.flags);
        }
    };
    struct Val { VkResult result; VkImageFormatProperties props; };
    std::mutex fmt_mu;
    std::map<Key, Val> fmt_cache;
    std::atomic<uint64_t> fmt_checks{0}, fmt_hits{0}, fmt_rejected{0};

    // one-shot submits
    std::mutex once_mu;
    VkCommandPool once_pool = VK_NULL_HANDLE;
    VkFence once_fence = VK_NULL_HANDLE;
};

namespace {
VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* impl = static_cast<Device::Impl*>(user);
    const char* id = data && data->pMessageIdName ? data->pMessageIdName : "";
    const char* msg = data && data->pMessage ? data->pMessage : "";
    if (std::strncmp(id, "Loader Message", 14) == 0) {   // the loader's own chatter (duplicate layer manifests of overlay tools, ...)
        impl->infos.fetch_add(1);
        NXLOG_D("vulkan", "%s: %s", id, msg);
        return VK_FALSE;
    }
    // A known false positive of the validation layers: for a geometry shader they add the input (gl_in) and the output ClipDistance /
    // CullDistance arrays together, while the limit applies to each separately (KhronosGroup/Vulkan-ValidationLayers#13414). The
    // translated geometry shaders declare 7 + 1 per block, the 8 the device allows.
    if (std::strcmp(id, "VUID-RuntimeSpirv-maxCombinedClipAndCullDistances-00710") == 0 && std::strstr(msg, "VK_SHADER_STAGE_GEOMETRY_BIT")) {
        impl->infos.fetch_add(1);
        NXLOG_D("vulkan", "filtered (validation layers #13414): %s", msg);
        return VK_FALSE;
    }
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        if (impl->errors.fetch_add(1) == 0) {
            std::lock_guard<std::mutex> lk(impl->first_error_mu);
            impl->first_error = std::string(id) + ": " + msg;
        }
        NXLOG_E("vulkan", "%s: %s", id, msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        impl->warnings.fetch_add(1);
        NXLOG_W("vulkan", "%s: %s", id, msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        impl->infos.fetch_add(1);
        NXLOG_D("vulkan", "%s: %s", id, msg);
    } else {
        impl->verbose.fetch_add(1);
    }
    return VK_FALSE;
}

// ---- VMA gauges (vma.*): read on the sampler thread every 10 s. Registered after vmaCreateAllocator, unregistered in ~Device before
// the allocator is destroyed. vmaCalculateStatistics walks the whole allocator, so one result is shared for 1 s (the four
// statistics gauges are read back to back in one sampler pass).
struct VmaStatsCache {
    std::mutex m;
    std::chrono::steady_clock::time_point at{};
    bool valid = false;
    VmaTotalStatistics st{};
};

VmaTotalStatistics vma_totals(VmaAllocator a, VmaStatsCache& c) {
    std::lock_guard<std::mutex> lk(c.m);
    const auto now = std::chrono::steady_clock::now();
    if (!c.valid || now - c.at > std::chrono::seconds(1)) {
        vmaCalculateStatistics(a, &c.st);
        c.at = now;
        c.valid = true;
    }
    return c.st;
}

// The device-local heap with the most memory (the one the budget gauges describe).
uint32_t device_local_heap(VmaAllocator a) {
    const VkPhysicalDeviceMemoryProperties* mp = nullptr;
    vmaGetMemoryProperties(a, &mp);
    uint32_t best = 0;
    VkDeviceSize best_size = 0;
    for (uint32_t i = 0; mp && i < mp->memoryHeapCount; ++i) {
        if ((mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp->memoryHeaps[i].size > best_size) {
            best = i;
            best_size = mp->memoryHeaps[i].size;
        }
    }
    return best;
}

std::mutex g_guard_mu;                 // the memguard thread reads these through gpu_bytes_for_guard; the device destructor clears them
VmaAllocator g_guard_vma = nullptr;
int g_guard_heap = -1;                 // -1: nothing to subtract
uint64_t gpu_bytes_for_guard() {      // the device's dedicated memory in use by this process, for the memory guard (nxrt/memguard.h)
    // Held across the query: unregister_vma_gauges takes the same lock before the allocator is destroyed.
    std::lock_guard<std::mutex> lk(g_guard_mu);
    if (!g_guard_vma || g_guard_heap < 0) return 0;
    VmaBudget b[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(g_guard_vma, b);
    return static_cast<uint64_t>(b[g_guard_heap].usage);
}

bool g_budget_exact = false;           // VK_EXT_memory_budget is on: heap usage and budget are the driver's figures
int g_video_heap = -1;                 // the largest device-local heap (guarded by g_guard_mu, as the allocator pointer)
void video_for_guard(nxrt::MemGuardVideo* out) {
    std::lock_guard<std::mutex> lk(g_guard_mu);
    if (!g_guard_vma || g_video_heap < 0) return;
    VmaBudget b[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(g_guard_vma, b);
    const VkPhysicalDeviceMemoryProperties* mp = nullptr;
    vmaGetMemoryProperties(g_guard_vma, &mp);
    out->usage = b[g_video_heap].usage;
    out->budget = b[g_video_heap].budget;
    out->heap = mp ? mp->memoryHeaps[g_video_heap].size : 0;
    out->driver_bytes = static_cast<uint64_t>(std::max<int64_t>(0, nxrt::vk_gauge_memory_bytes().get()));
    out->exact = g_budget_exact;
}

void register_vma_gauges(VmaAllocator a, VkPhysicalDeviceType type) {
    const uint32_t heap = device_local_heap(a);
    {
        const VkPhysicalDeviceMemoryProperties* mp = nullptr;
        vmaGetMemoryProperties(a, &mp);
        std::lock_guard<std::mutex> lk(g_guard_mu);
        g_guard_heap = mp ? vk_guard_discount_heap(*mp, type) : -1;
        g_guard_vma = a;
        g_video_heap = static_cast<int>(heap);
    }
    nxrt::memguard_set_discount(&gpu_bytes_for_guard);
    nxrt::memguard_set_video(&video_for_guard);
    auto cache = std::make_shared<VmaStatsCache>();
    nxrt::gauge_register("vma.device_usage_mb", [a, heap] {
        VmaBudget b[VK_MAX_MEMORY_HEAPS] = {};
        vmaGetHeapBudgets(a, b);
        return static_cast<int64_t>(b[heap].usage >> 20);
    });
    nxrt::gauge_register("vma.device_budget_mb", [a, heap] {
        VmaBudget b[VK_MAX_MEMORY_HEAPS] = {};
        vmaGetHeapBudgets(a, b);
        return static_cast<int64_t>(b[heap].budget >> 20);
    });
    nxrt::gauge_register("vma.alloc_count", [a, cache] { return static_cast<int64_t>(vma_totals(a, *cache).total.statistics.allocationCount); });
    nxrt::gauge_register("vma.alloc_mb", [a, cache] { return static_cast<int64_t>(vma_totals(a, *cache).total.statistics.allocationBytes >> 20); });
    nxrt::gauge_register("vma.block_count", [a, cache] { return static_cast<int64_t>(vma_totals(a, *cache).total.statistics.blockCount); });
    nxrt::gauge_register("vma.block_mb", [a, cache] { return static_cast<int64_t>(vma_totals(a, *cache).total.statistics.blockBytes >> 20); });
}

void unregister_vma_gauges() {
    nxrt::memguard_set_discount(nullptr);
    nxrt::memguard_set_video(nullptr);
    {
        std::lock_guard<std::mutex> lk(g_guard_mu);   // waits for a guard read in flight; later reads see a null allocator
        g_guard_vma = nullptr;
        g_video_heap = -1;
        g_guard_heap = -1;
    }
    const char* const names[] = {"vma.device_usage_mb", "vma.device_budget_mb", "vma.alloc_count", "vma.alloc_mb", "vma.block_count", "vma.block_mb"};
    for (const char* n : names) nxrt::gauge_unregister(n);
}

}  // namespace

// The heap whose usage the memory guard subtracts from the process commit, or -1. WDDM charges the video memory a process allocates to its
// commit, but only dedicated memory: on a discrete GPU that is the largest device-local heap. On an integrated GPU the device-local heap is
// system RAM (host-visible); subtracting its usage would hide real host memory from the guard, so nothing is subtracted.
int vk_guard_discount_heap(const VkPhysicalDeviceMemoryProperties& mp, VkPhysicalDeviceType type) {
    int best = -1;
    VkDeviceSize best_size = 0;
    for (uint32_t i = 0; i < mp.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; ++i) {
        if (!(mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
        bool host_visible = false;
        for (uint32_t t = 0; t < mp.memoryTypeCount; ++t)
            if (mp.memoryTypes[t].heapIndex == i && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) host_visible = true;
        if (type != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && host_visible) continue;
        if (mp.memoryHeaps[i].size > best_size) {
            best = static_cast<int>(i);
            best_size = mp.memoryHeaps[i].size;
        }
    }
    return best;
}

uint64_t vk_guard_gpu_bytes() { return gpu_bytes_for_guard(); }

// ---- creation ---------------------------------------------------------------------------------------------------------
std::unique_ptr<Device> Device::create(const DeviceDesc& desc, std::string* error) {
    std::unique_ptr<Device> d(new Device());
    d->impl_ = new Impl();
    std::string err;
    if (!d->init(desc, &err)) {
        NXLOG_E(kTag, "device creation failed: %s", err.c_str());
        if (error) *error = err;
        return nullptr;
    }
    return d;
}

bool Device::init(const DeviceDesc& desc, std::string* error) {
    auto fail = [&](const std::string& m) {
        *error = m;
        return false;
    };
#ifdef NDEBUG
    constexpr bool kDebugBuild = false;
#else
    constexpr bool kDebugBuild = true;
#endif
    const bool want_validation = desc.validation >= 0 ? desc.validation != 0 : config_get_bool("gfx", "validation", kDebugBuild);
    const bool headless = desc.headless >= 0 ? desc.headless != 0 : config_get_bool("gfx", "headless", false);
    std::string gpu_want = !desc.gpu.empty() ? desc.gpu : config_get_str("gfx", "gpu", "");

    // DLSS / DLAA: Streamline must see the instance, device and swapchain calls, so the loader functions come from sl.interposer.dll
    const PFN_vkGetInstanceProcAddr sl_gipa = extras::streamline_preinit();
    if (sl_gipa) volkInitializeCustom(sl_gipa);
    else if (volkInitialize() != VK_SUCCESS) return fail("no Vulkan loader found (vulkan-1.dll)");
    uint32_t inst_version = 0;
    vkEnumerateInstanceVersion(&inst_version);
    if (inst_version < VK_API_VERSION_1_3) return fail("Vulkan 1.3 is required; the loader offers less");

    if (!headless && !platform::has_window()) return fail("no platform window: create one (platform::create_window) or run headless");

    // layers and extensions
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    auto has_layer = [&](const char* name) {
        for (auto& l : layers) if (!std::strcmp(l.layerName, name)) return true;
        return false;
    };
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> iexts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, iexts.data());
    auto has_iext = [&](const char* name) {
        for (auto& e : iexts) if (!std::strcmp(e.extensionName, name)) return true;
        return false;
    };

    std::vector<const char*> inst_layers, inst_exts;
    if (want_validation) {
        if (has_layer(kValidationLayer)) {
            inst_layers.push_back(kValidationLayer);
            validation_ = true;
        } else {
            NXLOG_W(kTag, "validation requested but %s is not installed (Vulkan SDK): continuing without", kValidationLayer);
        }
    }
    if (!headless) {
        uint32_t cnt = 0;
        const char* const* pe = platform::vulkan_instance_extensions(&cnt);
        for (uint32_t i = 0; pe && i < cnt; ++i) inst_exts.push_back(pe[i]);
    }
    const bool x_colorspace = !headless && has_iext(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);   // HDR10 / scRGB entries in the surface format list
    if (x_colorspace) inst_exts.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
#ifdef _WIN32
    // needed by VK_EXT_full_screen_exclusive (below), which keeps borderless fullscreen from being turned into exclusive mode
    const bool x_surfcaps2 = !headless && has_iext(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    if (x_surfcaps2) inst_exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
#endif
    if (has_iext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        // the extension is on whenever it is there (command buffer labels for the game's debug groups, RenderDoc); the messenger only with validation
        inst_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        debug_utils_ = validation_;
        features_.debug_labels = true;
    }
    VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT vfeat = {VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    vfeat.enabledValidationFeatureCount = 1;
    vfeat.pEnabledValidationFeatures = enables;
    const bool vfeat_ok = validation_ && has_iext(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    if (vfeat_ok) inst_exts.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);

    VkDebugUtilsMessengerCreateInfoEXT dbg = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbg.pfnUserCallback = debug_callback;
    dbg.pUserData = impl_;
    if (vfeat_ok) vfeat.pNext = &dbg;   // messages from instance creation and destruction too

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = desc.app_name.c_str();
    app.applicationVersion = 1;
    app.pEngineName = "nxrt";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = static_cast<uint32_t>(inst_layers.size());
    ici.ppEnabledLayerNames = inst_layers.data();
    ici.enabledExtensionCount = static_cast<uint32_t>(inst_exts.size());
    ici.ppEnabledExtensionNames = inst_exts.data();
    ici.pNext = vfeat_ok ? static_cast<void*>(&vfeat) : (debug_utils_ ? static_cast<void*>(&dbg) : nullptr);
    VkResult r = vkCreateInstance(&ici, nullptr, &instance_);
    if (r != VK_SUCCESS) return fail("vkCreateInstance failed (" + std::to_string(r) + ")");
    volkLoadInstance(instance_);
    features_.swapchain_colorspace = x_colorspace;
    if (debug_utils_) vkCreateDebugUtilsMessengerEXT(instance_, &dbg, nullptr, &messenger_);

    if (!headless) {
#ifdef _WIN32
        if (sl_gipa) {
            // SDL loads vulkan-1.dll independently. DLSS-G must see the HWND/surface association.
            VkWin32SurfaceCreateInfoKHR ci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
            ci.hwnd = static_cast<HWND>(platform::native_hwnd());
            ci.hinstance = static_cast<HINSTANCE>(platform::native_hinstance());
            if (!ci.hwnd || !ci.hinstance || !vkCreateWin32SurfaceKHR)
                return fail("missing Win32 window properties or interposed surface entry point");
            r = vkCreateWin32SurfaceKHR(instance_, &ci, nullptr, &surface_);
            if (r != VK_SUCCESS) return fail("interposed vkCreateWin32SurfaceKHR failed (" + std::to_string(r) + ")");
            NXLOG_I(kTag, "window surface created through Streamline");
        } else
#endif
        {
            uint64_t s = 0;
            if (!platform::create_vulkan_surface(instance_, &s)) return fail("could not create the Vulkan surface for the window");
            surface_ = reinterpret_cast<VkSurfaceKHR>(s);
            owns_platform_surface_ = true;
        }
    }

    // physical devices
    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(instance_, &ndev, nullptr);
    if (!ndev) return fail("no Vulkan physical device");
    std::vector<VkPhysicalDevice> devs(ndev);
    vkEnumeratePhysicalDevices(instance_, &ndev, devs.data());

    struct Cand { VkPhysicalDevice pd; uint32_t qfam; bool eligible; std::string why; };
    std::vector<Cand> cands;
    for (uint32_t i = 0; i < ndev; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        GpuInfo gi;
        gi.index = i;
        gi.name = p.deviceName;
        gi.vendor_id = p.vendorID;
        gi.device_id = p.deviceID;
        gi.type = static_cast<int>(p.deviceType);
        gi.api_version = p.apiVersion;
        gi.driver_version = p.driverVersion;
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(devs[i], &mp);
        for (uint32_t h = 0; h < mp.memoryHeapCount; ++h)
            if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) gi.vram_bytes = std::max<uint64_t>(gi.vram_bytes, mp.memoryHeaps[h].size);

        Cand c{devs[i], UINT32_MAX, true, ""};
        if (p.apiVersion < VK_API_VERSION_1_3) { c.eligible = false; c.why = "Vulkan 1.3 not supported"; }
        if (c.eligible) {
            VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
            vkGetPhysicalDeviceFeatures2(devs[i], &f2);
            if (!f13.dynamicRendering || !f13.synchronization2) { c.eligible = false; c.why = "dynamic rendering / synchronization2 missing"; }
        }
        if (c.eligible && !headless) {
            uint32_t ne = 0;
            vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &ne, nullptr);
            std::vector<VkExtensionProperties> ex(ne);
            vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &ne, ex.data());
            bool sw = false;
            for (auto& e : ex) if (!std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) sw = true;
            if (!sw) { c.eligible = false; c.why = "no VK_KHR_swapchain"; }
        }
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, qf.data());
        for (uint32_t q = 0; q < nq && c.eligible && c.qfam == UINT32_MAX; ++q) {
            if (!(qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !(qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
            if (!headless) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, surface_, &present);
                if (!present) continue;
            }
            c.qfam = q;
        }
        if (c.eligible && c.qfam == UINT32_MAX) { c.eligible = false; c.why = headless ? "no graphics+compute queue" : "no queue family that draws and presents"; }
        gi.can_present = c.eligible;
        gpus_.push_back(gi);
        cands.push_back(c);
        NXLOG_I(kTag, "GPU %u: %s (%s, Vulkan %u.%u.%u, driver 0x%08x, %.1f GiB)%s%s", i, p.deviceName, type_name(gi.type),
                VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
                p.driverVersion, static_cast<double>(gi.vram_bytes) / (1024.0 * 1024.0 * 1024.0),
                c.eligible ? "" : " NOT USABLE: ", c.eligible ? "" : c.why.c_str());
    }

    // The rule runs over the usable devices only; a numeric override is an index into the list logged above.
    std::vector<GpuCandidate> usable;
    std::vector<size_t> usable_to_all;
    for (size_t i = 0; i < gpus_.size(); ++i) {
        if (!cands[i].eligible) continue;
        usable.push_back({gpus_[i].name, gpus_[i].type, gpus_[i].vram_bytes});
        usable_to_all.push_back(i);
    }
    if (usable.empty()) return fail("no usable Vulkan 1.3 GPU");
    std::string want = lower(gpu_want);
    bool numeric = !want.empty() && want.size() <= 2 && std::isdigit(static_cast<unsigned char>(want[0]));
    if (numeric) {
        const size_t idx = static_cast<size_t>(std::strtoul(want.c_str(), nullptr, 10));
        auto it = std::find(usable_to_all.begin(), usable_to_all.end(), idx);
        want = it != usable_to_all.end() ? std::to_string(it - usable_to_all.begin()) : std::string("\x01no-such-gpu");
    }
    bool matched = true;
    const size_t pick = usable_to_all[pick_gpu(usable, want.c_str(), &matched)];
    gpu_override_matched_ = matched;
    if (!matched) NXLOG_W(kTag, "gpu override '%s' matches no usable GPU: using the default", gpu_want.c_str());
    gpu_index_ = pick;
    physical_ = cands[pick].pd;
    queue_family_ = cands[pick].qfam;
    NXLOG_I(kTag, "using GPU %zu: %s%s", pick, gpus_[pick].name.c_str(), gpu_want.empty() ? " (default rule)" : " (override)");
    vkGetPhysicalDeviceProperties(physical_, &props_);

    extras::streamline_check_framegen(physical_);   // before device/swapchain hooks can allocate FG resources

    // features: the NVN back end wants everything the GPU offers (translated Maxwell shaders use Float64, subgroup operations,
    // DrawParameters, image gather, non-uniform indexing, geometry stages ...), so the device is created with every core 1.0
    // to 1.3 feature that is available (except robustBufferAccess) plus the extension features below.
    uint32_t next = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &next, nullptr);
    std::vector<VkExtensionProperties> dexts(next);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &next, dexts.data());
    auto has_dext = [&](const char* name) {
        for (auto& e : dexts) if (!std::strcmp(e.extensionName, name)) return true;
        return false;
    };
    VkPhysicalDeviceVulkan11Features a11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features a12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features a13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT afsi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT adcc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceDepthClipEnableFeaturesEXT adce = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT acbc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT aeds3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT avids = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT adrua = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT agpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT atfb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT au8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    VkPhysicalDevicePresentIdFeaturesKHR apid = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDeviceAntiLagFeaturesAMD aal = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD};
    VkPhysicalDevicePipelineRobustnessFeaturesEXT aprb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR afsr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR acsd = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
    VkPhysicalDeviceMemoryPriorityFeaturesEXT ampri = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT};
    VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT apage = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT};
    std::vector<const char*> dev_exts;
    void* chain = nullptr;
    auto add_ext = [&](const char* name, void* feat_struct) {
        if (!has_dext(name)) return false;
        dev_exts.push_back(name);
        // every extension feature struct is { sType, pNext, ... }: link it into the chain
        struct Base { VkStructureType s; void* n; };
        static_cast<Base*>(feat_struct)->n = chain;
        chain = feat_struct;
        return true;
    };
    const bool x_fsi = add_ext(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME, &afsi);
    const bool x_dcc = add_ext(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, &adcc);
    const bool x_dce = add_ext(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, &adce);
    const bool x_cbc = add_ext(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, &acbc);
    const bool x_eds3 = add_ext(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, &aeds3);
    const bool x_vids = add_ext(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, &avids);
    const bool x_drua = add_ext(VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME, &adrua);
    const bool x_tfb = add_ext(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME, &atfb);
    // 8-bit index buffers (the EXT and the KHR extension share the feature struct)
    const bool x_u8 = add_ext(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, &au8) || add_ext("VK_KHR_index_type_uint8", &au8);
    // pipelines linked from separately compiled shader parts (nvn_gpl.cpp): needs VK_KHR_pipeline_library next to the extension itself
    const bool x_gpl = has_dext(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) && has_dext(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
                       add_ext(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, &agpl);
    if (x_gpl) dev_exts.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    // display extras: HDR metadata and the latency extensions (low_latency2 needs present_id; neither is used unless the settings ask)
    const bool x_hdrmd = !headless && has_dext(VK_EXT_HDR_METADATA_EXTENSION_NAME);
    if (x_hdrmd) dev_exts.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
    const bool x_pid = !headless && add_ext(VK_KHR_PRESENT_ID_EXTENSION_NAME, &apid);
    const bool x_ll2 = x_pid && has_dext(VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
    if (x_ll2) dev_exts.push_back(VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
    const bool x_aal = !headless && add_ext(VK_AMD_ANTI_LAG_EXTENSION_NAME, &aal);
    // the OS's video memory budget and this process' usage per heap, which vmaGetHeapBudgets (the vma.device_* gauges, the memory guard)
    // reads: without the extension VMA only estimates them (80% of the heap as the budget, its own blocks as the usage)
    const bool x_membudget = has_dext(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (x_membudget) dev_exts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    // Over the budget, the OS may move this process' device-local allocations to host memory instead of the driver refusing new ones
    // (VK_EXT_pageable_device_local_memory), and the priority of an allocation says which stay (VK_EXT_memory_priority; create_image and
    // create_buffer set it). [gfx] pageable_memory = false: neither is enabled.
    const bool want_pageable = config_get_bool("gfx", "pageable_memory", true);
    const bool x_mpri = want_pageable && add_ext(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME, &ampri);
    const bool x_page = x_mpri && add_ext(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME, &apage);
    features_.external_memory_host = has_dext(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    if (features_.external_memory_host) dev_exts.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    if (has_dext(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) { dev_exts.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME); features_.push_descriptor = true; }
    // robust access per pipeline, for the translated shaders' uniform buffers only (nvn_pipeline.cpp ubo_robustness); robustness2 is
    // enabled for its pipeline behaviour, not its device-wide features (left off: they cost speed)
    const bool x_prb = add_ext(VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME, &aprb);
    // coarser fragment shading for chosen draws ([nvn] shading_rate, nvn_replay.cpp): only the per-pipeline rate is used
    const bool x_fsr = add_ext(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, &afsr);
    // the FSR SDK v2.4.0 upscaler builds compute shaders with derivative groups wherever the GPU offers them (it does not check that the
    // device enabled them: VUID-VkShaderModuleCreateInfo-pCode-08740 / 08742 otherwise); the NV extension shares the feature struct
    if (!add_ext(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME, &acsd)) add_ext(VK_NV_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME, &acsd);
    bool rba2 = false;
    if (x_prb && has_dext(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)) {
        VkPhysicalDeviceRobustness2FeaturesEXT r2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &r2};
        vkGetPhysicalDeviceFeatures2(physical_, &f2);
        rba2 = r2.robustBufferAccess2 == VK_TRUE;
        if (rba2) dev_exts.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    }
    // availability query: one chain holding all the structs
    a11.pNext = &a12; a12.pNext = &a13; a13.pNext = chain;
    VkPhysicalDeviceFeatures2 avail = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &a11};
    vkGetPhysicalDeviceFeatures2(physical_, &avail);
    // the same chain, as it was filled in, enables everything that is available (minus robustness, which costs speed)
    a13.dynamicRendering = VK_TRUE;
    a13.synchronization2 = VK_TRUE;
    a13.robustImageAccess = VK_FALSE;
    avail.features.robustBufferAccess = VK_FALSE;
    VkPhysicalDeviceFeatures2& en = avail;
    const VkPhysicalDeviceFeatures& af = avail.features;
    features_.bc = af.textureCompressionBC;
    features_.astc_ldr = af.textureCompressionASTC_LDR;
    features_.etc2 = af.textureCompressionETC2;
    features_.sampler_anisotropy = af.samplerAnisotropy;
    features_.independent_blend = af.independentBlend;
    features_.depth_clamp = af.depthClamp;
    features_.fill_mode_non_solid = af.fillModeNonSolid;
    features_.timeline_semaphore = a12.timelineSemaphore;
    features_.subgroup_size_control = a13.subgroupSizeControl;
    features_.compute_full_subgroups = a13.computeFullSubgroups;
    features_.dynamic_rendering = true;
    features_.synchronization2 = true;
    features_.geometry_shader = af.geometryShader;
    features_.shader_float64 = af.shaderFloat64;
    features_.shader_int64 = af.shaderInt64;
    features_.dual_src_blend = af.dualSrcBlend;
    features_.logic_op = af.logicOp;
    features_.multi_viewport = af.multiViewport;
    features_.depth_bounds = af.depthBounds;
    features_.depth_bias_clamp = af.depthBiasClamp;
    features_.wide_lines = af.wideLines;
    features_.large_points = af.largePoints;
    features_.image_cube_array = af.imageCubeArray;
    features_.occlusion_query = true;   // occlusion queries are core
    features_.occlusion_query_precise = af.occlusionQueryPrecise;
    features_.pipeline_statistics_query = af.pipelineStatisticsQuery;
    features_.sparse_binding = af.sparseBinding;
    features_.sparse_residency_image2d = af.sparseResidencyImage2D;
    features_.sparse_residency_image3d = af.sparseResidencyImage3D;
    features_.sparse_residency_aliased = af.sparseResidencyAliased;
    features_.shader_draw_parameters = a11.shaderDrawParameters;
    features_.sampler_mirror_clamp_to_edge = a12.samplerMirrorClampToEdge;
    features_.descriptor_indexing = a12.descriptorIndexing && a12.shaderSampledImageArrayNonUniformIndexing;
    features_.host_query_reset = a12.hostQueryReset;
    features_.fragment_shader_interlock = x_fsi && afsi.fragmentShaderPixelInterlock;
    features_.depth_clip_control = x_dcc && adcc.depthClipControl;
    features_.depth_clip_enable = x_dce && adce.depthClipEnable;
    features_.custom_border_color = x_cbc && acbc.customBorderColors && acbc.customBorderColorWithoutFormat;
    features_.extended_dynamic_state3 = x_eds3;
    features_.vertex_input_dynamic_state = x_vids && avids.vertexInputDynamicState;
    features_.dynamic_rendering_unused_attachments = x_drua && adrua.dynamicRenderingUnusedAttachments;
    features_.transform_feedback = x_tfb && atfb.transformFeedback;
    features_.index_type_uint8 = x_u8 && au8.indexTypeUint8;
    if (features_.transform_feedback) {   // transformFeedbackQueries is a property: the primitive counters of indirect draws under feedback need it
        VkPhysicalDeviceTransformFeedbackPropertiesEXT tfp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 tfpp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &tfp};
        vkGetPhysicalDeviceProperties2(physical_, &tfpp);
        features_.transform_feedback_queries = tfp.transformFeedbackQueries == VK_TRUE;
    }
    features_.graphics_pipeline_library = x_gpl && agpl.graphicsPipelineLibrary;
    if (features_.graphics_pipeline_library) {
        // the libraries are compiled apart from each other, so the vertex shader's outputs and the fragment shader's inputs must not need matching interpolation decorations
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 gpp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &gp};
        vkGetPhysicalDeviceProperties2(physical_, &gpp);
        features_.graphics_pipeline_library = gp.graphicsPipelineLibraryIndependentInterpolationDecoration == VK_TRUE;
        NXLOG_I(kTag, "pipeline libraries: fast linking %d, independent interpolation decoration %d", gp.graphicsPipelineLibraryFastLinking, gp.graphicsPipelineLibraryIndependentInterpolationDecoration);
    }
    features_.hdr_metadata = x_hdrmd;
    features_.present_id = x_pid && apid.presentId;
    features_.low_latency2 = x_ll2 && features_.present_id;
    features_.anti_lag = x_aal && aal.antiLag;
    features_.fragment_shading_rate = x_fsr && afsr.pipelineFragmentShadingRate;
    features_.ubo_robustness = (x_prb && aprb.pipelineRobustness) ? (rba2 ? 2 : 1) : 0;   // robustBufferAccess itself is a required feature
    NXLOG_I(kTag, "texture compression on this GPU: BC %s, ASTC-LDR %s, ETC2 %s", features_.bc ? "yes" : "no",
            features_.astc_ldr ? "yes" : "no", features_.etc2 ? "yes" : "no");
    NXLOG_I(kTag, "features: geometry %d float64 %d int64 %d dualSrc %d logicOp %d multiViewport %d drawParams %d descIndexing %d interlock %d depthClipControl %d customBorder %d hostMemImport %d pushDesc %d uboRobustness %d",
            features_.geometry_shader, features_.shader_float64, features_.shader_int64, features_.dual_src_blend, features_.logic_op,
            features_.multi_viewport, features_.shader_draw_parameters, features_.descriptor_indexing, features_.fragment_shader_interlock,
            features_.depth_clip_control, features_.custom_border_color, features_.external_memory_host, features_.push_descriptor,
            features_.ubo_robustness);
    {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT xh = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceVulkan11Properties p11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, features_.external_memory_host ? &xh : nullptr};
        VkPhysicalDeviceVulkan13Properties p13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
        p13.pNext = &p11;
        VkPhysicalDeviceProperties2 pp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p13};
        vkGetPhysicalDeviceProperties2(physical_, &pp);
        limits_.min_imported_host_alignment = features_.external_memory_host ? xh.minImportedHostPointerAlignment : 0;
        limits_.max_buffer_size = p13.maxBufferSize;
        limits_.min_subgroup_size = p13.minSubgroupSize;
        limits_.max_subgroup_size = p13.maxSubgroupSize;
        limits_.max_compute_workgroup_subgroups = p13.maxComputeWorkgroupSubgroups;
        limits_.required_subgroup_size_stages = p13.requiredSubgroupSizeStages;
        NXLOG_I(kTag, "subgroup: native %u, range %u..%u, required stages 0x%x, size control %u, full compute %u, max workgroup subgroups %u",
                p11.subgroupSize, p13.minSubgroupSize, p13.maxSubgroupSize, p13.requiredSubgroupSizeStages,
                a13.subgroupSizeControl, a13.computeFullSubgroups, p13.maxComputeWorkgroupSubgroups);
        limits_.subgroup_size = p11.subgroupSize;
        limits_.subgroup_ops = p11.subgroupSupportedOperations;
        limits_.max_memory_allocation_count = props_.limits.maxMemoryAllocationCount;
    }
    if (!headless) dev_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#ifdef _WIN32
    // Without it the driver may take a window covering the monitor (borderless fullscreen) into exclusive mode on its own, and every
    // switch to another window or monitor then blacks the screen out while the display mode changes (vk_renderer.cpp disallows it).
    if (x_surfcaps2 && has_dext(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME)) {
        dev_exts.push_back(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
        features_.full_screen_exclusive = true;
    }
#endif
    // [dlss] frame_gen = fsr3: the FSR 3 swapchain presents and acquires on queues of its own (extras/framegen_ffx.h)
    uint32_t fg_queues = 0;
    if (!headless) {
        uint32_t nfam = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &nfam, nullptr);
        std::vector<VkQueueFamilyProperties> fams(nfam);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &nfam, fams.data());
        if (queue_family_ < nfam) fg_queues = extras::ffx_fg_extra_queues(fams[queue_family_].queueCount);
    }
    const float prio[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1 + fg_queues;
    qci.pQueuePriorities = prio;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &en};
    // Video memory (VK_EXT_memory_budget is enabled above, with the priority and pageable extensions).
    // AMD's driver refuses an allocation that takes this process past the size of a heap (VK_ERROR_OUT_OF_DEVICE_MEMORY) where other
    // drivers let Windows move video memory to shared memory: slower, but the game goes on. VK_AMD_memory_overallocation_behavior asks
    // for the same there ([gfx] amd_overallocation = false: the driver's default again).
    VkDeviceMemoryOverallocationCreateInfoAMD overalloc = {VK_STRUCTURE_TYPE_DEVICE_MEMORY_OVERALLOCATION_CREATE_INFO_AMD};
    const bool x_overalloc = has_dext(VK_AMD_MEMORY_OVERALLOCATION_BEHAVIOR_EXTENSION_NAME) && config_get_bool("gfx", "amd_overallocation", true);
    if (x_overalloc) {
        dev_exts.push_back(VK_AMD_MEMORY_OVERALLOCATION_BEHAVIOR_EXTENSION_NAME);
        overalloc.overallocationBehavior = VK_MEMORY_OVERALLOCATION_BEHAVIOR_ALLOWED_AMD;
        overalloc.pNext = dci.pNext;
        dci.pNext = &overalloc;
    }
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(dev_exts.size());
    dci.ppEnabledExtensionNames = dev_exts.data();
    r = vkCreateDevice(physical_, &dci, nullptr, &device_);
    if (r != VK_SUCCESS) return fail("vkCreateDevice failed (" + std::to_string(r) + ")");
    if (sl_gipa) {
        // Resolve the device loader from the interposer itself. Do not let device loading
        // replace the swapchain hooks with driver pointers (including on recreation).
        auto sl_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(extras::streamline_proc("vkGetDeviceProcAddr"));
        if (!sl_gdpa) return fail("Streamline has no vkGetDeviceProcAddr");
        vkGetDeviceProcAddr = sl_gdpa;
    }
    volkLoadDevice(device_);
    driver_allocate_memory = vkAllocateMemory;
    driver_free_memory = vkFreeMemory;
    vkAllocateMemory = counted_allocate_memory;
    vkFreeMemory = counted_free_memory;
    NXLOG_I(kTag, "memory limits: maxMemoryAllocationCount %u, minImportedHostPointerAlignment %llu, external host %d, memory budget %d, overallocation allowed %d",
            limits_.max_memory_allocation_count, (unsigned long long)limits_.min_imported_host_alignment, features_.external_memory_host, x_membudget, x_overalloc);
    {
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(physical_, &mp);
        for (uint32_t i = 0; i < mp.memoryHeapCount; ++i) {
            std::string types;
            for (uint32_t t = 0; t < mp.memoryTypeCount; ++t)
                if (mp.memoryTypes[t].heapIndex == i) {
                    char tb[24];
                    std::snprintf(tb, sizeof tb, "%s%u:0x%x", types.empty() ? "" : " ", t, static_cast<unsigned>(mp.memoryTypes[t].propertyFlags));
                    types += tb;
                }
            NXLOG_I(kTag, "memory heap %u: %.2f GiB%s, types (index:flags) %s", i, static_cast<double>(mp.memoryHeaps[i].size) / (1024.0 * 1024.0 * 1024.0),
                    (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " device-local" : "", types.c_str());
        }
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    if (fg_queues) extras::ffx_fg_install(*this, fg_queues);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &nq, qf.data());
    timestamp_valid_bits_ = qf[queue_family_].timestampValidBits;

    VmaVulkanFunctions vf = {};
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    vf.vkAllocateMemory = counted_allocate_memory;
    vf.vkFreeMemory = counted_free_memory;
    VmaAllocatorCreateInfo aci = {};
    aci.physicalDevice = physical_;
    aci.device = device_;
    aci.instance = instance_;
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.pVulkanFunctions = &vf;
    if (x_membudget) aci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    g_budget_exact = x_membudget;
    const bool mem_priority = x_mpri && ampri.memoryPriority;
    if (mem_priority) aci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
    NXLOG_I(kTag, "video memory over the budget: memory priority %d, pageable device-local memory %d ([gfx] pageable_memory = %d)",
            mem_priority, x_page && apage.pageableDeviceLocalMemory, want_pageable);
    NXLOG_I(kTag, "video memory budget: %s", x_membudget ? "from the OS (VK_EXT_memory_budget)" : "estimated (no VK_EXT_memory_budget)");
    r = vmaCreateAllocator(&aci, &vma_);
    if (r != VK_SUCCESS) return fail("vmaCreateAllocator failed (" + std::to_string(r) + ")");
    register_vma_gauges(vma_, props_.deviceType);

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = queue_family_;
    if (vkCreateCommandPool(device_, &cpi, nullptr, &impl_->once_pool) == VK_SUCCESS) vk_gauge_command_pools().add(1);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device_, &fci, nullptr, &impl_->once_fence) == VK_SUCCESS) vk_gauge_fences().add(1);

    NXLOG_I(kTag, "device ready: %s, queue family %u, validation %s, %s", props_.deviceName, queue_family_, validation_ ? "on" : "off",
            headless ? "headless" : "window surface");
    return true;
}

Device::~Device() {
    if (device_) vkDeviceWaitIdle(device_);
    if (impl_) {
        if (impl_->once_fence) { vkDestroyFence(device_, impl_->once_fence, nullptr); vk_gauge_fences().sub(1); }
        if (impl_->once_pool) { vkDestroyCommandPool(device_, impl_->once_pool, nullptr); vk_gauge_command_pools().sub(1); }
    }
    unregister_vma_gauges();   // before the allocator goes: the gauges read it (the registry lock makes this wait for a running read)
    extras::framegen_shutdown(this);
    if (vma_) vmaDestroyAllocator(vma_);
    extras::streamline_shutdown();
    if (device_) vkDestroyDevice(device_, nullptr);
    if (surface_) {
        if (owns_platform_surface_) platform::destroy_vulkan_surface(instance_, reinterpret_cast<uint64_t>(surface_));
        else vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (messenger_) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    delete impl_;
}

// ---- validation -------------------------------------------------------------------------------------------------------
ValidationStats Device::validation_stats() const {
    ValidationStats s;
    s.errors = impl_->errors.load();
    s.warnings = impl_->warnings.load();
    s.infos = impl_->infos.load();
    s.verbose = impl_->verbose.load();
    std::lock_guard<std::mutex> lk(impl_->first_error_mu);
    s.first_error = impl_->first_error;
    return s;
}

void Device::validation_reset() {
    impl_->errors = impl_->warnings = impl_->infos = impl_->verbose = 0;
    std::lock_guard<std::mutex> lk(impl_->first_error_mu);
    impl_->first_error.clear();
}

void Device::set_name(VkObjectType type, uint64_t handle, const char* name) {
    if (!debug_utils_ || !name) return;
    VkDebugUtilsObjectNameInfoEXT ni = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    ni.objectType = type;
    ni.objectHandle = handle;
    ni.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(device_, &ni);
}

// ---- images -----------------------------------------------------------------------------------------------------------
FormatCheckStats Device::format_check_stats() const {
    return {impl_->fmt_checks.load(), impl_->fmt_hits.load(), impl_->fmt_rejected.load()};
}

bool Device::image_supported(const ImageDesc& d, std::string* why) {
    auto no = [&](const std::string& m) {
        if (why) *why = m;
        return false;
    };
    char fmtbuf[48];
    std::snprintf(fmtbuf, sizeof fmtbuf, "format %d", static_cast<int>(d.format));
    if (d.format == VK_FORMAT_UNDEFINED) return no("VK_FORMAT_UNDEFINED");
    if (!d.width || !d.height || !d.depth || !d.mip_levels || !d.array_layers) return no("a zero image dimension");
    if (is_astc(d.format) && !features_.astc_ldr) return no(std::string(fmtbuf) + ": ASTC, textureCompressionASTC_LDR is not available on " + props_.deviceName);
    if (is_bc(d.format) && !features_.bc) return no(std::string(fmtbuf) + ": BC, textureCompressionBC is not available on " + props_.deviceName);
    if (is_etc2(d.format) && !features_.etc2) return no(std::string(fmtbuf) + ": ETC2, textureCompressionETC2 is not available on " + props_.deviceName);

    const Impl::Key key{d.format, d.type, d.tiling, d.usage, d.flags};
    Impl::Val val;
    {
        std::lock_guard<std::mutex> lk(impl_->fmt_mu);
        auto it = impl_->fmt_cache.find(key);
        if (it != impl_->fmt_cache.end()) {
            val = it->second;
            impl_->fmt_hits++;
        } else {
            VkPhysicalDeviceImageFormatInfo2 info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
            info.format = d.format;
            info.type = d.type;
            info.tiling = d.tiling;
            info.usage = d.usage;
            info.flags = d.flags;
            VkImageFormatProperties2 out = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
            val.result = vkGetPhysicalDeviceImageFormatProperties2(physical_, &info, &out);
            val.props = out.imageFormatProperties;
            impl_->fmt_checks++;
            impl_->fmt_cache.emplace(key, val);
        }
    }
    if (val.result != VK_SUCCESS) return no(std::string(fmtbuf) + ": vkGetPhysicalDeviceImageFormatProperties2 says " + std::to_string(val.result) + " for this type/tiling/usage/flags");
    if (d.width > val.props.maxExtent.width || d.height > val.props.maxExtent.height || d.depth > val.props.maxExtent.depth)
        return no(std::string(fmtbuf) + ": extent exceeds the device maximum");
    if (d.mip_levels > val.props.maxMipLevels) return no(std::string(fmtbuf) + ": more mip levels than the device allows");
    if (d.array_layers > val.props.maxArrayLayers) return no(std::string(fmtbuf) + ": more array layers than the device allows");
    if (!(val.props.sampleCounts & d.samples)) return no(std::string(fmtbuf) + ": sample count not supported");
    const uint32_t maxdim = std::max({d.width, d.height, d.type == VK_IMAGE_TYPE_3D ? d.depth : 1u});
    if (d.mip_levels > 1 + static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(maxdim))))) return no(std::string(fmtbuf) + ": more mip levels than the image has");
    return true;
}

VkResult Device::create_image(const ImageDesc& d, Image* out, std::string* why) {
    std::string reason;
    if (!image_supported(d, &reason)) {
        impl_->fmt_rejected++;
        NXLOG_W(kTag, "image '%s' refused before creation: %s", d.name ? d.name : "?", reason.c_str());
        if (why) *why = reason;
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = d.flags;
    ci.imageType = d.type;
    ci.format = d.format;
    ci.extent = {d.width, d.height, d.depth};
    ci.mipLevels = d.mip_levels;
    ci.arrayLayers = d.array_layers;
    ci.samples = d.samples;
    ci.tiling = d.tiling;
    ci.usage = d.usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    // VK_EXT_memory_priority (used for a dedicated allocation; ignored without the extension): what is rendered to stays in video memory
    // before what is only sampled. Zero, the default, would make every large image the first to go.
    ai.priority = (d.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) ? 1.0f : 0.5f;
    Image img;
    VkImage handle = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkResult r = vmaCreateImage(vma_, &ci, &ai, &handle, &alloc, nullptr);
    if (r != VK_SUCCESS) {
        if (why) *why = "vmaCreateImage failed (" + std::to_string(r) + ")";
        return r;
    }
    vk_gauge_images().add(1);
    {   // very large images are worth a line: they decide the process' memory footprint
        VmaAllocationInfo inf{};
        vmaGetAllocationInfo(vma_, alloc, &inf);
        vk_gauge_image_bytes().add(static_cast<int64_t>(inf.size));
        if (inf.size >= (256ull << 20))
            NXLOG_I(kTag, "large image '%s': %ux%ux%u, %u levels, %u layers, format %d, %llu MB", d.name ? d.name : "?", d.width, d.height, d.depth,
                    d.mip_levels, d.array_layers, static_cast<int>(d.format), static_cast<unsigned long long>(inf.size >> 20));
    }
    img.image = handle;
    img.allocation = alloc;
    img.format = d.format;
    img.extent = ci.extent;
    img.mip_levels = d.mip_levels;
    img.array_layers = d.array_layers;
    if (d.create_view) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = handle;
        vi.format = d.format;
        if (d.type == VK_IMAGE_TYPE_3D) vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
        else if (d.type == VK_IMAGE_TYPE_1D) vi.viewType = d.array_layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        else if (d.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) vi.viewType = d.array_layers > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        else vi.viewType = d.array_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange = {aspect_of(d.format), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        r = vkCreateImageView(device_, &vi, nullptr, &img.view);
        if (r != VK_SUCCESS) {
            VmaAllocationInfo inf{};
            vmaGetAllocationInfo(vma_, alloc, &inf);
            vk_gauge_image_bytes().sub(static_cast<int64_t>(inf.size));
            vmaDestroyImage(vma_, handle, alloc);
            vk_gauge_images().sub(1);
            if (why) *why = "vkCreateImageView failed (" + std::to_string(r) + ")";
            return r;
        }
        vk_gauge_image_views().add(1);
    }
    set_name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(handle), d.name);
    *out = img;
    return VK_SUCCESS;
}

void Device::destroy_image(Image& img) {
    if (img.view) { vkDestroyImageView(device_, img.view, nullptr); vk_gauge_image_views().sub(1); }
    if (img.image) {
        if (img.allocation) {
            VmaAllocationInfo inf{};
            vmaGetAllocationInfo(vma_, img.allocation, &inf);
            vk_gauge_image_bytes().sub(static_cast<int64_t>(inf.size));
        }
        vmaDestroyImage(vma_, img.image, img.allocation);
        vk_gauge_images().sub(1);
    }
    img = Image{};
}

// ---- buffers ----------------------------------------------------------------------------------------------------------
VkResult Device::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryUse use, Buffer* out, const char* name) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.priority = 1.0f;   // VK_EXT_memory_priority, a dedicated allocation: buffers are what draws read (pool copies among them), and small next to the images
    if (use == MemoryUse::Upload) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    if (use == MemoryUse::Staging) {   // system memory whatever the usage: a load holds gigabytes of these at once, and video memory is for what stays
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    if (use == MemoryUse::Readback) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info = {};
    Buffer b;
    VkResult r = vmaCreateBuffer(vma_, &bi, &ai, &b.buffer, &b.allocation, &info);
    if (r != VK_SUCCESS) return r;
    vk_gauge_buffers().add(1);
    {
        VkMemoryPropertyFlags mp = 0;
        vmaGetAllocationMemoryProperties(vma_, b.allocation, &mp);
        if (mp & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) vk_gauge_buffer_bytes().add(static_cast<int64_t>(info.size));
    }
    if (use == MemoryUse::Upload || use == MemoryUse::Staging || use == MemoryUse::Readback) {      // S34: where host-visible buffers live (device-local host-visible = resizable BAR, else system memory)
        static std::atomic<int> logged{0};
        VkMemoryPropertyFlags mp = 0;
        vmaGetAllocationMemoryProperties(vma_, b.allocation, &mp);
        const bool dl = (mp & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
        static std::atomic<uint64_t> n_dl{0}, n_sys{0}, b_dl{0}, b_sys{0};
        (dl ? n_dl : n_sys).fetch_add(1);
        (dl ? b_dl : b_sys).fetch_add(size);
        if (logged.load() < 6 || (size >= (8u << 20) && logged.load() < 40)) {
            logged.fetch_add(1);
            NXLOG_D("vk", "host-visible buffer %s: %llu KiB in %s memory (flags 0x%x); so far %llu in device-local (%llu MiB), %llu in system memory (%llu MiB)", name ? name : "?",
                    static_cast<unsigned long long>(size >> 10), dl ? "device-local" : "system", static_cast<unsigned>(mp), static_cast<unsigned long long>(n_dl.load()),
                    static_cast<unsigned long long>(b_dl.load() >> 20), static_cast<unsigned long long>(n_sys.load()), static_cast<unsigned long long>(b_sys.load() >> 20));
        }
    }
    b.size = size;
    b.mapped = info.pMappedData;
    set_name(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(b.buffer), name);
    *out = b;
    return VK_SUCCESS;
}

void Device::destroy_buffer(Buffer& b) {
    if (b.buffer) {
        if (b.allocation) {
            VkMemoryPropertyFlags mp = 0;
            vmaGetAllocationMemoryProperties(vma_, b.allocation, &mp);
            if (mp & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                VmaAllocationInfo inf{};
                vmaGetAllocationInfo(vma_, b.allocation, &inf);
                vk_gauge_buffer_bytes().sub(static_cast<int64_t>(inf.size));
            }
        }
        vmaDestroyBuffer(vma_, b.buffer, b.allocation);
        vk_gauge_buffers().sub(1);
    }
    b = Buffer{};
}

void Device::flush_buffer(const Buffer& b) {
    if (b.allocation) vmaFlushAllocation(vma_, b.allocation, 0, VK_WHOLE_SIZE);
}

void Device::invalidate_buffer(const Buffer& b) {
    if (b.allocation) vmaInvalidateAllocation(vma_, b.allocation, 0, VK_WHOLE_SIZE);
}

// ---- one-shot submit, idle, stats -------------------------------------------------------------------------------------
bool Device::submit_once(const std::function<void(VkCommandBuffer)>& fn) {
    std::lock_guard<std::mutex> lk(impl_->once_mu);
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = impl_->once_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &ai, &cmd) != VK_SUCCESS) return false;
    vk_gauge_command_buffers().add(1);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    fn(cmd);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    bool ok;
    {
        std::lock_guard<std::mutex> ql(queue_mutex_);
        ok = vkQueueSubmit(queue_, 1, &si, impl_->once_fence) == VK_SUCCESS;
    }
    if (ok) ok = vkWaitForFences(device_, 1, &impl_->once_fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    vkResetFences(device_, 1, &impl_->once_fence);
    vkFreeCommandBuffers(device_, impl_->once_pool, 1, &cmd);
    vk_gauge_command_buffers().sub(1);
    return ok;
}

void Device::wait_idle() {
    std::lock_guard<std::mutex> ql(queue_mutex_);
    vkDeviceWaitIdle(device_);
}

Device::MemoryStats Device::memory_stats() {
    VmaTotalStatistics st;
    vmaCalculateStatistics(vma_, &st);
    MemoryStats m;
    m.allocations = st.total.statistics.allocationCount;
    m.blocks = st.total.statistics.blockCount;
    m.bytes = st.total.statistics.allocationBytes;
    return m;
}

}  // namespace nxrt::gfx
