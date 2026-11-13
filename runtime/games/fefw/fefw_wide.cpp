// SPDX-License-Identifier: GPL-3.0-or-later
// Fire Emblem: Fortune's Weave: outputs wider than 16:9 ([fefw] ultrawide = auto | off). Offsets are in main of update 131072 (build
// DB93B012...); the sites are in recomp/games/fefw/main.sites.
//
// Credit: the camera, UI, scissor, world-marker, 2D-effect and movie sites and what to change there were found by Kompas9, whose
// MIT-licensed "Ultrawide 3440x1440 v1.1" IPS for this build (github.com/Kompas9/Switch-Game-Patches, games/Fire-Emblem-Fortunes-Weave;
// THIRD_PARTY_NOTICES.md) does it for 43:18 with code caves. Here they are host hooks on the game's own code, with every value computed from
// the output's aspect, so any width works; the IPS itself is not used or shipped. The backdrop and off-screen indicator hooks are this
// port's own.
//
// The game draws for a 16:9 screen at any output size: the 3D camera has a fixed 16:9 aspect and the UI layouts are 1920x1080 with the
// viewport the whole output, so a wider output stretches everything sideways. With k = (output w / h) / (16 / 9):
//   camera       the perspective camera's update (0xb51420): a type-0 camera whose aspect (+0x70, +0x74) is 16 : 9 gets 16k : 9, so
//                the view gets wider at the same vertical field of view.
//   UI           the 2D draw info's projection setter (0x5769d0) copies the layout's 4x4 at x0 + 0x10; a 1920x1080 ortho (1/960, +-1/540,
//                no x translation, w 1) gets 1/(960k): the layout covers the centred 16:9 part of the output, the 3D picture the rest.
//   backdrops    a picture or window that spans the whole 1920 layout width (dims, menu backdrops) gets x * k in the model-view its
//                Calculate (0x595740, 0x5af080) puts in its constant buffer, so it still covers the output; a full-height one on one edge
//                (side gradients) is stretched to the output's edge there. Children and text keep theirs. (Not in Kompas9's patch.)
//   scissor      the scissor pane (0x5975a0) works out its rectangle in viewport pixels from the layout size, not the matrix: when the
//                draw info's x scale is ours the left edge and width are scaled by 1/k about the viewport's centre.
//   markers      world-to-screen into 1920x1080 UI units (x * 960 + 960) at 13 sites: x * k, as the camera is k times wider. The
//                off-screen indicator (0x18f3760, x * 960 against its box; not in Kompas9's patch) gets the same.
//   2D effects   the effect draw (0xb76680) builds its own ortho from the effect's screen size: a 16:9 one gets 1/k, and the two full-
//                target scissors of the call are cut to the centred 16:9 part (particles off the 16:9 screen stay hidden, as on the
//                console); the scissor is put back to the whole target at its end.
//   stat chart   the unit screen's radar polygons (0x17d8e80) are built by the game from the layout's Locator panes in 1920x1080 layout pixels
//                and drawn in its own 2D primitive buffer over the whole output, not through a pane: their x gets 960 + (x - 960) / k.
//   movies       the movie draw (0x1904aa0) clears the target to opaque black and draws into the centred 16:9 viewport; a midhook in
//                front of its tail-called draw draws, puts the viewport back to the whole target and returns.
// Outputs narrower than 16:9 are not widened: fefw_res.cpp fits the output to 16:9 inside them and the window letterboxes it. Wider than
// 3840 (the game's window textures; 5120x1440) fefw_res.cpp keeps the shape at 3840 wide and the window scales it up.
#include "fefw_game.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::fefw {

namespace {

constexpr const char* kTag = "fefw";

// ---- sites (module offsets) ---------------------------------------------------------------------------------------------------------
constexpr uint64_t kCameraUpdate = 0xb51440;      // after the prologue of 0xb51420: x0 = the camera
constexpr uint64_t kCamType = 0xc;                // u32: 0 = the perspective cameras the patch widens
constexpr uint64_t kCamAspect = 0x70;             // f32 x2: aspect numerator, denominator (16, 9)
constexpr uint64_t kProjSet = 0x576a10;           // `ret` of 0x5769d0: x0 + 0x10 = the 4x4 just copied (row major)
constexpr uint64_t kPictureCalc = 0x595784;       // nn::ui2d Picture::Calculate (0x595740) after its LoadMtx: x19 = the pane, x20 = the draw info
constexpr uint64_t kWindowCalc = 0x5af0b8;        // nn::ui2d Window::Calculate (0x5af080) after its LoadMtx: x20 = the pane, x19 = the draw info
constexpr uint64_t kDrawModelView = 0x80;         // in the draw info: 3x4, the model-view of the pane being calculated
constexpr uint64_t kPaneSize = 0x50;              // f32 w, h
constexpr uint64_t kPaneBase = 0x5b;              // u8: base position, bits 0-1 horizontal (0 centre, 1 left, 2 right)
constexpr uint64_t kPaneGlbMtx = 0x70;            // 3x4 row major
constexpr uint64_t kScissor = 0x5976d4;           // in 0x5975a0: s3 viewport x, s2 viewport w, s5 left edge, s7 width; x1 = draw info
constexpr uint64_t kMarkersS1[] = {0xb81bfc, 0xd939cc, 0xd943c4, 0xd94528, 0xd94670, 0xd947b8, 0xd94900, 0xd94a44,
                                   0xd94b88, 0xd94ccc, 0x1912700, 0x19131ec};   // `fmadd s1, s1, s2, s2` (s2 = 960)
constexpr uint64_t kMarkerS2 = 0xa7f148;          // `fmadd s2, s2, s3, s3` (s3 = 960)
constexpr uint64_t kEdgeMarker = 0x18f38c0;       // `fmul s3, s2, s1` (s2 = 960) in the off-screen indicator 0x18f3760
constexpr uint64_t kFxOrtho = 0xb768d4;           // in 0xb76680: x22 = the effect's screen size (s32 w, h), the ortho at sp + 0x80
constexpr uint64_t kFxScissor1 = 0xb76bb0;        // `blr x8`: SetScissor(x0 = command wrapper, 0, 0, w, h) (vtable + 0x58)
constexpr uint64_t kFxScissor2 = 0xb76cfc;
constexpr uint64_t kFxEnd = 0xb76d9c;             // the epilogue
constexpr uint64_t kStatGraph = 0x17d90b0;        // in 0x17d8e80 (the stat radar chart): after the 8 ring vertices are computed; sp + 0x20 = 8 x (x, y), sp + 0x10 = centre x
constexpr uint64_t kMovieTarget = 0x1904b5c;      // in 0x1904aa0, after the target is set: [x19] = command wrapper, x20 + 0x10 = s32 w, h
constexpr uint64_t kMovieDraw = 0x1904c20;        // midhook: the draw is tail-called next (vtable + 0x110 with 3, 0)

// the command wrapper's virtual functions (used by 0xb76680 and 0x1904aa0)
constexpr uint64_t kVfViewport = 0x48;            // (x, y, w, h)
constexpr uint64_t kVfScissor = 0x58;             // (x, y, w, h)
constexpr uint64_t kVfClearColor = 0x90;          // (index, const float rgba[4]*)
constexpr uint64_t kVfDraw = 0x110;               // the movie's draw (3, 0)

constexpr uint32_t kInv960 = 0x3a888889;          // 1/960: a 1920 wide ortho
constexpr uint32_t kInv540 = 0x3af2b9d6;          // 1/540: 1080 high
constexpr uint32_t kOne = 0x3f800000;

enum Counter { kCCamera, kCUi, kCBackdrop, kCScissor, kCMarker, kCEdge, kCFxOrtho, kCFxScissor, kCMovie, kCStatGraph, kCCount };
constexpr const char* kCounterNames[kCCount] = {"camera",     "UI projection", "full-width pane",   "list scissor", "world marker",
                                               "off-screen marker", "2D effect", "2D effect scissor", "movie", "stat chart"};
static_assert(kCCount == WideInfo::kCounters, "fefw_game.h WideInfo::kCounters");

struct Wide {
    uint64_t base = 0x7100000000ull;
    bool active = false;
    int out_w = 0, out_h = 0;
    float k = 1.0f;
    float inv_k = 1.0f;
    uint32_t ui_scale = 0;          // the bits of 1/(960k) written into UI projections
    float cam_aspect_num = 16.0f;   // 9 * w / h
    std::vector<uint32_t> hooks;
    std::atomic<uint64_t> count[kCCount] = {};
    std::atomic<bool> logged[kCCount] = {};
};

Wide& ws() {
    static Wide w;
    return w;
}

template <class T> T rdv(uint64_t a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof v);
    return v;
}
template <class T> void wrv(uint64_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof v); }
uint32_t fbits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// Counts a hook's work; the first time, a log line with what it did.
bool first(Counter which) {
    Wide& w = ws();
    w.count[which].fetch_add(1, std::memory_order_relaxed);
    return !w.logged[which].load(std::memory_order_relaxed) && !w.logged[which].exchange(true);
}

// The guest function in slot `slot` of the object's vtable, or 0.
uint64_t vfn(uint64_t obj, uint64_t slot) {
    if (!obj || !mem::is_mapped(obj, 8)) return 0;
    const uint64_t vt = rdv<uint64_t>(obj);
    if (!vt || !mem::is_mapped(vt + slot, 8)) return 0;
    return rdv<uint64_t>(vt + slot);
}

// Calls fn(x0..x4) on the guest thread's context and puts every register back: the site's code goes on as if nothing ran.
void call_keep(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    const ::Ctx saved = *c;
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[3] = a3, c->x[4] = a4;
    dispatch_call(c, fn);
    *c = saved;
}

// The centred part of a w wide target that a 16:9 picture covers once the target is k times wider: x and width.
void centred(int32_t w, float inv_k, int32_t* x, int32_t* cw) {
    const int32_t inner = std::clamp(static_cast<int32_t>(std::lround(w * inv_k)), 1, w);
    *x = (w - inner) / 2;
    *cw = w - 2 * *x;
}

// ---- camera ------------------------------------------------------------------------------------------------------------------------------
void hook_camera(::Ctx* c, uint64_t, void*) {
    Wide& w = ws();
    const uint64_t cam = c->x[0];
    if (rdv<uint32_t>(cam + kCamType) != 0) return;
    if (rdv<float>(cam + kCamAspect) != 16.0f || rdv<float>(cam + kCamAspect + 4) != 9.0f) return;
    wrv<float>(cam + kCamAspect, w.cam_aspect_num);
    if (first(kCCamera)) NXLOG_I(kTag, "ultrawide: camera 0x%llx aspect 16:9 -> %.3f:9", static_cast<unsigned long long>(cam), w.cam_aspect_num);
}

// ---- UI projection and the scissor pane ---------------------------------------------------------------------------------------------------
void hook_projection(::Ctx* c, uint64_t, void*) {
    Wide& w = ws();
    const uint64_t m = c->x[0] + 0x10;
    if (rdv<uint32_t>(m) != kInv960 || (rdv<uint32_t>(m + 0x14) & 0x7fffffff) != kInv540) return;
    if (rdv<uint64_t>(m + 4) != 0 || (rdv<uint32_t>(m + 0xc) & 0x7fffffff) != 0 || rdv<uint32_t>(m + 0x3c) != kOne) return;
    wrv<uint32_t>(m, w.ui_scale);
    if (first(kCUi)) NXLOG_I(kTag, "ultrawide: 1920x1080 UI projection (draw info 0x%llx): x scale 1/960 -> 1/%.1f, the UI in the centred 16:9 area",
                             static_cast<unsigned long long>(c->x[0]), 960.0 * w.k);
}

// A picture or window that spans the whole 1920 wide layout (a dim, a menu backdrop) is drawn k times wider, so it covers the output: its
// Calculate copies the pane's global matrix into the draw info's model-view (LoadMtx) and from there into the pane's constant buffer; the
// model-view's x row is scaled in between. The pane's own matrix, and so its children, stay as they are. Text boxes are not pictures.
void widen_pane(uint64_t pane, uint64_t info) {
    Wide& w = ws();
    if (rdv<uint32_t>(info + 0x10) != w.ui_scale) return;
    const uint64_t m = pane + kPaneGlbMtx;
    const float m00 = rdv<float>(m), m01 = rdv<float>(m + 4), m03 = rdv<float>(m + 0xc), pw = rdv<float>(pane + kPaneSize);
    if (!(m00 > 0.0f) || m01 != 0.0f || !(pw > 0.0f)) return;
    const int hpos = rdv<uint8_t>(pane + kPaneBase) & 3;
    const float left = m03 + m00 * (hpos == 1 ? 0.0f : hpos == 2 ? -pw : -0.5f * pw), right = left + m00 * pw;
    // Full-width backdrops may zoom beyond both edges. A single-edge pane must
    // meet the edge, so the oversized, clipped minimap image is not stretched.
    const bool covers_width = left <= -958.0f && right >= 958.0f;
    const bool at_left = covers_width || std::fabs(left + 960.0f) <= 2.0f;
    const bool at_right = covers_width || std::fabs(right - 960.0f) <= 2.0f;
    if (!at_left && !at_right) return;
    // x' = a x + b in layout units: a full-width pane scales about the centre; a full-height one on one edge reaches the output's edge there
    float a = w.k, b = 0.0f;
    if (!(at_left && at_right)) {
        if (std::fabs(rdv<float>(pane + kPaneSize + 4) * rdv<float>(m + 0x14)) < 1000.0f) return;
        const float edge = 960.0f * w.k;
        if (at_left) a = (right + edge) / (right - left), b = right - a * right;
        else a = (right - left + (edge - right)) / (right - left), b = left - a * left;   // keeps the left edge, the right one goes to +edge
    }
    const uint64_t mv = info + kDrawModelView;
    for (int i = 0; i < 3; ++i) wrv<float>(mv + 4 * i, rdv<float>(mv + 4 * i) * a);
    wrv<float>(mv + 0xc, rdv<float>(mv + 0xc) * a + b);
    if (first(kCBackdrop))
        NXLOG_I(kTag, "ultrawide: full-width / edge pane (vtable 0x%llx, %.0fx%.0f, x %.0f..%.0f) drawn x %.0f..%.0f",
                static_cast<unsigned long long>(rdv<uint64_t>(pane) - w.base), pw, rdv<float>(pane + kPaneSize + 4), left, right, a * left + b, a * right + b);
}
void hook_picture_calc(::Ctx* c, uint64_t, void*) { widen_pane(c->x[19], c->x[20]); }
void hook_window_calc(::Ctx* c, uint64_t, void*) { widen_pane(c->x[20], c->x[19]); }

void hook_scissor(::Ctx* c, uint64_t, void*) {
    Wide& w = ws();
    if (rdv<uint32_t>(c->x[1] + 0x10) != w.ui_scale) return;
    const float centre = c->v[3].f[0] + c->v[2].f[0] * 0.5f;
    const float left = c->v[5].f[0], width = c->v[7].f[0];
    c->v[5].f[0] = centre + (left - centre) * w.inv_k;
    c->v[7].f[0] = width * w.inv_k;
    if (first(kCScissor))
        NXLOG_I(kTag, "ultrawide: scissor pane x %.1f w %.1f -> x %.1f w %.1f (viewport %.0f wide)", left, width, c->v[5].f[0], c->v[7].f[0],
                c->v[2].f[0]);
}

// ---- stat radar chart ---------------------------------------------------------------------------------------------------------------------
// The unit screen's stat charts (L_BasicStatusGraph / L_BattleStatusGraph / L_GrowthRateGraph) are not a pane: the game reads the layout's
// 8 Locator panes, builds the polygon in 1920x1080 layout pixels and pushes the triangles into its own 2D primitive buffer, which is mapped
// over the whole output, so on a wide output it ended up stretched and displaced from the circle drawn by the layout. Put the x values on
// the centred 16:9 area: x' = 960 + (x - 960) / k, for the 8 ring points and the centre point.
void hook_stat_graph(::Ctx* c, uint64_t, void*) {
    Wide& w = ws();
    const uint64_t sp = c->x[31];
    auto fix = [&](uint64_t a) { wrv<float>(a, 960.0f + (rdv<float>(a) - 960.0f) * w.inv_k); };
    for (int i = 0; i < 8; ++i) fix(sp + 0x20 + 8 * i);
    fix(sp + 0x10);
    if (first(kCStatGraph)) NXLOG_I(kTag, "ultrawide: stat chart polygon x scaled by %.4f about 960", w.inv_k);
}

// ---- world markers ------------------------------------------------------------------------------------------------------------------------
void hook_marker_s1(::Ctx* c, uint64_t, void*) {
    c->v[1].f[0] *= ws().k;
    if (first(kCMarker)) NXLOG_I(kTag, "ultrawide: world marker x scaled by %.4f", ws().k);
}
void hook_marker_s2(::Ctx* c, uint64_t, void*) {
    c->v[2].f[0] *= ws().k;
    if (first(kCMarker)) NXLOG_I(kTag, "ultrawide: world marker x scaled by %.4f", ws().k);
}
void hook_edge_marker(::Ctx* c, uint64_t, void*) {
    c->v[1].f[0] *= ws().k;
    if (first(kCEdge)) NXLOG_I(kTag, "ultrawide: off-screen indicator x scaled by %.4f", ws().k);
}

// ---- 2D effects -----------------------------------------------------------------------------------------------------------------------------
struct FxCall {
    uint64_t sp = 0;        // the effect draw's frame (0: none of ours)
    uint64_t cmd = 0;       // the command wrapper the scissor went to
    int32_t w = 0, h = 0;   // its target, for the scissor at the end
    bool clipped = false;
};
thread_local FxCall t_fx;

void hook_fx_ortho(::Ctx* c, uint64_t, void*) {
    Wide& w = ws();
    t_fx = FxCall();
    const uint64_t size = c->x[22], sp = c->x[31], m = sp + 0x80;
    const int32_t sw = rdv<int32_t>(size), sh = rdv<int32_t>(size + 4);
    if (sw <= 0 || sh <= 0 || static_cast<int64_t>(sw) * 9 != static_cast<int64_t>(sh) * 16) return;
    if (rdv<float>(m) != 2.0f / static_cast<float>(sw) || (rdv<uint32_t>(m + 0xc) & 0x7fffffff) != 0 || rdv<uint32_t>(m + 0x3c) != kOne) return;
    wrv<float>(m, rdv<float>(m) * w.inv_k);
    t_fx.sp = sp;
    if (first(kCFxOrtho)) NXLOG_I(kTag, "ultrawide: 2D effect screen %dx%d: x scale /%.4f", sw, sh, w.k);
}

void hook_fx_scissor(::Ctx* c, uint64_t, void*) {
    if (!t_fx.sp || t_fx.sp != c->x[31]) return;
    const int32_t x = static_cast<int32_t>(c->x[1]), y = static_cast<int32_t>(c->x[2]), tw = static_cast<int32_t>(c->x[3]),
                  th = static_cast<int32_t>(c->x[4]);
    if (x != 0 || y != 0 || tw <= 0 || th <= 0) return;
    int32_t cx = 0, cw = tw;
    centred(tw, ws().inv_k, &cx, &cw);
    c->x[1] = static_cast<uint32_t>(cx);
    c->x[3] = static_cast<uint32_t>(cw);
    t_fx.cmd = c->x[0];
    t_fx.w = tw;
    t_fx.h = th;
    t_fx.clipped = true;
    if (first(kCFxScissor)) NXLOG_I(kTag, "ultrawide: 2D effect scissor on a %dx%d target: x %d w %d", tw, th, cx, cw);
}

void hook_fx_end(::Ctx* c, uint64_t, void*) {
    const FxCall f = t_fx;
    t_fx = FxCall();
    if (!f.clipped || f.sp != c->x[31]) return;
    if (const uint64_t fn = vfn(f.cmd, kVfScissor)) call_keep(c, fn, f.cmd, 0, 0, static_cast<uint32_t>(f.w), static_cast<uint32_t>(f.h));
}

// ---- movies ------------------------------------------------------------------------------------------------------------------------------------
struct MovieCall {
    uint64_t sp = 0;
    int32_t w = 0, h = 0;
};
thread_local MovieCall t_movie;

void hook_movie_target(::Ctx* c, uint64_t, void*) {
    t_movie = MovieCall();
    const uint64_t cmd = rdv<uint64_t>(c->x[19]);
    const int32_t tw = rdv<int32_t>(c->x[20] + 0x10), th = rdv<int32_t>(c->x[20] + 0x14);
    if (tw <= 0 || th <= 0 || static_cast<int64_t>(tw) * 9 <= static_cast<int64_t>(th) * 16) return;   // only a target wider than 16:9
    const uint64_t clear = vfn(cmd, kVfClearColor), viewport = vfn(cmd, kVfViewport), draw = vfn(cmd, kVfDraw);
    if (!clear || !viewport || !draw) return;
    // opaque black over the whole target (the scissor is the whole target here), from a colour below the guest's stack
    {
        const ::Ctx saved = *c;
        const uint64_t sp = (c->x[31] - 0x40) & ~0xfull;
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        std::memcpy(reinterpret_cast<void*>(sp), black, sizeof black);
        c->x[31] = sp;
        c->x[0] = cmd, c->x[1] = 0, c->x[2] = sp;
        dispatch_call(c, clear);
        *c = saved;
    }
    const int32_t inner = std::min(tw, static_cast<int32_t>(std::lround(th * 16.0 / 9.0)));
    const int32_t x = (tw - inner) / 2;
    call_keep(c, viewport, cmd, static_cast<uint32_t>(x), 0, static_cast<uint32_t>(inner), static_cast<uint32_t>(th));
    t_movie.sp = c->x[31];
    t_movie.w = tw;
    t_movie.h = th;
    if (first(kCMovie)) NXLOG_I(kTag, "ultrawide: movie on a %dx%d target: black bars, picture at x %d, %dx%d", tw, th, x, inner, th);
}

// Midhook in front of the tail call: the function's epilogue (0x1904c24..0x1904c4c), the draw as the tail call makes it, then the viewport
// back to the whole target; the draw's registers are what the caller gets.
int hook_movie_draw(::Ctx* c, uint64_t, void*) {
    const MovieCall mc = t_movie;
    t_movie = MovieCall();
    if (!mc.sp || mc.sp != c->x[31]) return 0;
    const uint64_t cmd = rdv<uint64_t>(c->x[19]);
    const uint64_t draw = vfn(cmd, kVfDraw), viewport = vfn(cmd, kVfViewport);
    if (!draw || !viewport) return 0;
    const uint64_t sp = c->x[31];
    c->x[20] = rdv<uint64_t>(sp + 0x50), c->x[19] = rdv<uint64_t>(sp + 0x58);
    c->x[25] = rdv<uint64_t>(sp + 0x20);
    c->x[22] = rdv<uint64_t>(sp + 0x40), c->x[21] = rdv<uint64_t>(sp + 0x48);
    c->x[24] = rdv<uint64_t>(sp + 0x30), c->x[23] = rdv<uint64_t>(sp + 0x38);
    c->x[29] = rdv<uint64_t>(sp + 0x10), c->x[30] = rdv<uint64_t>(sp + 0x18);
    c->x[31] = sp + 0x60;
    c->x[0] = cmd, c->x[1] = 3, c->x[2] = 0, c->x[3] = draw;
    dispatch_call(c, draw);
    call_keep(c, viewport, cmd, 0, 0, static_cast<uint32_t>(mc.w), static_cast<uint32_t>(mc.h));
    return 1;
}

}  // namespace

std::string wide_mode() {
    static const std::string mode = [] {   // read at start
        std::string m = config_get_str("fefw", "ultrawide", "auto");
        for (char& ch : m) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (m == "0" || m == "false" || m == "no") m = "off";
        if (m == "1" || m == "true" || m == "yes" || m == "on") m = "auto";
        if (m != "off" && m != "auto") {
            NXLOG_W(kTag, "[fefw] ultrawide = %s is not known: auto", m.c_str());
            m = "auto";
        }
        return m;
    }();
    return mode;
}

bool wide_aspect(int w, int h) { return w > 0 && h > 0 && static_cast<double>(w) * 9.0 > static_cast<double>(h) * 16.0 * 1.01; }

void wide_install(uint64_t base, int out_w, int out_h) {
    Wide& w = ws();
    if (w.active || !wide_aspect(out_w, out_h)) return;
    w.base = base;
    w.out_w = out_w;
    w.out_h = out_h;
    w.k = static_cast<float>(static_cast<double>(out_w) * 9.0 / (static_cast<double>(out_h) * 16.0));
    w.inv_k = 1.0f / w.k;
    w.ui_scale = fbits(static_cast<float>(1.0 / (960.0 * static_cast<double>(w.k))));
    w.cam_aspect_num = static_cast<float>(9.0 * out_w / out_h);
    w.active = true;
    // [fefw] ultrawide_skip (debug): a comma-separated list of hook groups to leave out: camera, ui, backdrop, scissor, markers, fx, fxclip,
    // movie, statgraph
    std::string skip = "," + config_get_str("fefw", "ultrawide_skip", "") + ",";
    skip.erase(std::remove(skip.begin(), skip.end(), ' '), skip.end());
    auto on = [&](const char* g) { return skip.find(std::string(",") + g + ",") == std::string::npos; };
    auto add = [&](uint64_t off, InlineHookFn fn) { w.hooks.push_back(hook_add_inline(base + off, fn)); };
    if (on("camera")) add(kCameraUpdate, &hook_camera);
    if (on("ui")) add(kProjSet, &hook_projection);
    if (on("backdrop")) {
        add(kPictureCalc, &hook_picture_calc);
        add(kWindowCalc, &hook_window_calc);
    }
    if (on("scissor")) add(kScissor, &hook_scissor);
    if (on("markers")) {
        for (uint64_t off : kMarkersS1) add(off, &hook_marker_s1);
        add(kMarkerS2, &hook_marker_s2);
        add(kEdgeMarker, &hook_edge_marker);
    }
    if (on("statgraph")) add(kStatGraph, &hook_stat_graph);
    if (on("fx")) add(kFxOrtho, &hook_fx_ortho);
    if (on("fx") && on("fxclip")) {
        add(kFxScissor1, &hook_fx_scissor);
        add(kFxScissor2, &hook_fx_scissor);
        add(kFxEnd, &hook_fx_end);
    }
    if (on("movie")) {
        add(kMovieTarget, &hook_movie_target);
        w.hooks.push_back(hook_add_mid(base + kMovieDraw, &hook_movie_draw));
    }
    NXLOG_I(kTag, "ultrawide: output %dx%d (%.3f:1, %.4fx 16:9): wider camera, UI / movies / 2D effects in the centred %dx%d, %zu hooks "
                  "(sites after Kompas9's ultrawide patch)",
            out_w, out_h, static_cast<double>(out_w) / out_h, w.k, static_cast<int>(std::lround(out_h * 16.0 / 9.0)), out_h, w.hooks.size());
}

void wide_remove() {
    Wide& w = ws();
    for (uint32_t id : w.hooks)
        if (id) hook_remove(id);
    w.hooks.clear();
    w.active = false;
}

WideInfo wide_info() {
    const Wide& w = ws();
    WideInfo i;
    i.active = w.active;
    i.out_w = w.out_w;
    i.out_h = w.out_h;
    i.k = w.k;
    for (int n = 0; n < kCCount; ++n) {
        i.counts[n] = w.count[n].load(std::memory_order_relaxed);
        i.names[n] = kCounterNames[n];
    }
    return i;
}

}  // namespace nxrt::games::fefw
