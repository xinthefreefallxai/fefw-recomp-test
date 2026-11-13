// SPDX-License-Identifier: GPL-3.0-or-later
// Temporal upscaling: the camera from the G-buffer uniform block, the input pass (motion vectors, depth and colour copies) and the
// backend call (temporal.h).
#include "extras/temporal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "nxrt/config.h"
#include "nxrt/extras.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"

namespace nxrt::extras {
namespace {

constexpr const char* kTag = "temporal";

#include "extras/temporal_shaders.inc"

// ---- 4x4 matrices, column-major (m[c * 4 + r]), column vectors ------------------------------------------------------------------
struct M4 {
    double m[16] = {};
    double& at(int r, int c) { return m[c * 4 + r]; }
    double at(int r, int c) const { return m[c * 4 + r]; }
};
M4 mul(const M4& a, const M4& b) {
    M4 o;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a.at(r, k) * b.at(k, c);
            o.at(r, c) = s;
        }
    return o;
}
bool inverse(const M4& in, M4& out) {   // Gauss-Jordan with partial pivoting
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            a[r][c] = in.at(r, c);
            a[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-300) return false;
        if (p != c) for (int k = 0; k < 8; ++k) std::swap(a[p][k], a[c][k]);
        const double d = a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = a[r][c];
            if (f != 0.0) for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out.at(r, c) = a[r][c + 4];
    return true;
}
void to_float(const M4& a, float* out) { for (int i = 0; i < 16; ++i) out[i] = static_cast<float>(a.m[i]); }

// The camera of one frame, from the G-buffer vertex shaders' block c3 (see temporal.h).
struct Camera {
    M4 vpj, vpu, view, proj_u;
    double jit[2] = {};          // the projection's jitter in NDC (ndc_jittered = ndc + jit)
    double zsgn = -1;            // sign of the view-space z in front of the camera (-1: right-handed, looking down -z)
    double p22 = 0, p23 = 0, p32 = -1;
    double fwd[3] = {}, pos[3] = {}, right[3] = {}, up[3] = {};
};

bool parse_camera(const uint32_t* w, size_t n, Camera& cam) {
    if (n < 68) return false;
    float f[68];
    std::memcpy(f, w, sizeof f);
    for (float x : f) if (!std::isfinite(x)) return false;
    for (int i = 0; i < 16; ++i) cam.vpj.m[i] = f[i];
    for (int i = 0; i < 16; ++i) cam.view.m[i] = f[52 + i];
    const M4& v = cam.view;
    // the view matrix: an orthonormal rotation, translation, last row 0 0 0 1
    if (std::fabs(v.at(3, 0)) > 1e-4 || std::fabs(v.at(3, 1)) > 1e-4 || std::fabs(v.at(3, 2)) > 1e-4 || std::fabs(v.at(3, 3) - 1.0) > 1e-4) return false;
    for (int r = 0; r < 3; ++r) {
        double len = 0;
        for (int c = 0; c < 3; ++c) len += v.at(r, c) * v.at(r, c);
        if (std::fabs(len - 1.0) > 0.02) return false;
    }
    M4 vinv;
    if (!inverse(v, vinv)) return false;
    const M4 p = mul(cam.vpj, vinv);
    // a perspective projection: w = +-z, no x / y in w, no cross terms between x and y
    if (std::fabs(std::fabs(p.at(3, 2)) - 1.0) > 0.01 || std::fabs(p.at(3, 0)) > 1e-3 || std::fabs(p.at(3, 1)) > 1e-3 || std::fabs(p.at(3, 3)) > 1e-3) return false;
    if (std::fabs(p.at(0, 1)) > 1e-3 || std::fabs(p.at(1, 0)) > 1e-3 || p.at(0, 0) < 0.05 || std::fabs(p.at(1, 1)) < 0.05) return false;
    cam.p32 = p.at(3, 2);
    cam.zsgn = cam.p32 > 0 ? 1.0 : -1.0;
    cam.jit[0] = p.at(0, 2) / cam.p32;
    cam.jit[1] = p.at(1, 2) / cam.p32;
    cam.p22 = p.at(2, 2);
    cam.p23 = p.at(2, 3);
    cam.vpu = cam.vpj;
    for (int c = 0; c < 4; ++c) {
        cam.vpu.at(0, c) -= cam.jit[0] * cam.vpj.at(3, c);
        cam.vpu.at(1, c) -= cam.jit[1] * cam.vpj.at(3, c);
    }
    cam.proj_u = mul(cam.vpu, vinv);
    for (int i = 0; i < 3; ++i) {
        cam.right[i] = v.at(0, i);
        cam.up[i] = v.at(1, i);
        cam.fwd[i] = cam.zsgn * v.at(2, i);
        cam.pos[i] = vinv.at(i, 3);
    }
    return true;
}

// ---- state ------------------------------------------------------------------------------------------------------------------------
struct Target {
    gfx::Image img;
    bool ready = false;   // left the UNDEFINED layout
};

struct State {
    std::mutex mu;
    bool have_cam = false;
    Camera cam;                    // this frame's (kept from the first valid G-buffer draw)
    bool have_prev = false;
    Camera prev;
    uint64_t gbuffer_blocks = 0, rejected_blocks = 0;
    bool logged_cam = false, logged_reject = false, logged_nocam = false, logged_jitter = false, logged_bigjit = false, logged_backend_fail = false;
    uint64_t evaluations = 0, resets = 0, no_cam_frames = 0;
    double last_jit_px[2] = {};
    uint32_t phases = 0;
    double phase_seen[32][2] = {};
    std::chrono::steady_clock::time_point last_eval{};
    UpscaleMode last_mode = UpscaleMode::Off;
    VkExtent2D last_render = {}, last_out = {};
    // GPU objects
    gfx::Device* dev = nullptr;
    bool init_tried = false, init_ok = false;
    VkSampler nearest = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    static constexpr uint32_t kRing = 8;
    VkDescriptorSet sets[kRing] = {};
    uint32_t next = 0;
    Target color, depth, motion, output;
};
State& st() {
    static State s;
    return s;
}

std::atomic<float> g_game_jit_x{0.0f}, g_game_jit_y{0.0f};

struct Push {
    float reproj[16];
    float jitter[2];
    float size[2];
    float zmap[2];
    float pad[2];
};
static_assert(sizeof(Push) == 96, "push block");

void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
             VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

bool init_gpu(State& s, gfx::Device& dev) {
    if (s.init_tried) return s.init_ok;
    s.init_tried = true;
    s.dev = &dev;
    VkDevice d = dev.device();
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(d, &sci, nullptr, &s.nearest) != VK_SUCCESS) return false;
    const VkDescriptorSetLayoutBinding b[5] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 5;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(d, &li, nullptr, &s.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &s.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d, &pli, nullptr, &s.layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof(kTemporalPrepComp);
    mi.pCode = kTemporalPrepComp;
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d, &mi, nullptr, &mod) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
    ci.layout = s.layout;
    const VkResult pr = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &s.pipe);
    vkDestroyShaderModule(d, mod, nullptr);
    if (pr != VK_SUCCESS) return false;
    const VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * State::kRing}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 * State::kRing}};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = State::kRing;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d, &pci, nullptr, &s.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[State::kRing];
    std::fill(std::begin(layouts), std::end(layouts), s.set_layout);
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s.pool;
    ai.descriptorSetCount = State::kRing;
    ai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(d, &ai, s.sets) != VK_SUCCESS) return false;
    s.init_ok = true;
    return true;
}

// (Re)makes a target of the given size and format. The GPU must be idle with the old image (sizes change rarely: wait_idle).
bool ensure(State& s, Target& t, VkExtent2D e, VkFormat fmt, const char* name) {
    if (t.img && t.img.extent.width == e.width && t.img.extent.height == e.height && t.img.format == fmt) return true;
    if (t.img) {
        s.dev->wait_idle();
        s.dev->destroy_image(t.img);
    }
    gfx::ImageDesc d;
    d.format = fmt;
    d.width = e.width;
    d.height = e.height;
    d.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    d.name = name;
    std::string why;
    if (s.dev->create_image(d, &t.img, &why) != VK_SUCCESS) {
        NXLOG_E(kTag, "%s %ux%u: image not made (%s)", name, e.width, e.height, why.c_str());
        t.img = {};
        return false;
    }
    t.ready = false;
    return true;
}

TemporalImage as_input(const Target& t) {
    TemporalImage o;
    o.image = t.img.image;
    o.view = t.img.view;
    o.format = t.img.format;
    o.width = t.img.extent.width;
    o.height = t.img.extent.height;
    o.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return o;
}

const char* mode_name(UpscaleMode m) {
    switch (m) {
        case UpscaleMode::Fsr3: return "FSR 3";
        case UpscaleMode::Dlss: return "DLSS";
        case UpscaleMode::Dlaa: return "DLAA";
        case UpscaleMode::Xess: return "XeSS";
        default: return "?";
    }
}

}  // namespace

void temporal_note_game_jitter(float x_ndc, float y_ndc) {
    g_game_jit_x.store(x_ndc, std::memory_order_relaxed);
    g_game_jit_y.store(y_ndc, std::memory_order_relaxed);
}

bool temporal_wanted() { return upscale_temporal(); }

namespace {
double radical_inverse(uint32_t i, uint32_t base) {
    double f = 1.0, r = 0.0;
    while (i) {
        f /= base;
        r += f * static_cast<double>(i % base);
        i /= base;
    }
    return r;
}
}  // namespace

// The game's own jitter does not reach the G-buffer camera (see temporal.h), so the runtime jitters it: Halton(2, 3), 8 phases, +-0.5 px.
bool temporal_jitter_block(uint32_t* words, size_t n_words, uint32_t width, uint32_t height, uint64_t frame) {
    static const int mode = static_cast<int>(config_get_int("upscale", "jitter", 1));   // 0 never, 1 when the projection has no jitter of its own
    if (mode == 0 || !words || n_words < 68 || width < 16 || height < 16) return false;
    Camera c;
    if (!parse_camera(words, n_words, c)) return false;
    if (std::fabs(c.jit[0]) * width > 0.01 || std::fabs(c.jit[1]) * height > 0.01) return false;   // already jittered (by the game)
    const uint32_t idx = static_cast<uint32_t>(frame % 8) + 1;
    const double jx = (2.0 * radical_inverse(idx, 2) - 1.0) / width, jy = (2.0 * radical_inverse(idx, 3) - 1.0) / height;
    float f[16];
    std::memcpy(f, words, sizeof f);
    for (int col = 0; col < 4; ++col) {   // column-major: ndc.x += jx, ndc.y += jy (clip x += jx * w)
        f[col * 4 + 0] = static_cast<float>(f[col * 4 + 0] + jx * f[col * 4 + 3]);
        f[col * 4 + 1] = static_cast<float>(f[col * 4 + 1] + jy * f[col * 4 + 3]);
    }
    std::memcpy(words, f, sizeof f);
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        NXLOG_I(kTag, "jitter injected into the scene camera (Halton 2/3, 8 phases, %ux%u): first %+.4f %+.4f px", width, height, jx * width * 0.5,
                -jy * height * 0.5);
    return true;
}

void temporal_note_gbuffer(const uint32_t* words, size_t n_words) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.have_cam) return;
    ++s.gbuffer_blocks;
    Camera c;
    if (!words || !parse_camera(words, n_words, c)) {
        ++s.rejected_blocks;
        return;
    }
    s.cam = c;
    s.have_cam = true;
    if (!s.logged_cam) {
        s.logged_cam = true;
        NXLOG_I(kTag, "camera from the G-buffer uniform block (vertex slot 0: view-projection floats 0..15, view 52..67): projection x %.4f y %.4f, "
                      "z %.6g w-term %.4f, jitter %+.6f %+.6f NDC, camera at %.1f %.1f %.1f",
                c.proj_u.at(0, 0), c.proj_u.at(1, 1), c.p22, c.p23, c.jit[0], c.jit[1], c.pos[0], c.pos[1], c.pos[2]);
    }
}

bool temporal_camera_ready() {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.have_cam;
}

VkImageView temporal_evaluate(void* gfx_device, VkCommandBuffer cb, VkImageView color, VkImageView depth, VkImageLayout depth_layout, VkExtent2D render,
                              VkExtent2D out, bool depth_neg_one) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    const UpscaleMode mode = upscale_mode();
    if (!s.have_cam) {
        if (++s.no_cam_frames == 120 && !s.logged_nocam) {
            s.logged_nocam = true;
            NXLOG_W(kTag, "120 tone-map draws without a camera: no G-buffer draw held a valid view-projection + view block (%llu blocks seen, %llu rejected)",
                    static_cast<unsigned long long>(s.gbuffer_blocks), static_cast<unsigned long long>(s.rejected_blocks));
        }
        return VK_NULL_HANDLE;
    }
    const Camera cur = s.cam;
    s.have_cam = false;   // the next frame needs its own
    if (!gfx_device || !color || !depth || !render.width || !render.height || !out.width || !out.height) return VK_NULL_HANDLE;
    gfx::Device& dev = *static_cast<gfx::Device*>(gfx_device);
    if (!init_gpu(s, dev)) {
        if (!s.logged_backend_fail) {
            s.logged_backend_fail = true;
            NXLOG_E(kTag, "input pass could not be created: temporal upscaling off");
        }
        return VK_NULL_HANDLE;
    }
    TemporalBackend* be = mode == UpscaleMode::Fsr3   ? temporal_ffx(gfx_device)
                        : mode == UpscaleMode::Xess ? temporal_xess(gfx_device)
                                                    : temporal_dlss(gfx_device, mode == UpscaleMode::Dlaa);
    if (!be) return VK_NULL_HANDLE;
    if (!ensure(s, s.color, render, VK_FORMAT_R16G16B16A16_SFLOAT, "temporal colour") || !ensure(s, s.depth, render, VK_FORMAT_R32_SFLOAT, "temporal depth") ||
        !ensure(s, s.motion, render, VK_FORMAT_R16G16_SFLOAT, "temporal motion") || !ensure(s, s.output, out, VK_FORMAT_R16G16B16A16_SFLOAT, "temporal output"))
        return VK_NULL_HANDLE;

    // jitter: the projection's offset in NDC -> pixels (FSR convention, picture +y down; NDC +y is the top row)
    double jx = cur.jit[0], jy = cur.jit[1];
    double jpx = jx * render.width * 0.5, jpy = -jy * render.height * 0.5;
    if (std::fabs(jpx) > 1.0 || std::fabs(jpy) > 1.0) {   // an off-centre frustum, not a sub-pixel jitter
        if (!s.logged_bigjit) {
            s.logged_bigjit = true;
            NXLOG_W(kTag, "projection offset %.3f %.3f px is not a sub-pixel jitter: treated as none", jpx, jpy);
        }
        jx = jy = jpx = jpy = 0;
    }
    static const int jsign = static_cast<int>(config_get_int("upscale", "jitter_sign", 0));   // debug: bit 0 flips x, bit 1 flips y for the SDK
    const float sdk_jx = static_cast<float>((jsign & 1) ? -jpx : jpx), sdk_jy = static_cast<float>((jsign & 2) ? -jpy : jpy);
    if (!s.logged_jitter && (jpx != 0 || jpy != 0)) {
        s.logged_jitter = true;
        NXLOG_I(kTag, "jitter seen in the projection: %+.4f %+.4f px (render %ux%u); the game's TAA code last stored %+.6f %+.6f NDC", jpx, jpy, render.width,
                render.height, static_cast<double>(g_game_jit_x.load()), static_cast<double>(g_game_jit_y.load()));
    }
    if (s.phases < 32) {
        bool seen = false;
        for (uint32_t i = 0; i < s.phases; ++i) seen |= std::fabs(s.phase_seen[i][0] - jpx) < 1e-3 && std::fabs(s.phase_seen[i][1] - jpy) < 1e-3;
        if (!seen) {
            s.phase_seen[s.phases][0] = jpx;
            s.phase_seen[s.phases][1] = jpy;
            ++s.phases;
        }
    }

    // history: reset on the first frame, size or mode change and camera cuts
    bool reset = !s.have_prev || mode != s.last_mode || render.width != s.last_render.width || render.height != s.last_render.height ||
                 out.width != s.last_out.width || out.height != s.last_out.height;
    const auto now = std::chrono::steady_clock::now();
    double ms = s.evaluations ? std::chrono::duration<double, std::milli>(now - s.last_eval).count() : 16.7;
    if (ms > 250.0) reset = true;   // the scene was not drawn for a while (menus, loading)
    if (s.have_prev) {
        const double dot = cur.fwd[0] * s.prev.fwd[0] + cur.fwd[1] * s.prev.fwd[1] + cur.fwd[2] * s.prev.fwd[2];
        if (dot < 0.9) reset = true;
    }
    const Camera& prev = reset ? cur : s.prev;
    M4 inv_cur_j, inv_cur_u, inv_prev_u, inv_proj;
    if (!inverse(cur.vpj, inv_cur_j) || !inverse(cur.vpu, inv_cur_u) || !inverse(prev.vpu, inv_prev_u) || !inverse(cur.proj_u, inv_proj)) return VK_NULL_HANDLE;
    const M4 reproj = mul(prev.vpu, inv_cur_j);

    // input pass
    VkDevice d = dev.device();
    const VkPipelineStageFlags2 any = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    const VkAccessFlags2 rw = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    for (Target* t : {&s.color, &s.depth, &s.motion}) {
        barrier(cb, t->img.image, t->ready ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, any, rw, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_WRITE_BIT);
        t->ready = true;
    }
    VkDescriptorSet set = s.sets[s.next];
    s.next = (s.next + 1) % State::kRing;
    VkDescriptorImageInfo ii[5] = {{s.nearest, depth, depth_layout},
                                   {s.nearest, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {VK_NULL_HANDLE, s.motion.img.view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, s.depth.img.view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, s.color.img.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[5];
    for (uint32_t i = 0; i < 5; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(d, 5, w, 0, nullptr);
    Push pc = {};
    to_float(reproj, pc.reproj);
    pc.jitter[0] = static_cast<float>(jx);
    pc.jitter[1] = static_cast<float>(jy);
    pc.size[0] = static_cast<float>(render.width);
    pc.size[1] = static_cast<float>(render.height);
    pc.zmap[0] = depth_neg_one ? 2.0f : 1.0f;
    pc.zmap[1] = depth_neg_one ? -1.0f : 0.0f;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cb, s.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (render.width + 7) / 8, (render.height + 7) / 8, 1);
    for (Target* t : {&s.color, &s.depth, &s.motion})
        barrier(cb, t->img.image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, any, rw);
    barrier(cb, s.output.img.image, s.output.ready ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, any, rw, any, rw);
    s.output.ready = true;

    // the backend
    TemporalFrame f;
    f.color = as_input(s.color);
    f.depth = as_input(s.depth);
    f.motion = as_input(s.motion);
    f.output = as_input(s.output);
    f.render = render;
    f.out = out;
    f.jitter_px[0] = sdk_jx;
    f.jitter_px[1] = sdk_jy;
    f.mv_scale[0] = static_cast<float>(render.width);
    f.mv_scale[1] = static_cast<float>(render.height);
    f.reset = reset;
    f.frame_ms = static_cast<float>(std::clamp(ms, 1.0, 100.0));
    // near / far from the projection: NDC z(t) = (p22 * zsgn * t + p23) / (p32 * zsgn * t) for view distance t
    const double a32 = std::fabs(cur.p32);
    auto ndc_at = [&](double t) { return (cur.p22 * cur.zsgn * t + cur.p23) / (a32 * t); };
    const bool inverted = ndc_at(1.0) > ndc_at(1000.0);
    const double z_near_ndc = inverted ? 1.0 : (depth_neg_one ? -1.0 : 0.0), z_far_ndc = inverted ? (depth_neg_one ? -1.0 : 0.0) : 1.0;
    auto dist_at = [&](double z) {
        const double den = z * a32 - cur.p22 * cur.zsgn;
        const double t = std::fabs(den) > 1e-12 ? cur.p23 / den : 1e9;
        return (t > 0 && std::isfinite(t)) ? t : 1e9;
    };
    f.near_z = static_cast<float>(dist_at(z_near_ndc));
    f.far_z = static_cast<float>(std::min(dist_at(z_far_ndc), 1e9));
    f.fov_y = static_cast<float>(2.0 * std::atan(1.0 / std::fabs(cur.proj_u.at(1, 1))));
    f.aspect = static_cast<float>(std::fabs(cur.proj_u.at(1, 1) / cur.proj_u.at(0, 0)));
    f.sharpness = upscale_sharpness();
    to_float(cur.proj_u, f.view_to_clip);
    to_float(inv_proj, f.clip_to_view);
    const M4 c2p = mul(prev.vpu, inv_cur_u);
    M4 p2c;
    if (!inverse(c2p, p2c)) p2c = c2p;
    to_float(c2p, f.clip_to_prev_clip);
    to_float(p2c, f.prev_clip_to_clip);
    for (int i = 0; i < 3; ++i) {
        f.cam_pos[i] = static_cast<float>(cur.pos[i]);
        f.cam_up[i] = static_cast<float>(cur.up[i]);
        f.cam_right[i] = static_cast<float>(cur.right[i]);
        f.cam_fwd[i] = static_cast<float>(cur.fwd[i]);
    }
    if (s.evaluations == 0)
        NXLOG_I(kTag, "%s: render %ux%u -> %ux%u, depth %s, near %.3f far %.1f, vertical fov %.1f deg, motion vectors from depth + camera (camera motion only)",
                be->name(), render.width, render.height, out.width, out.height, inverted ? "reversed" : "forward", static_cast<double>(f.near_z),
                static_cast<double>(f.far_z), static_cast<double>(f.fov_y) * 57.29578);
    if (!be->evaluate(cb, f)) return VK_NULL_HANDLE;
    barrier(cb, s.output.img.image, VK_IMAGE_LAYOUT_GENERAL, any, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    s.resets += reset ? 1 : 0;
    ++s.evaluations;
    if (s.evaluations % 600 == 0)
        NXLOG_I(kTag, "%s: %llu frames, %llu history resets, %u jitter phases seen, last jitter %+.3f %+.3f px", be->name(),
                static_cast<unsigned long long>(s.evaluations), static_cast<unsigned long long>(s.resets), s.phases, jpx, jpy);
    s.last_jit_px[0] = jpx;
    s.last_jit_px[1] = jpy;
    s.last_eval = now;
    s.last_mode = mode;
    s.last_render = render;
    s.last_out = out;
    s.prev = cur;
    s.have_prev = true;
    (void)mode_name;
    return s.output.img.view;
}

}  // namespace nxrt::extras
