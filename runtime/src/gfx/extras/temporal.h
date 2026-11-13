// SPDX-License-Identifier: GPL-3.0-or-later
// Temporal upscaling / anti-aliasing on the HDR scene before the game's tone map: AMD FSR 3.1 (FidelityFX API, temporal_ffx.cpp), NVIDIA DLSS /
// DLAA (Streamline, temporal_dlss.cpp) and Intel XeSS (libxess.dll, temporal_xess.cpp). The SDKs are loaded at run time; nothing is linked.
//
// Inputs (docs/S14.md, "Temporal upscaling"):
//   colour   the HDR scene the tone-map draw samples (the same interception as the spatial scene upscaler, nvn_replay.cpp)
//   depth    the depth target of the G-buffer pass (D24S8 in Pokopia), reversed-Z, NDC z = depth (NVN depth mode NEAR_IS_ZERO)
//   camera   the G-buffer vertex shaders' uniform block c3 (NVN uniform slot 0 of the vertex stage): floats 0..15 the view-projection
//            (column-major, jittered), floats 52..67 the view matrix. The projection is VP * inverse(V); its z-column x / y terms are the
//            sub-pixel jitter (zero without the game's TAA, about +-1 / width with [pkp] game_taa).
//   motion   made here by a compute pass from depth, the current jittered and the previous unjittered view-projection (camera motion only).
// The projection jitter is added by the runtime (temporal_jitter_block): the game's own TAA jitter does not reach the scene camera.
#pragma once
#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nxrt::extras {

// One frame's inputs for a backend: every image is owned by the temporal pass, in VK_IMAGE_LAYOUT_GENERAL, readable and writable by compute.
struct TemporalImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageUsageFlags usage = 0;
};
struct TemporalFrame {
    TemporalImage color, depth, motion, output;
    VkExtent2D render = {}, out = {};
    float jitter_px[2] = {};         // the projection's sub-pixel offset in pixels (FSR convention: +x right, +y down in the picture)
    float mv_scale[2] = {};          // motion vectors are in UV units: times the render size gives pixels
    bool reset = false;              // no usable history (first frame, camera cut, size change)
    float frame_ms = 16.7f;
    float near_z = 0.1f, far_z = 1000.0f, fov_y = 1.0f, aspect = 1.0f;
    float sharpness = 0.0f;          // 0..1, FSR's RCAS
    // camera (column-vector convention, stored column-major: the memory layout of a row-major matrix in row-vector convention, as Streamline wants)
    float view_to_clip[16] = {}, clip_to_view[16] = {}, clip_to_prev_clip[16] = {}, prev_clip_to_clip[16] = {};
    float cam_pos[3] = {}, cam_up[3] = {}, cam_right[3] = {}, cam_fwd[3] = {};
};

class TemporalBackend {
public:
    virtual ~TemporalBackend() = default;
    virtual const char* name() const = 0;
    // Records the upscale into cb (outside any render pass). False: nothing recorded (the caller falls back to the game's own picture).
    virtual bool evaluate(VkCommandBuffer cb, const TemporalFrame& f) = 0;
    // Why the last evaluate() failed, in the SDK's own words ("slEvaluateFeature failed (39)"), or "" when it has not. The F1 page shows it
    // beside the fallback it caused, so a player reads the real cause instead of a guess about missing DLLs.
    virtual const char* last_failure() const { return ""; }
};

// ---- what the replay calls (nvn_replay.cpp) --------------------------------------------------------------------------------------
bool temporal_wanted();   // a temporal mode is selected
// A G-buffer draw: `words` is the vertex stage's uniform slot 0 (bytes / 4 words). Keeps the first block that holds a valid camera this frame.
void temporal_note_gbuffer(const uint32_t* words, size_t n_words);
bool temporal_camera_ready();   // a camera was kept since the last evaluation
// The scene camera's jitter. Pokopia's own TAA jitter (0x7102574a18, written into a view object's projection at +0xc0) never reaches the
// G-buffer camera block (its view-projection stays unjittered with the enable forced, run b7), so the runtime jitters every vertex uniform
// slot 0 block that holds a valid camera: adds a Halton(2, 3) offset of the frame (8 phases, +-0.5 px of width x height) to the view-projection in
// `words` (a copy the draw then binds). False: not a camera block, already jittered, or [upscale] jitter = 0.
bool temporal_jitter_block(uint32_t* words, size_t n_words, uint32_t width, uint32_t height, uint64_t frame);
// The tone-map draw: colour (view in SHADER_READ_ONLY_OPTIMAL) and depth (depth-aspect view, layout depth_layout) of the scene, both at least
// `render` large; depth_neg_one: NVN depth mode NEAR_IS_MINUS_W (NDC z = 2 d - 1). Returns the result (`out` large, GENERAL layout, readable by
// fragment shaders after the recorded barrier) or null (nothing recorded: the draw keeps its own input).
VkImageView temporal_evaluate(void* gfx_device, VkCommandBuffer cb, VkImageView color, VkImageView depth, VkImageLayout depth_layout, VkExtent2D render,
                              VkExtent2D out, bool depth_neg_one);

// ---- backends --------------------------------------------------------------------------------------------------------------------
// gfx_device: the gfx::Device. Null when the SDK cannot be loaded or the GPU does not support it (logged once).
TemporalBackend* temporal_ffx(void* gfx_device);
TemporalBackend* temporal_dlss(void* gfx_device, bool dlaa);
TemporalBackend* temporal_xess(void* gfx_device);   // Intel XeSS (temporal_xess.cpp); null unless the device was made for it at start
// True when the backend's DLLs can be found (for the F1 page; does not load anything).
bool temporal_ffx_available();
bool temporal_dlss_available();
bool temporal_xess_available();   // libxess.dll is found
bool temporal_xess_ready();       // libxess.dll is loaded and the Vulkan instance and device were made with what XeSS needs

// XeSS needs its instance and device extensions and features when the Vulkan objects are made (vk_device.cpp). Both do nothing unless
// [dlss] upscaler = xess / auto (auto: when libxess.dll is found) or [upscale] mode = xess, and leave the lists as they were when the loader
// or the GPU lacks anything XeSS asks for. `have`: what the loader / GPU offers; `chain`: the device's extension feature chain (the
// availability query fills what is linked in, and the device enables it).
void xess_instance_setup(const std::vector<VkExtensionProperties>& have, std::vector<const char*>& exts);
void xess_device_setup(VkInstance instance, VkPhysicalDevice physical, const std::vector<VkExtensionProperties>& have, std::vector<const char*>& exts,
                       void** chain);
void xess_shutdown();   // the device idle, before it is destroyed: the context goes first
bool streamline_framegen_files();   // the DLSS files and the five DLSS Frame Generation adds are all in the Streamline folder
bool temporal_dlss_started();   // Streamline was initialised before the Vulkan instance (needed for DLSS; set by streamline_preinit)

// Streamline must own vkCreateInstance / vkCreateDevice / the swapchain calls: when the start-up mode is DLSS or DLAA, this loads
// sl.interposer.dll, runs slInit and returns its vkGetInstanceProcAddr for volkInitializeCustom (vk_device.cpp). Null: use the normal loader.
PFN_vkGetInstanceProcAddr streamline_preinit();
bool streamline_framegen_available();
void streamline_check_framegen(VkPhysicalDevice physical);
void streamline_shutdown();   // after swapchains/resources, before destroying the Vulkan device; keep interposer mapped

}  // namespace nxrt::extras
