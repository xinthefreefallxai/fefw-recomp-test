// SPDX-License-Identifier: GPL-3.0-or-later
// NVN2 DLSS entry points (api_dlss.h): the network builder, the network, the evaluation parameters and nvnCommandBufferEvaluateDlss. None
// of them is in the NVN1 table (NVN2_REG). Signatures and object layouts are those of Fortune's Weave's NVN 231.40 driver; the evidence for
// each is in notes/dlss.md. The builder and parameter procs behave exactly as the driver's (plain stores and loads); GetOptimalSettings
// answers what the console's NGX computes; the network's GPU work is the host upscaler's (api_dlss_replay.cpp).
#include "api_dlss.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include "nvn_cmd.h"
#include "nvn_impl.h"
#include "nxrt/log.h"

struct NVNdlssNetworkBuilder;
struct NVNdlssNetwork;
struct NVNdlssParameters;

namespace nxrt::nvn {
using namespace be;
namespace {

constexpr const char* kTag = "nvn.dlss";
constexpr uint32_t kNetMagic = 0x53534C44;   // "DLSS"

// What the guest network object holds: its host side, checked against the live set before use.
struct NetHeader {
    DlssNet* host;
    uint32_t magic;
    uint32_t id;
};

std::mutex g_mu;
std::unordered_set<DlssNet*> g_live;
std::atomic<uint32_t> g_next_id{1};

DlssBuilderData* bd(NVNdlssNetworkBuilder* b) { return reinterpret_cast<DlssBuilderData*>(b); }
const DlssBuilderData* bd(const NVNdlssNetworkBuilder* b) { return reinterpret_cast<const DlssBuilderData*>(b); }
DlssParamsData* pd(NVNdlssParameters* p) { return reinterpret_cast<DlssParamsData*>(p); }
const DlssParamsData* pd(const NVNdlssParameters* p) { return reinterpret_cast<const DlssParamsData*>(p); }

// The live host side of a guest network (a reference), or null.
Ref<DlssNet> net_of(const NVNdlssNetwork* n) {
    if (!n) return nullptr;
    NetHeader h;
    std::memcpy(&h, n, sizeof h);
    if (h.magic != kNetMagic || !h.host) return nullptr;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_live.count(h.host)) return nullptr;
    return Ref<DlssNet>::share(h.host);
}

// Takes the host side out of a guest network (Finalize, or Initialize over a live one).
void release_net(NVNdlssNetwork* n) {
    NetHeader h;
    std::memcpy(&h, n, sizeof h);
    if (h.magic != kNetMagic || !h.host) return;
    DlssNet* host = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_live.erase(h.host)) host = h.host;
    }
    std::memset(n, 0, sizeof h);
    if (host) host->unref();   // command lists that still hold evaluations keep it alive
}

const char* method_name(int m) { return m == 0 ? "up to 2x" : m == 1 ? "3x" : m == 2 ? "1x (DLAA)" : "?"; }

// ---- network builder: the driver's stores and loads -----------------------------------------------------------------------------------
void nvnDlssNetworkBuilderSetDefaults(NVNdlssNetworkBuilder* b) {
    DlssBuilderData* d = bd(b);
    const uint64_t device = d->device;   // the driver clears 0x08..0xC0 and keeps the device
    std::memset(d, 0, sizeof *d);
    d->device = device;
    d->max_entries = 1;
}
void nvnDlssNetworkBuilderSetDevice(NVNdlssNetworkBuilder* b, NVNdevice* device) { bd(b)->device = reinterpret_cast<uint64_t>(device); }
NVNdevice* nvnDlssNetworkBuilderGetDevice(const NVNdlssNetworkBuilder* b) { return reinterpret_cast<NVNdevice*>(bd(b)->device); }
void nvnDlssNetworkBuilderSetFlags(NVNdlssNetworkBuilder* b, int flags) { bd(b)->flags = flags; }
int nvnDlssNetworkBuilderGetFlags(const NVNdlssNetworkBuilder* b) { return bd(b)->flags; }
void nvnDlssNetworkBuilderSetMethod(NVNdlssNetworkBuilder* b, int method) { bd(b)->method = method; }
int nvnDlssNetworkBuilderGetMethod(const NVNdlssNetworkBuilder* b) { return bd(b)->method; }
void nvnDlssNetworkBuilderSetPreset(NVNdlssNetworkBuilder* b, int preset) { bd(b)->preset = preset; }
int nvnDlssNetworkBuilderGetPreset(const NVNdlssNetworkBuilder* b) { return bd(b)->preset; }
void nvnDlssNetworkBuilderSetOutputResolution(NVNdlssNetworkBuilder* b, int w, int h) {
    bd(b)->out_w = w;
    bd(b)->out_h = h;
}
void nvnDlssNetworkBuilderGetOutputResolution(const NVNdlssNetworkBuilder* b, int* w, int* h) {
    if (w) *w = bd(b)->out_w;
    if (h) *h = bd(b)->out_h;
}
void nvnDlssNetworkBuilderSetMaxEvaluationEntries(NVNdlssNetworkBuilder* b, int n) { bd(b)->max_entries = n; }
int nvnDlssNetworkBuilderGetMaxEvaluationEntries(const NVNdlssNetworkBuilder* b) { return bd(b)->max_entries; }
void nvnDlssNetworkBuilderSetStorage(NVNdlssNetworkBuilder* b, void* memory, size_t size) {
    bd(b)->memory = reinterpret_cast<uint64_t>(memory);
    bd(b)->memory_size = size;
}
void* nvnDlssNetworkBuilderGetMemory(const NVNdlssNetworkBuilder* b) { return reinterpret_cast<void*>(bd(b)->memory); }
size_t nvnDlssNetworkBuilderGetMemorySize(const NVNdlssNetworkBuilder* b) { return static_cast<size_t>(bd(b)->memory_size); }
// (builder, pool, ids, count): the driver copies 32 ids whatever the count (Fortune's Weave passes the count it reserved, device info 0x85).
// Here a count of 1..31 copies that many and zeroes the rest, and null ids (no reservation, 0x85 answered 0) zeroes them all.
void nvnDlssNetworkBuilderSetTextureIDs(NVNdlssNetworkBuilder* b, const NVNtexturePool* pool, const int* ids, int count) {
    DlssBuilderData* d = bd(b);
    d->texture_pool = reinterpret_cast<uint64_t>(pool);
    const int n = !ids ? 0 : (count >= 1 && count < 32) ? count : 32;
    std::memset(d->ids, 0, sizeof d->ids);
    if (n) std::memcpy(d->ids, ids, static_cast<size_t>(n) * sizeof(int));
}
void nvnDlssNetworkBuilderGetTextureIDs(const NVNdlssNetworkBuilder* b, int* out) {
    if (out) std::memcpy(out, bd(b)->ids, sizeof bd(b)->ids);
}
const NVNtexturePool* nvnDlssNetworkBuilderGetTexturePool(const NVNdlssNetworkBuilder* b) { return reinterpret_cast<const NVNtexturePool*>(bd(b)->texture_pool); }

// (builder, &optimal w, &optimal h, &min w, &min h, &max w, &max h): the driver forwards the output size and method to the DLSS module
// (sdk 0x658AC0 -> subsdk0 0xAA760); Fortune's Weave passes an eighth argument (null) that the driver does not read.
void nvnDlssNetworkBuilderGetOptimalSettings(const NVNdlssNetworkBuilder* b, int* opt_w, int* opt_h, int* min_w, int* min_h, int* max_w, int* max_h) {
    const DlssOptimal o = dlss_optimal(bd(b)->out_w, bd(b)->out_h, bd(b)->method);
    if (opt_w) *opt_w = o.opt_w;
    if (opt_h) *opt_h = o.opt_h;
    if (min_w) *min_w = o.min_w;
    if (min_h) *min_h = o.min_h;
    if (max_w) *max_w = o.max_w;
    if (max_h) *max_h = o.max_h;
}

size_t nvnDlssNetworkBuilderGetStorageSize(const NVNdlssNetworkBuilder*) { return kDlssStorageSize; }

// ---- network --------------------------------------------------------------------------------------------------------------------------
NVNboolean nvnDlssNetworkInitialize(NVNdlssNetwork* n, const NVNdlssNetworkBuilder* b) {
    if (!n || !b) return 0;
    release_net(n);
    std::memset(n, 0, kDlssNetworkSize);
    const DlssBuilderData& d = *bd(b);
    if (d.out_w <= 0 || d.out_h <= 0 || d.out_w > 16384 || d.out_h > 16384) {
        NXLOG_W(kTag, "nvnDlssNetworkInitialize: output %dx%d refused", d.out_w, d.out_h);
        return 0;
    }
    auto* host = new DlssNet();
    host->id = g_next_id.fetch_add(1);
    host->flags = d.flags;
    host->method = d.method;
    host->preset = d.preset;
    host->max_entries = std::clamp(d.max_entries, 1, 16);
    host->max_w = d.out_w;
    host->max_h = d.out_h;
    host->out_w = d.out_w;
    host->out_h = d.out_h;
    host->texture_pool = d.texture_pool;
    std::memcpy(host->ids, d.ids, sizeof host->ids);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_live.insert(host);
    }
    const NetHeader h{host, kNetMagic, host->id};
    std::memcpy(n, &h, sizeof h);
    const DlssOptimal o = dlss_optimal(d.out_w, d.out_h, d.method);
    NXLOG_I(kTag, "network %u: output %dx%d, method %d (%s), preset %d, flags 0x%x, %d evaluation entries, texture ids %d.., storage %llu bytes; "
                  "NGX optimal %dx%d, dynamic %dx%d .. %dx%d",
            host->id, d.out_w, d.out_h, d.method, method_name(d.method), d.preset, static_cast<unsigned>(d.flags), host->max_entries, d.ids[0],
            static_cast<unsigned long long>(d.memory_size), o.opt_w, o.opt_h, o.min_w, o.min_h, o.max_w, o.max_h);
    return 1;
}

void nvnDlssNetworkFinalize(NVNdlssNetwork* n) {
    if (n) release_net(n);
}

// The driver refuses a size above the one the network was initialized with; otherwise the output size changes.
NVNboolean nvnDlssNetworkResize(NVNdlssNetwork* n, int w, int h) {
    Ref<DlssNet> net = net_of(n);
    if (!net || w <= 0 || h <= 0 || w > net->max_w || h > net->max_h) return 0;
    net->out_w = w;
    net->out_h = h;
    return 1;
}

// ---- parameters: the driver's stores and loads ----------------------------------------------------------------------------------------
void nvnDlssParametersSetDefaults(NVNdlssParameters* p) {
    DlssParamsData* d = pd(p);
    std::memset(d, 0, sizeof *d);
    d->mv_scale[0] = d->mv_scale[1] = 1.0f;
    d->pre_exposure = 1.0f;
    for (int& v : d->overlay) v = -1;
}
void set_tex(uint64_t& tex, uint64_t& view, const NVNtexture* t, const NVNtextureView* v) {
    tex = reinterpret_cast<uint64_t>(t);
    view = reinterpret_cast<uint64_t>(v);
}
void nvnDlssParametersSetInputColor(NVNdlssParameters* p, const NVNtexture* t, const NVNtextureView* v) { set_tex(pd(p)->color, pd(p)->color_view, t, v); }
void nvnDlssParametersSetOutputColor(NVNdlssParameters* p, const NVNtexture* t, const NVNtextureView* v) { set_tex(pd(p)->output, pd(p)->output_view, t, v); }
void nvnDlssParametersSetMotionVectors(NVNdlssParameters* p, const NVNtexture* t, const NVNtextureView* v) { set_tex(pd(p)->motion, pd(p)->motion_view, t, v); }
void nvnDlssParametersSetDepth(NVNdlssParameters* p, const NVNtexture* t, const NVNtextureView* v) { set_tex(pd(p)->depth, pd(p)->depth_view, t, v); }
void nvnDlssParametersSetBiasCurrentColorMask(NVNdlssParameters* p, const NVNtexture* t, const NVNtextureView* v) { set_tex(pd(p)->mask, pd(p)->mask_view, t, v); }
void nvnDlssParametersSetExposureTexture(NVNdlssParameters* p, const NVNtexture* t, const NVNtextureView* v) { set_tex(pd(p)->exposure, pd(p)->exposure_view, t, v); }
void nvnDlssParametersSetJitterOffset(NVNdlssParameters* p, float x, float y) {
    pd(p)->jitter[0] = x;
    pd(p)->jitter[1] = y;
}
void nvnDlssParametersGetJitterOffset(const NVNdlssParameters* p, float* x, float* y) {
    if (x) *x = pd(p)->jitter[0];
    if (y) *y = pd(p)->jitter[1];
}
void nvnDlssParametersSetMotionVectorScale(NVNdlssParameters* p, float x, float y) {
    pd(p)->mv_scale[0] = x;
    pd(p)->mv_scale[1] = y;
}
void nvnDlssParametersGetMotionVectorScale(const NVNdlssParameters* p, float* x, float* y) {
    if (x) *x = pd(p)->mv_scale[0];
    if (y) *y = pd(p)->mv_scale[1];
}
void nvnDlssParametersSetPreExposure(NVNdlssParameters* p, float v) { pd(p)->pre_exposure = v; }
float nvnDlssParametersGetPreExposure(const NVNdlssParameters* p) { return pd(p)->pre_exposure; }
void nvnDlssParametersSetHistoryReset(NVNdlssParameters* p, NVNboolean v) { pd(p)->reset = v; }
NVNboolean nvnDlssParametersGetHistoryReset(const NVNdlssParameters* p) { return pd(p)->reset; }
void nvnDlssParametersSetEvaluationIndex(NVNdlssParameters* p, int i) { pd(p)->eval_index = i; }
int nvnDlssParametersGetEvaluationIndex(const NVNdlssParameters* p) { return pd(p)->eval_index; }

void set2(int32_t* d, int x, int y) {
    d[0] = x;
    d[1] = y;
}
void get2(const int32_t* d, int* x, int* y) {
    if (x) *x = d[0];
    if (y) *y = d[1];
}
void nvnDlssParametersSetInputSubrectangleSize(NVNdlssParameters* p, int w, int h) { set2(pd(p)->in_size, w, h); }
void nvnDlssParametersGetInputSubrectangleSize(const NVNdlssParameters* p, int* w, int* h) { get2(pd(p)->in_size, w, h); }
void nvnDlssParametersSetInputColorSubrectangleBase(NVNdlssParameters* p, int x, int y) { set2(pd(p)->color_base, x, y); }
void nvnDlssParametersGetInputColorSubrectangleBase(const NVNdlssParameters* p, int* x, int* y) { get2(pd(p)->color_base, x, y); }
void nvnDlssParametersSetDepthSubrectangleBase(NVNdlssParameters* p, int x, int y) { set2(pd(p)->depth_base, x, y); }
void nvnDlssParametersGetDepthSubrectangleBase(const NVNdlssParameters* p, int* x, int* y) { get2(pd(p)->depth_base, x, y); }
void nvnDlssParametersSetMotionVectorSubrectangleBase(NVNdlssParameters* p, int x, int y) { set2(pd(p)->mv_base, x, y); }
void nvnDlssParametersGetMotionVectorSubrectangleBase(const NVNdlssParameters* p, int* x, int* y) { get2(pd(p)->mv_base, x, y); }
void nvnDlssParametersSetBiasCurrentColorSubrectangleBase(NVNdlssParameters* p, int x, int y) { set2(pd(p)->mask_base, x, y); }
void nvnDlssParametersGetBiasCurrentColorSubrectangleBase(const NVNdlssParameters* p, int* x, int* y) { get2(pd(p)->mask_base, x, y); }
void nvnDlssParametersSetOutputSubrectangleBase(NVNdlssParameters* p, int x, int y) { set2(pd(p)->out_base, x, y); }
void nvnDlssParametersGetOutputSubrectangleBase(const NVNdlssParameters* p, int* x, int* y) { get2(pd(p)->out_base, x, y); }
// NGX's on-screen debug overlays: kept, not drawn.
void nvnDlssParametersSetOverlayDebugLayer(NVNdlssParameters* p, int v) { pd(p)->overlay[0] = v; }
void nvnDlssParametersSetOverlayFullScreen(NVNdlssParameters* p, int v) { pd(p)->overlay[1] = v; }
void nvnDlssParametersSetOverlayShowNans(NVNdlssParameters* p, int v) { pd(p)->overlay[2] = v; }
void nvnDlssParametersSetOverlayJitterDebug(NVNdlssParameters* p, int v) { pd(p)->overlay[3] = v; }

// ---- evaluation -----------------------------------------------------------------------------------------------------------------------
void take_tex(Ref<Texture>& out, TextureViewInfo& view, bool& has_view, uint64_t tex, uint64_t v) {
    out = Ref<Texture>::share(host_of<Texture>(reinterpret_cast<const NVNtexture*>(tex)));
    has_view = v != 0 && out;
    view = has_view ? decode(reinterpret_cast<const NVNtextureView*>(v)) : TextureViewInfo{};
}

void nvnCommandBufferEvaluateDlss(NVNcommandBuffer* cmd, const NVNdlssNetwork* n, const NVNdlssParameters* params) {
    static std::atomic<bool> told_net{false}, told_tex{false};
    CommandBuffer* cb = host_of<CommandBuffer>(cmd);
    if (!cb || !params) return;
    Ref<DlssNet> net = net_of(n);
    if (!net) {
        if (!told_net.exchange(true)) NXLOG_W(kTag, "nvnCommandBufferEvaluateDlss on a network that is not initialized: nothing recorded (first)");
        return;
    }
    Ref<DlssEval> e(new DlssEval());
    e->net = net;
    e->p = *pd(params);
    e->out_w = net->out_w.load();
    e->out_h = net->out_h.load();
    take_tex(e->color, e->color_view, e->has_color_view, e->p.color, e->p.color_view);
    take_tex(e->output, e->output_view, e->has_output_view, e->p.output, e->p.output_view);
    take_tex(e->motion, e->motion_view, e->has_motion_view, e->p.motion, e->p.motion_view);
    take_tex(e->depth, e->depth_view, e->has_depth_view, e->p.depth, e->p.depth_view);
    take_tex(e->mask, e->mask_view, e->has_mask_view, e->p.mask, e->p.mask_view);
    if (!e->color || !e->output) {
        if (!told_tex.exchange(true)) NXLOG_W(kTag, "nvnCommandBufferEvaluateDlss without an input or output texture: nothing recorded (first)");
        return;
    }
    std::lock_guard<CmdLock> lk(cb->lock);
    if (!cb->recording || !cb->rec) return;
    cb->rec->hold(e.get());
    P_EvaluateDlss pl;
    pl.eval = e.get();
    cb->rec->emit(Op::EvaluateDlss, pl);
}

#define DLSS_PROCS(X)                                                                                                                     \
    X(nvnDlssNetworkBuilderSetDefaults) X(nvnDlssNetworkBuilderSetDevice) X(nvnDlssNetworkBuilderGetDevice) X(nvnDlssNetworkBuilderSetFlags) \
    X(nvnDlssNetworkBuilderGetFlags) X(nvnDlssNetworkBuilderSetMethod) X(nvnDlssNetworkBuilderGetMethod) X(nvnDlssNetworkBuilderSetPreset) \
    X(nvnDlssNetworkBuilderGetPreset) X(nvnDlssNetworkBuilderSetOutputResolution) X(nvnDlssNetworkBuilderGetOutputResolution)            \
    X(nvnDlssNetworkBuilderSetMaxEvaluationEntries) X(nvnDlssNetworkBuilderGetMaxEvaluationEntries) X(nvnDlssNetworkBuilderSetStorage)     \
    X(nvnDlssNetworkBuilderGetMemory) X(nvnDlssNetworkBuilderGetMemorySize) X(nvnDlssNetworkBuilderSetTextureIDs)                        \
    X(nvnDlssNetworkBuilderGetTextureIDs) X(nvnDlssNetworkBuilderGetTexturePool) X(nvnDlssNetworkBuilderGetOptimalSettings)              \
    X(nvnDlssNetworkBuilderGetStorageSize) X(nvnDlssNetworkInitialize) X(nvnDlssNetworkFinalize) X(nvnDlssNetworkResize)                 \
    X(nvnDlssParametersSetDefaults) X(nvnDlssParametersSetInputColor) X(nvnDlssParametersSetOutputColor) X(nvnDlssParametersSetMotionVectors) \
    X(nvnDlssParametersSetDepth) X(nvnDlssParametersSetBiasCurrentColorMask) X(nvnDlssParametersSetExposureTexture)                      \
    X(nvnDlssParametersSetJitterOffset) X(nvnDlssParametersGetJitterOffset) X(nvnDlssParametersSetMotionVectorScale)                      \
    X(nvnDlssParametersGetMotionVectorScale) X(nvnDlssParametersSetPreExposure) X(nvnDlssParametersGetPreExposure)                       \
    X(nvnDlssParametersSetHistoryReset) X(nvnDlssParametersGetHistoryReset) X(nvnDlssParametersSetEvaluationIndex)                       \
    X(nvnDlssParametersGetEvaluationIndex) X(nvnDlssParametersSetInputSubrectangleSize) X(nvnDlssParametersGetInputSubrectangleSize)      \
    X(nvnDlssParametersSetInputColorSubrectangleBase) X(nvnDlssParametersGetInputColorSubrectangleBase)                                   \
    X(nvnDlssParametersSetDepthSubrectangleBase) X(nvnDlssParametersGetDepthSubrectangleBase)                                             \
    X(nvnDlssParametersSetMotionVectorSubrectangleBase) X(nvnDlssParametersGetMotionVectorSubrectangleBase)                               \
    X(nvnDlssParametersSetBiasCurrentColorSubrectangleBase) X(nvnDlssParametersGetBiasCurrentColorSubrectangleBase)                       \
    X(nvnDlssParametersSetOutputSubrectangleBase) X(nvnDlssParametersGetOutputSubrectangleBase) X(nvnDlssParametersSetOverlayDebugLayer)  \
    X(nvnDlssParametersSetOverlayFullScreen) X(nvnDlssParametersSetOverlayShowNans) X(nvnDlssParametersSetOverlayJitterDebug)            \
    X(nvnCommandBufferEvaluateDlss)

}  // namespace

namespace be {

DlssNet::~DlssNet() {
    if (gpu && free_gpu) free_gpu(gpu);
}

// NGX (subsdk0 0x7D200 via 0xAA760): the method picks a perf-quality mode (0 -> Balanced, 1 -> UltraPerformance, 2 -> DLAA; the driver
// maps anything else to Balanced), whose scale (0.58, 1/3, 1) gives the optimal size (rounded to nearest). The dynamic range is 0.5 .. 1 of
// the output for a scale inside it, 0.99 .. 1 at scale 1, and the optimal size alone outside it. Float arithmetic as the module's.
DlssOptimal dlss_optimal(int out_w, int out_h, int method) {
    float scale;
    std::memcpy(&scale, method == 1 ? "\xab\xaa\xaa\x3e" : method == 2 ? "\x00\x00\x80\x3f" : "\xe1\x7a\x14\x3f", 4);   // 1/3, 1, 0.58
    const float w = static_cast<float>(static_cast<uint32_t>(std::max(out_w, 0))), h = static_cast<float>(static_cast<uint32_t>(std::max(out_h, 0)));
    const bool out_of_range = scale < 0.5f || scale > 1.0f;
    float lo = out_of_range ? scale : 0.5f, hi = out_of_range ? scale : 1.0f;
    if (scale == 1.0f) {
        hi = 1.0f;
        lo = 0.99f;
    }
    auto r = [](float v) { return static_cast<int>(std::lround(v)); };
    DlssOptimal o;
    o.opt_w = r(scale * w);
    o.opt_h = r(scale * h);
    o.max_w = r(hi * w);
    o.max_h = r(hi * h);
    o.min_w = r(lo * w);
    o.min_h = r(lo * h);
    return o;
}

std::vector<std::string> dlss_proc_names() {
    std::vector<std::string> v;
#define DLSS_NAME(fn) v.emplace_back(#fn);
    DLSS_PROCS(DLSS_NAME)
#undef DLSS_NAME
    return v;
}

}  // namespace be

void register_api_dlss() {
#define DLSS_REG(fn) NVN2_REG(fn);
    DLSS_PROCS(DLSS_REG)
#undef DLSS_REG
}

}  // namespace nxrt::nvn
