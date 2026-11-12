// SPDX-License-Identifier: GPL-3.0-or-later
// Display extras (S14): HDR output, upscaling, frame generation and low-latency modes.
//
// Everything here is off by default except HDR "auto" (HDR is used when the display and Windows offer it). The settings are read once at
// start-up from the play-mode settings file (controls.ini, sections [hdr] [upscale] [latency]) with mpr.ini / environment overrides, and
// the live part can be changed from the F1 page. The pieces:
//
//   HDR       the game's scene pipeline is switched to float targets and a tone curve whose shoulder reaches the display's peak; the
//             present step encodes the result as HDR10 (ST 2084) or scRGB. See src/gfx/extras/hdr_*.
//   Upscale   a spatial pass, AMD FSR 1 or NVIDIA Image Scaling, in the present step (any internal resolution below the window size) or on
//             the HDR scene before the game's tone map. Temporal: FSR 3.1 (FidelityFX API, Vulkan DLLs loaded at run time) and DLSS / DLAA
//             (NVIDIA Streamline, loaded at run time) on the HDR scene before the tone map, with motion vectors made from depth and the
//             camera matrices and the game's own projection jitter (src/gfx/extras/temporal.h, docs/S14.md). XeSS stays unavailable.
//   Latency   NVIDIA Reflex (VK_NV_low_latency2) and AMD Anti-Lag 2 (VK_AMD_anti_lag) markers and sleeps around input sampling.
#pragma once
#include <cstdint>
#include <string>

namespace nxrt::extras {

// ---- settings --------------------------------------------------------------------------------------------------------------------
// Auto: HDR when the display offers it. On: the HDR scene pipeline even without an HDR display. Native: as On, and the levels follow what the game
// declares for an HDR TV on Switch 2 (nn::vi layer content luminance ranges, NVN2 window colour space and HDR metadata); see docs/S14.md.
enum class HdrMode : uint8_t { Off = 0, Auto, On, Native };
enum class HdrFormat : uint8_t { Auto = 0, Hdr10, ScRgb };     // swapchain flavour (Auto: HDR10 when offered, else scRGB)

struct HdrSettings {
    HdrMode mode = HdrMode::Auto;
    HdrFormat format = HdrFormat::Auto;
    double peak_nits = 0;            // 0 = the display's reported peak (1000 when it reports none), else 200..10000
    double paper_white_nits = 0;     // 0 = Windows' SDR content brightness when HDR is on there (203 otherwise), else 80..500
    double highlight_boost = 1.0;    // 0 = the game's SDR look up to its own white, 1 = highlights reach the peak (0 .. 2)
};

// Nis: NVIDIA Image Scaling v1.0.3 (spatial, any GPU). Fsr3, Dlss, Dlaa: temporal (Dlaa = DLSS at the input size).
enum class UpscaleMode : uint8_t { Off = 0, Fsr1, Fsr3, Dlss, Xess, Nis, Dlaa };
// Temporal quality preset: the input scale per side (native 1, quality 1/1.5, balanced 1/1.7, performance 1/2, ultra performance 1/3); Custom
// keeps scene_scale. The scale only takes effect where the game renders the scene below its output (scene_scale); otherwise the temporal
// upscalers run at the input size (native anti-aliasing).
enum class UpscaleQuality : uint8_t { Native = 0, Quality, Balanced, Performance, UltraPerformance, Custom };
struct UpscaleSettings {
    UpscaleMode mode = UpscaleMode::Off;
    UpscaleQuality quality = UpscaleQuality::Quality;
    double sharpness = 0.8;          // 0 .. 1 (FSR 1 RCAS or the NIS sharpening; 1 = sharpest)
    int dlss_preset = 0;             // DLSS model preset: 0 = the DLSS library's default, else the preset letter's number (E 5, F 6, J 10, K 11, L 12, M 13)
    int scene_scale = 100;           // 25 .. 100: percent of the output size the 3D scene is rendered at (the game's own dynamic-resolution path; HUD and post stay native)
    std::string fsr_dll;             // FidelityFX API DLL for FSR 3 (amd_fidelityfx_loader_vk.dll or amd_fidelityfx_vk.dll; empty: next to the exe)
    std::string dlss_dll;            // folder (or sl.interposer.dll path) of the Streamline + DLSS DLLs (empty: next to the exe)
    std::string xess_dll;            // libxess.dll the player supplies (empty: not used)
    bool frame_generation = false;   // FSR 3 frame interpolation (needs the FSR 3 mode)
};

enum class LatencyMode : uint8_t { Off = 0, Auto, On, Boost };   // Auto: whichever of Reflex / Anti-Lag 2 the GPU has; Boost: Reflex boost
struct LatencySettings {
    LatencyMode mode = LatencyMode::Off;
    int frames_ahead = 1;            // with the mode on: simulation of a frame waits until at most this many earlier frames are still unpresented (0 .. 3; -1 = no wait)
    bool log_timings = false;        // log one line of timings per second
};

// Picture controls applied in the present step (the output pass), on the display-encoded picture: black level, contrast, saturation, then gamma.
// All neutral (1, 1, 0, 1): the present step is left exactly as it is (the pass is skipped).
struct PictureSettings {
    double contrast = 1.0;     // 0.5 .. 1.5, around mid grey (0.5 of the encoded signal)
    double saturation = 1.0;   // 0 .. 2 (0 = grey)
    double black = 0.0;        // -0.5 .. 0.25: below 0 pulls the blacks down (the signal at -black becomes black), above 0 lifts them; white stays
    double gamma = 1.0;        // 0.5 .. 2: mid-tone brightness (signal^(1/gamma)); above 1 brightens
    bool neutral() const { return contrast == 1.0 && saturation == 1.0 && black == 0.0 && gamma == 1.0; }
};
// The named presets ([picture] preset): "neutral" and "vivid". False for any other name (*out unchanged).
inline bool picture_preset(const std::string& name, PictureSettings* out) {   // inline: the launcher's settings code uses it without nxrt_gfx
    if (name == "neutral" || name == "off" || name.empty()) {
        *out = PictureSettings{};
        return true;
    }
    if (name == "vivid") {   // a modest push against the game's hazy look
        *out = PictureSettings{};
        out->contrast = 1.10;
        out->saturation = 1.15;
        out->black = -0.03;
        return true;
    }
    return false;
}
PictureSettings picture_clamped(PictureSettings p);

struct Settings {
    HdrSettings hdr;
    UpscaleSettings upscale;
    LatencySettings latency;
    PictureSettings picture;
};

// Reads [hdr] [upscale] [latency] from the nxrt configuration (mpr.ini, environment NXRT_HDR_MODE ...). Never fails.
Settings settings_from_config();
// The settings in force (set once by play mode / the boot code from the file; the live fields change from the F1 page).
Settings settings();
void set_settings(const Settings& s);
// Lock-free reads of the live upscale fields (the render thread asks every frame).
UpscaleMode upscale_mode();
float upscale_sharpness();
int dlss_preset();                 // the live [upscale] dlss_preset (0 = default)
int dlss_preset_from_word(const std::string& w);   // "default", "e" .. "m"; -1 when unknown
const char* dlss_preset_word(int preset);
int upscale_scene_scale();          // percent, 100 = off (a temporal mode with a quality preset other than Custom: the preset's percent)
UpscaleQuality upscale_quality();
bool upscale_temporal();            // the mode is FSR 3, DLSS or DLAA
int quality_percent(UpscaleQuality q);   // the preset's input scale per side in percent (Custom: -1)
// The game's own projection jitter as its TAA code last stored it (NDC units, the previous frame's; pkp_taa.cpp), for the log only.
void temporal_note_game_jitter(float x_ndc, float y_ndc);
// The picture controls in force for the present step (lock-free; the F1 page's sliders change them live before Apply and save).
struct PictureLive {
    float contrast = 1, saturation = 1, black = 0, gamma = 1;
    bool active = false;             // false: all neutral, the present step skips the adjustment
};
PictureLive picture_live();
void set_picture_live(const PictureSettings& p);   // only the live copy; set_settings() also sets it

// ---- HDR runtime state ---------------------------------------------------------------------------------------------------------
enum class OutputKind : uint8_t { Sdr8 = 0, Hdr10, ScRgb };
const char* output_kind_name(OutputKind k);

struct HdrState {
    bool scene_hdr = false;          // the game runs the HDR scene pipeline: float window / post targets, patched tone-map shader and curve
    OutputKind output = OutputKind::Sdr8;   // what the swapchain carries
    float paper_white_nits = 203;
    float peak_nits = 1000;
    float peak_ratio = 1;            // asymptote of the extended shoulder, in paper-white units (1 = the game's own SDR curve)
    bool display_hdr = false;        // Windows reports HDR on for the monitor (informational)
    float display_peak_nits = 0;
    float display_sdr_white_nits = 80;
    // What the game declares about its own output (0 / -1: not declared). Used by HdrMode::Native.
    float game_standard_nits = 0;    // nn::vi::SetLayerNextHdrStandardContentLuminanceRangeMetadata: the SDR-graded (paper white) range's maximum
    float game_extended_nits = 0;    // nn::vi::SetLayerNextHdrExtendedContentLuminanceRangeMetadata: the content's highlight maximum
    int game_color_space = -1;       // nvnWindowBuilderSetColorSpace value (NVN2 enum, raw)
    float game_max_cll = 0, game_max_fall = 0;   // nvnWindow(Builder)SetHdrContentMetadata
    float game_display_max_nits = 0; // nvnWindow(Builder)SetHdrDisplayMetadata maximum luminance
    uint32_t metadata_serial = 0;    // bumped whenever the swapchain's HDR metadata should be sent again
};
// Readable from any thread (a copy of a snapshot behind a lock-free sequence).
HdrState hdr_state();
void hdr_set_state(const HdrState& s);
inline bool hdr_scene_active() { return hdr_state().scene_hdr; }

// Recomputes paper white, peak and the peak ratio from settings() and the display numbers kept in the state (after a live settings change).
// The swapchain's output kind and scene_hdr never change here (they are decided when the renderer is created).
void hdr_refresh_levels();

// The game's declarations (HdrMode::Native). Thread-safe; each call refreshes the levels and the swapchain metadata, and logs the native decision once.
void hdr_note_layer_luminance(bool extended, unsigned min_nits, unsigned max_nits);
// color_space: the NVN2 value (-1 = not set); display: 10 floats (red, green, blue, white x/y, max, min luminance) or null; content: max CLL, max FALL or null.
void hdr_note_window_hdr(int color_space, const float* display, const float* content);
bool hdr_mode_native();

// Resolves the numbers the settings leave at 0 ("auto") against what the display reports.
void hdr_resolve_levels(const HdrSettings& s, bool display_hdr, float display_peak_nits, float display_sdr_white_nits, float* paper, float* peak);

}  // namespace nxrt::extras
