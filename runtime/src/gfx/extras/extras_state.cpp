// SPDX-License-Identifier: GPL-3.0-or-later
// Settings and runtime state of the display extras (include/nxrt/extras.h).
#include "nxrt/extras.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <mutex>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "extras/hdr_math.h"

namespace nxrt::extras {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::mutex g_mu;
Settings g_settings;
bool g_settings_set = false;
std::atomic<int> g_upscale_mode{-1};          // -1: not read yet
std::atomic<float> g_upscale_sharp{0.8f};
std::atomic<int> g_dlss_preset{0};
std::atomic<int> g_scene_scale{100};
std::atomic<int> g_upscale_quality{static_cast<int>(UpscaleQuality::Quality)};
std::atomic<float> g_pic_contrast{1.0f}, g_pic_saturation{1.0f}, g_pic_black{0.0f}, g_pic_gamma{1.0f};
std::atomic<bool> g_pic_active{false};

// The state is a handful of floats read by the render thread, the guest threads (hooks) and the UI. A seqlock keeps readers lock-free
// and consistent without a mutex on the hot path.
std::atomic<uint32_t> g_seq{0};
HdrState g_state;

}  // namespace

const char* output_kind_name(OutputKind k) {
    switch (k) {
        case OutputKind::Sdr8: return "sdr8";
        case OutputKind::Hdr10: return "hdr10";
        case OutputKind::ScRgb: return "scrgb";
    }
    return "?";
}

Settings settings_from_config() {
    Settings s;
    const std::string hm = lower(config_get_str("hdr", "mode", "auto"));
    s.hdr.mode = hm == "off" || hm == "0" || hm == "false" ? HdrMode::Off : hm == "on" || hm == "1" || hm == "true" ? HdrMode::On : hm == "native" ? HdrMode::Native : HdrMode::Auto;
    const std::string hf = lower(config_get_str("hdr", "format", "auto"));
    s.hdr.format = hf == "hdr10" ? HdrFormat::Hdr10 : hf == "scrgb" ? HdrFormat::ScRgb : HdrFormat::Auto;
    s.hdr.peak_nits = std::clamp(config_get_double("hdr", "peak_nits", 0.0), 0.0, 10000.0);
    if (s.hdr.peak_nits != 0 && s.hdr.peak_nits < 200) s.hdr.peak_nits = 200;
    s.hdr.paper_white_nits = std::clamp(config_get_double("hdr", "paper_white_nits", 0.0), 0.0, 500.0);
    if (s.hdr.paper_white_nits != 0 && s.hdr.paper_white_nits < 80) s.hdr.paper_white_nits = 80;
    s.hdr.highlight_boost = std::clamp(config_get_double("hdr", "highlight_boost", 1.0), 0.0, 2.0);

    const std::string um = lower(config_get_str("upscale", "mode", "off"));
    s.upscale.mode = um == "fsr1" || um == "spatial" ? UpscaleMode::Fsr1 : um == "fsr3" ? UpscaleMode::Fsr3 : um == "dlss" ? UpscaleMode::Dlss : um == "xess" ? UpscaleMode::Xess : um == "nis" ? UpscaleMode::Nis : um == "dlaa" ? UpscaleMode::Dlaa : UpscaleMode::Off;
    const std::string uq = lower(config_get_str("upscale", "quality", "quality"));
    s.upscale.quality = uq == "native" || uq == "nativeaa" || uq == "aa" ? UpscaleQuality::Native : uq == "balanced" ? UpscaleQuality::Balanced
                      : uq == "performance" ? UpscaleQuality::Performance : uq == "ultra_performance" || uq == "ultraperformance" ? UpscaleQuality::UltraPerformance
                      : uq == "custom" ? UpscaleQuality::Custom : UpscaleQuality::Quality;
    s.upscale.sharpness = std::clamp(config_get_double("upscale", "sharpness", 0.8), 0.0, 1.0);
    s.upscale.dlss_preset = std::max(0, dlss_preset_from_word(lower(config_get_str("upscale", "dlss_preset", "default"))));
    s.upscale.scene_scale = static_cast<int>(std::clamp<int64_t>(config_get_int("upscale", "scene_scale", 100), 25, 100));
    s.upscale.fsr_dll = config_get_str("upscale", "fsr_dll", "");
    s.upscale.dlss_dll = config_get_str("upscale", "dlss_dll", "");
    s.upscale.xess_dll = config_get_str("upscale", "xess_dll", "");
    s.upscale.frame_generation = config_get_bool("upscale", "frame_generation", false);

    const std::string lm = lower(config_get_str("latency", "mode", "off"));
    s.latency.mode = lm == "auto" ? LatencyMode::Auto : lm == "on" || lm == "1" || lm == "true" ? LatencyMode::On : lm == "boost" ? LatencyMode::Boost : LatencyMode::Off;
    s.latency.frames_ahead = static_cast<int>(std::clamp<int64_t>(config_get_int("latency", "frames_ahead", 1), -1, 3));
    s.latency.log_timings = config_get_bool("latency", "log_timings", false);

    // [picture]: a preset first, then any single key set next to it wins over the preset's value
    PictureSettings pic;
    picture_preset(lower(config_get_str("picture", "preset", "neutral")), &pic);
    pic.contrast = config_get_double("picture", "contrast", pic.contrast);
    pic.saturation = config_get_double("picture", "saturation", pic.saturation);
    pic.black = config_get_double("picture", "black", pic.black);
    pic.gamma = config_get_double("picture", "gamma", pic.gamma);
    s.picture = picture_clamped(pic);
    return s;
}

Settings settings() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_settings_set) {
        g_settings = settings_from_config();
        g_settings_set = true;
        g_upscale_mode.store(static_cast<int>(g_settings.upscale.mode));
        g_upscale_sharp.store(static_cast<float>(g_settings.upscale.sharpness));
        g_dlss_preset.store(g_settings.upscale.dlss_preset);
        g_scene_scale.store(g_settings.upscale.scene_scale);
        g_upscale_quality.store(static_cast<int>(g_settings.upscale.quality));
        set_picture_live(g_settings.picture);
    }
    return g_settings;
}

void set_settings(const Settings& s) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_settings = s;
    g_settings_set = true;
    g_upscale_mode.store(static_cast<int>(s.upscale.mode));
    g_upscale_sharp.store(static_cast<float>(s.upscale.sharpness));
    g_dlss_preset.store(s.upscale.dlss_preset);
    g_scene_scale.store(s.upscale.scene_scale);
    g_upscale_quality.store(static_cast<int>(s.upscale.quality));
    set_picture_live(s.picture);
}

PictureSettings picture_clamped(PictureSettings p) {
    p.contrast = std::clamp(p.contrast, 0.5, 1.5);
    p.saturation = std::clamp(p.saturation, 0.0, 2.0);
    p.black = std::clamp(p.black, -0.5, 0.25);
    p.gamma = std::clamp(p.gamma, 0.5, 2.0);
    return p;
}

void set_picture_live(const PictureSettings& in) {
    const PictureSettings p = picture_clamped(in);
    g_pic_contrast.store(static_cast<float>(p.contrast), std::memory_order_relaxed);
    g_pic_saturation.store(static_cast<float>(p.saturation), std::memory_order_relaxed);
    g_pic_black.store(static_cast<float>(p.black), std::memory_order_relaxed);
    g_pic_gamma.store(static_cast<float>(p.gamma), std::memory_order_relaxed);
    g_pic_active.store(!p.neutral(), std::memory_order_release);
}

PictureLive picture_live() {
    upscale_mode();      // reads the settings once
    PictureLive l;
    l.active = g_pic_active.load(std::memory_order_acquire);
    if (!l.active) return l;
    l.contrast = g_pic_contrast.load(std::memory_order_relaxed);
    l.saturation = g_pic_saturation.load(std::memory_order_relaxed);
    l.black = g_pic_black.load(std::memory_order_relaxed);
    l.gamma = g_pic_gamma.load(std::memory_order_relaxed);
    return l;
}

UpscaleMode upscale_mode() {
    int m = g_upscale_mode.load(std::memory_order_relaxed);
    if (m < 0) {
        settings();
        m = g_upscale_mode.load(std::memory_order_relaxed);
    }
    return static_cast<UpscaleMode>(m);
}

float upscale_sharpness() { return g_upscale_sharp.load(std::memory_order_relaxed); }
int dlss_preset() { return g_dlss_preset.load(std::memory_order_relaxed); }
// The presets a current DLSS library still has: E and F (the older convolutional models, the lightest), J and K (transformer), L and M
// (second transformer generation). The other letters fall back to the default inside DLSS.
int dlss_preset_from_word(const std::string& w) {
    if (w.empty() || w == "default" || w == "auto" || w == "0") return 0;
    if (w.size() == 1) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(w[0])));
        if (c == 'e' || c == 'f' || (c >= 'j' && c <= 'm')) return c - 'a' + 1;
    }
    return -1;
}
const char* dlss_preset_word(int preset) {
    switch (preset) {
        case 5: return "e";
        case 6: return "f";
        case 10: return "j";
        case 11: return "k";
        case 12: return "l";
        case 13: return "m";
        default: return "default";
    }
}

int upscale_scene_scale() {
    upscale_mode();      // reads the settings once
    if (upscale_mode() == UpscaleMode::Dlaa) return 100;
    if (upscale_temporal()) {
        const int pct = quality_percent(upscale_quality());
        if (pct > 0) return pct;
    }
    return g_scene_scale.load(std::memory_order_relaxed);
}

UpscaleQuality upscale_quality() {
    upscale_mode();
    return static_cast<UpscaleQuality>(g_upscale_quality.load(std::memory_order_relaxed));
}

bool upscale_temporal() {
    const UpscaleMode m = upscale_mode();
    return m == UpscaleMode::Fsr3 || m == UpscaleMode::Dlss || m == UpscaleMode::Dlaa;
}

int quality_percent(UpscaleQuality q) {
    switch (q) {
        case UpscaleQuality::Native: return 100;
        case UpscaleQuality::Quality: return 67;            // 1 / 1.5
        case UpscaleQuality::Balanced: return 59;           // 1 / 1.7
        case UpscaleQuality::Performance: return 50;        // 1 / 2
        case UpscaleQuality::UltraPerformance: return 33;   // 1 / 3
        case UpscaleQuality::Custom: return -1;
    }
    return -1;
}

HdrState hdr_state() {
    for (;;) {
        const uint32_t a = g_seq.load(std::memory_order_acquire);
        if (a & 1u) continue;
        const HdrState copy = g_state;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_seq.load(std::memory_order_relaxed) == a) return copy;
    }
}

void hdr_set_state(const HdrState& s) {
    std::lock_guard<std::mutex> lk(g_mu);   // one writer at a time
    const uint32_t a = g_seq.load(std::memory_order_relaxed);
    g_seq.store(a + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_state = s;
    g_seq.store(a + 2, std::memory_order_release);
}

void hdr_refresh_levels() {
    HdrState hs = hdr_state();
    if (!hs.scene_hdr) return;
    const Settings es = settings();
    hdr_resolve_levels(es.hdr, hs.display_hdr, hs.display_peak_nits, hs.display_sdr_white_nits, &hs.paper_white_nits, &hs.peak_nits);
    if (es.hdr.mode == HdrMode::Native && es.hdr.paper_white_nits <= 0 && hs.game_standard_nits >= 80.0f) {
        // the game's SDR-graded range is what a Switch 2 shows at the TV's paper white; [hdr] paper_white_nits, when set, still wins
        hs.paper_white_nits = std::clamp(hs.game_standard_nits, 80.0f, 500.0f);
        hs.peak_nits = std::max(hs.peak_nits, hs.paper_white_nits);
    }
    hs.peak_ratio = hs.output == OutputKind::Sdr8 ? 1.0f : peak_ratio(hs.peak_nits, hs.paper_white_nits, static_cast<float>(es.hdr.highlight_boost));
    hdr_set_state(hs);
}

bool hdr_mode_native() { return settings().hdr.mode == HdrMode::Native; }

namespace {
std::mutex g_note_mu;
std::string g_native_logged;

const char* output_format_name(OutputKind k) {
    return k == OutputKind::Hdr10 ? "A2B10G10R10" : k == OutputKind::ScRgb ? "R16G16B16A16_SFLOAT" : "B8G8R8A8 (SDR)";
}
const char* output_space_name(OutputKind k) {
    return k == OutputKind::Hdr10 ? "HDR10_ST2084" : k == OutputKind::ScRgb ? "EXTENDED_SRGB_LINEAR" : "SRGB_NONLINEAR";
}

// After a declaration: levels, a metadata resend, and the one decision line (again only when what it says changes).
void after_note() {
    hdr_refresh_levels();
    if (!hdr_mode_native()) return;
    const HdrState hs = hdr_state();
    char line[512];
    std::snprintf(line, sizeof line,
                  "HDR native: game path on (vi layer luminance standard %.0f, extended %.0f nits; NVN window colour space %d, content max %.0f), "
                  "format %s, colour space %s, peak %.0f nits, paper white %.0f nits",
                  hs.game_standard_nits, hs.game_extended_nits, hs.game_color_space, hs.game_max_cll, output_format_name(hs.output),
                  output_space_name(hs.output), hs.peak_nits, hs.paper_white_nits);
    if (hs.game_standard_nits <= 0 || hs.game_extended_nits <= 0) return;   // both layer ranges arrive back to back: one line for the pair
    if (g_native_logged == line) return;
    g_native_logged = line;
    NXLOG_I("hdr", "%s", line);
}
}  // namespace

void hdr_note_layer_luminance(bool extended, unsigned min_nits, unsigned max_nits) {
    std::lock_guard<std::mutex> lk(g_note_mu);
    HdrState hs = hdr_state();
    float& slot = extended ? hs.game_extended_nits : hs.game_standard_nits;
    const float v = static_cast<float>(max_nits);
    (void)min_nits;
    if (slot == v) return;
    slot = v;
    hs.metadata_serial++;
    hdr_set_state(hs);
    after_note();
}

void hdr_note_window_hdr(int color_space, const float* display, const float* content) {
    std::lock_guard<std::mutex> lk(g_note_mu);
    HdrState hs = hdr_state();
    if (color_space >= 0) hs.game_color_space = color_space;
    if (display) hs.game_display_max_nits = display[8];
    if (content) {
        hs.game_max_cll = content[0];
        hs.game_max_fall = content[1];
    }
    hs.metadata_serial++;
    hdr_set_state(hs);
    NXLOG_I("hdr", "NVN window HDR from the game: colour space %d, display max %.0f nits, content max CLL %.0f / FALL %.0f nits", hs.game_color_space,
            hs.game_display_max_nits, hs.game_max_cll, hs.game_max_fall);
    after_note();
}

void hdr_resolve_levels(const HdrSettings& s, bool display_hdr, float display_peak_nits, float display_sdr_white_nits, float* paper, float* peak) {
    float pw = static_cast<float>(s.paper_white_nits);
    if (pw <= 0) pw = display_hdr && display_sdr_white_nits >= 80.0f ? std::clamp(display_sdr_white_nits, 80.0f, 500.0f) : 203.0f;
    float pk = static_cast<float>(s.peak_nits);
    if (pk <= 0) pk = display_peak_nits >= 200.0f ? std::min(display_peak_nits, 10000.0f) : 1000.0f;
    pk = std::max(pk, pw);
    if (paper) *paper = pw;
    if (peak) *peak = pk;
}

}  // namespace nxrt::extras
