// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: factory settings and the settings file (reader and writer). The file format is described in play_types.h.
#include "play_types.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/haptic.h"

namespace nxrt::play {
namespace {

const std::string::size_type kNpos = std::string::npos;

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "%.9g" for doubles, as the file format asks.
std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

// As num(), but a value that would not read back to the same double gets all 17 digits (16/9 needs them).
std::string num_exact(double v) {
    std::string s = num(v);
    if (std::strtod(s.c_str(), nullptr) != v) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.17g", v);
        return buf;
    }
    return s;
}

// Whole numbers and reals: the whole string must be a number, and reals must be finite.
bool to_int(const std::string& v, long long* out) {
    if (v.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long long x = std::strtoll(v.c_str(), &end, 10);
    if (errno != 0 || end != v.c_str() + v.size()) return false;
    *out = x;
    return true;
}

bool to_double(const std::string& v, double* out) {
    if (v.empty()) return false;
    char* end = nullptr;
    const double x = std::strtod(v.c_str(), &end);
    if (end != v.c_str() + v.size() || !std::isfinite(x)) return false;
    *out = x;
    return true;
}

bool to_bool(const std::string& v, bool* out) {
    const std::string l = lower(v);
    if (l == "1" || l == "true" || l == "yes" || l == "on") {
        *out = true;
        return true;
    }
    if (l == "0" || l == "false" || l == "no" || l == "off") {
        *out = false;
        return true;
    }
    return false;
}

// One word of a fixed list (case-insensitive): its index in `words`. A word that is none of them sets *msg listing the allowed ones.
bool pick_word(const std::string& k, const std::string& v, const std::vector<const char*>& words, int* idx, std::string* msg) {
    const std::string l = lower(v);
    for (size_t i = 0; i < words.size(); ++i) {
        if (l == words[i]) {
            *idx = static_cast<int>(i);
            return true;
        }
    }
    std::string allowed;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i > 0) allowed += i + 1 == words.size() ? " or " : ", ";
        allowed += words[i];
    }
    *msg = k + " must be " + allowed + ", got '" + v + "'";
    return false;
}

// The words the file uses for the display extras enums, indexed by the enum value (the first word of each list is the default's).
const std::vector<const char*> kHdrModeWords = {"off", "auto", "on", "native"};
const std::vector<const char*> kHdrFormatWords = {"auto", "hdr10", "scrgb"};
const std::vector<const char*> kUpscaleWords = {"off", "fsr1", "fsr3", "dlss", "xess", "nis", "dlaa"};
const std::vector<const char*> kUpscaleQualityWords = {"native", "quality", "balanced", "performance", "ultra_performance", "custom"};
const std::vector<const char*> kLatencyWords = {"off", "auto", "on", "boost"};
// The upscale words accepted when reading (spatial is an alias of fsr1) and the enum value each one means.
const std::vector<const char*> kUpscaleReadWords = {"off", "fsr1", "spatial", "fsr3", "dlss", "xess", "nis", "dlaa"};
const extras::UpscaleMode kUpscaleFromWord[] = {extras::UpscaleMode::Off, extras::UpscaleMode::Fsr1, extras::UpscaleMode::Fsr1,
                                                extras::UpscaleMode::Fsr3, extras::UpscaleMode::Dlss, extras::UpscaleMode::Xess, extras::UpscaleMode::Nis,
                                                extras::UpscaleMode::Dlaa};

// Splits on '\n'. Line i of the result is line i+1 of the file (for error messages).
std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t nl = text.find('\n', pos);
        if (nl == kNpos) {
            lines.push_back(text.substr(pos));
            break;
        }
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return lines;
}

// Cuts a trailing comment: a '#' or ';' that follows a space or tab.
std::string strip_inline_comment(const std::string& line) {
    for (size_t i = 1; i < line.size(); ++i) {
        if ((line[i] == '#' || line[i] == ';') && (line[i - 1] == ' ' || line[i - 1] == '\t')) return trim(line.substr(0, i));
    }
    return line;
}

// The settings with the built-in tables only: the outputs from ActInfo, the struct defaults, and no bindings.
Settings builtin_settings() {
    Settings s;
    for (int i = 0; i < kActCount; ++i) {
        Output o;
        parse_output(act_info(static_cast<Act>(i)).default_out, &o, nullptr);   // our own table, always valid
        s.out[i] = o;
    }
    return s;
}

enum class Sec { None, Unknown, Meta, Mouse, Pad, PadMap, Timing, Display, Audio, Hdr, Upscale, Latency, Picture, Prompts, Fps, Switch, Aim, Camera, Tweaks, Bindings };

// The factory-defaults revision this build writes ([meta] defaults_rev) and the factory bindings an older controls.ini does not have: the
// game's (play_types.h GameDefaults).
int defaults_rev() {
    const GameDefaults* d = game_defaults();
    return d ? d->defaults_rev : 0;
}

const char* sec_label(Sec s) {
    switch (s) {
        case Sec::Meta: return "[meta]";
        case Sec::Mouse: return "[mouse]";
        case Sec::Pad: return "[pad]";
        case Sec::PadMap: return "[padmap]";
        case Sec::Timing: return "[timing]";
        case Sec::Display: return "[display]";
        case Sec::Audio: return "[audio]";
        case Sec::Hdr: return "[hdr]";
        case Sec::Upscale: return "[upscale]";
        case Sec::Latency: return "[latency]";
        case Sec::Picture: return "[picture]";
        case Sec::Prompts: return "[prompts]";
        case Sec::Fps: return "[fps]";
        case Sec::Switch: return "[switch]";
        case Sec::Aim: return "[aim]";
        case Sec::Camera: return "[camera]";
        case Sec::Tweaks: return "[tweaks]";
        default: return "";
    }
}

// Applies one `key = value` of a fixed section. On a bad value returns false with *msg set (no line prefix).
// *known is false for a key the section does not have.
bool apply_fixed(Sec sec, const std::string& k, const std::string& v, Settings* s, bool* known, std::string* msg) {
    *known = true;
    auto set_num = [&](double* dst) {
        double x = 0;
        if (!to_double(v, &x)) {
            *msg = k + " must be a number, got '" + v + "'";
            return false;
        }
        *dst = x;
        return true;
    };
    auto set_real = [&](double lo, double hi, double* dst) {
        double x = 0;
        if (!to_double(v, &x)) {
            *msg = k + " must be a number, got '" + v + "'";
            return false;
        }
        if (x < lo || x > hi) {
            *msg = k + " must be " + num(lo) + ".." + num(hi) + ", got " + v;
            return false;
        }
        *dst = x;
        return true;
    };
    // A real that is either 0 (the "auto" value) or within lo..hi.
    auto set_zero_or_real = [&](double lo, double hi, double* dst) {
        double x = 0;
        if (!to_double(v, &x)) {
            *msg = k + " must be a number, got '" + v + "'";
            return false;
        }
        if (x != 0 && (x < lo || x > hi)) {
            *msg = k + " must be 0 (auto) or " + num(lo) + ".." + num(hi) + ", got " + v;
            return false;
        }
        *dst = x;
        return true;
    };
    auto set_int = [&](long long lo, long long hi, int* dst) {
        long long x = 0;
        if (!to_int(v, &x)) {
            *msg = k + " must be a whole number, got '" + v + "'";
            return false;
        }
        if (x < lo || x > hi) {
            *msg = k + " must be " + std::to_string(lo) + ".." + std::to_string(hi) + ", got " + v;
            return false;
        }
        *dst = static_cast<int>(x);
        return true;
    };
    auto set_bool = [&](bool* dst) {
        if (!to_bool(v, dst)) {
            *msg = k + " must be 1/0 (or true/false, yes/no, on/off), got '" + v + "'";
            return false;
        }
        return true;
    };
    // A fixed render size: 0 (follow the window) or 320..16384.
    auto set_size = [&](int* dst) {
        long long x = 0;
        if (!to_int(v, &x)) {
            *msg = k + " must be a whole number, got '" + v + "'";
            return false;
        }
        if (x != 0 && (x < 320 || x > 16384)) {
            *msg = k + " must be 0 or 320..16384, got " + v;
            return false;
        }
        *dst = static_cast<int>(x);
        return true;
    };
    // The forced aspect ratio: auto (0), a ratio W:H (16:9, 21:9, 32:9, 16:10) or a number 1.0..5.0.
    auto set_aspect = [&](double* dst) {
        double x = 0;
        const size_t colon = v.find(':');
        double num = 0, den = 0;
        if (lower(v) == "auto") {
            x = 0;
        } else if (colon != std::string::npos && to_double(v.substr(0, colon), &num) && to_double(v.substr(colon + 1), &den) && num > 0 && den > 0) {
            x = num / den;
        } else if (!to_double(v, &x)) {
            *msg = k + " must be auto, W:H or a number, got '" + v + "'";
            return false;
        }
        if (x != 0 && (x < 1.0 || x > 5.0)) {
            *msg = k + " must be 0 (auto) or 1.0..5.0, got " + v;
            return false;
        }
        *dst = x;
        return true;
    };

    switch (sec) {
        case Sec::Meta:
            if (k == "defaults_rev") return set_int(0, 1000, &s->defaults_rev);
            break;
        case Sec::Mouse:
            if (k == "yaw") return set_num(&s->mouse.yaw_rad);
            if (k == "pitch") return set_num(&s->mouse.pitch_rad);
            // the ball camera's speed: 0.1 to 20 on the Source scale (the F1 slider's range), in radians per count for the older keys
            if (k == key_alt_yaw()) return set_real(rad_per_count(0.1), rad_per_count(20.0), &s->mouse.alt_yaw_rad);
            if (k == key_alt_pitch()) return set_real(rad_per_count(0.1), rad_per_count(20.0), &s->mouse.alt_pitch_rad);
            if (k == key_alt_camera()) return set_bool(&s->mouse.alt_camera);
            if (k == key_alt_sens()) {   // Source scale, both directions
                double x = 0;
                if (!set_real(0.1, 20.0, &x)) return false;
                s->mouse.alt_yaw_rad = s->mouse.alt_pitch_rad = rad_per_count(x);
                return true;
            }
            if (k == "yaw_source" || k == "pitch_source") {   // Source scale, converted to radians per count
                double x = 0;
                if (!set_num(&x)) return false;
                (k == "yaw_source" ? s->mouse.yaw_rad : s->mouse.pitch_rad) = rad_per_count(x);
                return true;
            }
            if (k == "invert_y") return set_bool(&s->mouse.invert_y);
            if (k == "invert_x") return set_bool(&s->mouse.invert_x);
            if (k == "menu_cursor") return set_bool(&s->mouse.menu_cursor);
            if (k == "menu_touch") return true;   // legacy key, dead since it never did anything: accepted and ignored
            if (k == key_wheel_cycles()) return set_bool(&s->mouse.wheel_cycles);
            if (k == "jc2_speed") return set_real(0.05, 20.0, &s->mouse.jc2_speed);
            if (k == "jc2_wake") return set_real(0.0, 500.0, &s->mouse.jc2_wake);
            if (k == "jc2_capture") return set_bool(&s->mouse.jc2_capture);
            break;
        case Sec::Aim:
            if (k == "lock_mode") {
                LockMode m = LockMode::Classic;
                if (!parse_lock_mode(v, &m)) {
                    *msg = "lock_mode must be classic or free, got '" + v + "'";
                    return false;
                }
                s->aim.lock_mode = m;
                return true;
            }
            if (k == "free_lock_margin") return set_real(0.0, 0.4, &s->aim.free_lock_margin);
            break;
        case Sec::Camera:
            if (k == "gun_drives_view") return set_bool(&s->camera.gun_drives_view);
            break;
        case Sec::Tweaks: {
            bool mine = false;
            if (!fastboot_apply(k, v, &s->fastboot, &mine, msg)) return false;
            if (mine) return true;
            return apply_tweaks_key(k, v, &s->tweaks, known, msg);
        }
        case Sec::Pad:
            if (k == "label_abxy") return set_bool(&s->pad.label_abxy);
            if (k == "deadzone") return set_real(0.0, 0.5, &s->pad.stick_deadzone);
            if (k == "right_deadzone") return set_real(0.0, 0.5, &s->pad.right_deadzone);
            if (k == "stick_max") return set_real(0.5, 1.0, &s->pad.stick_max);
            if (k == "invert_y") return set_bool(&s->pad.invert_y);
            if (k == "look_sens") return set_real(0.25, 3.0, &s->pad.look_sens);
            if (k == "trigger_threshold") return set_real(0.05, 1.0, &s->pad.trigger_threshold);
            if (k == "gyro") return set_bool(&s->pad.gyro);
            if (k == "gyro_scale") return set_num(&s->pad.gyro_scale);
            if (k == "rumble") return set_bool(&s->pad.rumble);
            if (k == "rumble_scale") return set_num(&s->pad.rumble_scale);
            if (k == "haptic") {
                gfx::haptic::Mode m = gfx::haptic::Mode::Auto;
                if (!gfx::haptic::mode_from_name(v.c_str(), &m)) {
                    *msg = "haptic must be auto, off, rumble or dualsense, got '" + v + "'";
                    return false;
                }
                s->pad.haptic = static_cast<int>(m);
                return true;
            }
            if (k == "haptic_strength") return set_int(-1, 100, &s->pad.haptic_strength);
            if (k == key_alt_orbit()) return set_real(0.0, 6.0, &s->pad.alt_orbit);
            break;
        case Sec::Timing:
            if (k == "tap_ms") return set_int(1, 5000, &s->timing.tap_ms);
            if (k == "hold_ms") return set_int(1, 5000, &s->timing.hold_ms);
            if (k == "double_ms") return set_int(1, 5000, &s->timing.double_ms);
            if (k == "pulse_ms") return set_int(1, 5000, &s->timing.pulse_ms);
            break;
        case Sec::Prompts:
            if (k == "style") {
                if (!parse_prompt_style(v, &s->prompts.style)) {
                    *msg = "style must be switch, xbox, playstation, keyboard or auto, got '" + v + "'";
                    return false;
                }
                return true;
            }
            break;
        case Sec::Fps:
            if (k == "show") return set_bool(&s->fps.show);
            if (k == "key") {
                InputId id;
                if (!parse_input(v, &id) || id.kind != InKind::Key) {
                    *msg = "key must be a keyboard key name (F2, Tab, Left Ctrl ...), got '" + v + "'";
                    return false;
                }
                s->fps.key = id;
                return true;
            }
            if (k == "corner") {
                if (!parse_corner(v, &s->fps.corner)) {
                    *msg = "corner must be top_left, top_right, bottom_left or bottom_right, got '" + v + "'";
                    return false;
                }
                return true;
            }
            if (k == "detail") return set_int(0, 2, &s->fps.detail);
            if (k == "in_screenshots") return set_bool(&s->fps.in_screenshots);
            break;
        case Sec::PadMap: {
            PadSrc src;
            if (!parse_pad_src(k, &src)) break;   // unknown pad button name: a warning
            uint64_t& dst = s->pad.remap[static_cast<int>(src)];
            if (lower(v) == "default") {
                dst = kPadDefault;
                return true;
            }
            Output o;
            std::string perr;
            if (!parse_output(v, &o, &perr)) {
                *msg = perr;
                return false;
            }
            if (o.lx != 0 || o.ly != 0 || o.rx != 0 || o.ry != 0) {
                *msg = k + " can press Switch buttons only (no stick directions), got '" + v + "'";
                return false;
            }
            dst = o.buttons;
            return true;
        }
        case Sec::Display:
            if (k == "window_mode") {
                const std::string l = lower(v);
                if (l != "windowed" && l != "borderless" && l != "exclusive") {
                    *msg = "window_mode must be windowed, borderless or exclusive, got '" + v + "'";
                    return false;
                }
                s->display.window_mode = l;
                return true;
            }
            if (k == "vsync") return set_bool(&s->display.vsync);
            if (k == "fps_limit" || k == "fps_cap") {     // fps_cap: the older name
                if (lower(v) == "auto") { s->display.fps_cap = -1; return true; }
                return set_int(0, 1000, &s->display.fps_cap);
            }
            if (k == "width") return set_int(320, 16384, &s->display.width);
            if (k == "height") return set_int(320, 16384, &s->display.height);
            if (k == "keep_aspect") return set_bool(&s->display.keep_aspect);
            if (k == "render_scale") {
                if (lower(v) == "auto") { s->display.render_scale = 0; return true; }
                return set_int(25, 400, &s->display.render_scale);
            }
            if (k == "render_width") return set_size(&s->display.render_width);
            if (k == "render_height") return set_size(&s->display.render_height);
            if (k == "aspect") return set_aspect(&s->display.aspect);
            if (k == "fov_offset") return set_real(-30.0, 40.0, &s->display.fov_offset);
            if (k == "dynamic_resolution") return set_bool(&s->display.dynamic_resolution);
            if (k == "anisotropy") {
                long long x = 0;
                if (!to_int(v, &x)) {
                    *msg = k + " must be a whole number, got '" + v + "'";
                    return false;
                }
                if (x != 0 && x != 2 && x != 4 && x != 8 && x != 16) {
                    *msg = "anisotropy must be 0, 2, 4, 8 or 16, got " + v;
                    return false;
                }
                s->display.anisotropy = static_cast<int>(x);
                return true;
            }
            if (k == "lod_bias") return set_real(-2.0, 2.0, &s->display.lod_bias);
            if (k == "bloom") return set_bool(&s->display.bloom);
            if (k == "chromatic_aberration") return set_bool(&s->display.chromatic_aberration);
            if (k == "view_distance") return set_real(1.0, 4.0, &s->display.view_distance);
            if (k == "quality_preset") {
                const std::string q = lower(v);
                if (q != "switch2" && q != "high" && q != "ultra" && q != "custom") {
                    *msg = "quality_preset must be switch2, high, ultra or custom, got '" + v + "'";
                    return false;
                }
                s->display.quality_preset = q;
                return true;
            }
            if (k == "fast_gpu_memory") return set_bool(&s->display.fast_gpu_memory);
            if (k == "fewer_gpu_waits") return set_bool(&s->display.fewer_gpu_waits);
            if (k == "shader_prewarm") return set_bool(&s->display.shader_prewarm);
            if (k == "astc_quality") {
                const std::string q = lower(v);
                if (q != "fast" && q != "high" && q != "uncompressed") {
                    *msg = "astc_quality must be fast, high or uncompressed, got " + v;
                    return false;
                }
                s->display.astc_quality = q;
                return true;
            }
            if (k == "monitor") {
                if (lower(v) == "auto") { s->display.monitor = 0; return true; }
                return set_int(0, 32, &s->display.monitor);
            }
            break;
        case Sec::Audio:
            if (k == "volume") return set_real(0.0, 1.0, &s->audio.volume);
            if (k == "mute_unfocused") return set_bool(&s->audio.mute_unfocused);
            if (k == "device") {
                s->audio.device = v.empty() ? "default" : v;
                return true;
            }
            if (k == "channels") {
                const std::string w = lower(v);
                if (w == "stereo" || w == "2") { s->audio.channels = 2; return true; }
                if (w == "5.1" || w == "surround" || w == "6") { s->audio.channels = 6; return true; }
                *msg = k + " must be 2 (stereo) or 6 (5.1 surround), got '" + v + "'";
                return false;
            }
            break;
        case Sec::Hdr: {
            extras::HdrSettings& h = s->extras.hdr;
            if (k == "mode") {
                int i = 0;
                if (!pick_word(k, v, kHdrModeWords, &i, msg)) return false;
                h.mode = static_cast<extras::HdrMode>(i);
                return true;
            }
            if (k == "format") {
                int i = 0;
                if (!pick_word(k, v, kHdrFormatWords, &i, msg)) return false;
                h.format = static_cast<extras::HdrFormat>(i);
                return true;
            }
            if (k == "peak_nits") return set_zero_or_real(200.0, 10000.0, &h.peak_nits);
            if (k == "paper_white_nits") return set_zero_or_real(80.0, 500.0, &h.paper_white_nits);
            if (k == "highlight_boost") return set_real(0.0, 2.0, &h.highlight_boost);
            break;
        }
        case Sec::Upscale: {
            extras::UpscaleSettings& u = s->extras.upscale;
            if (k == "mode") {
                int i = 0;
                if (!pick_word(k, v, kUpscaleReadWords, &i, msg)) return false;
                u.mode = kUpscaleFromWord[i];
                return true;
            }
            if (k == "quality") {   // the temporal modes' render scale preset
                int i = 0;
                if (!pick_word(k, v, kUpscaleQualityWords, &i, msg)) return false;
                u.quality = static_cast<extras::UpscaleQuality>(i);
                return true;
            }
            if (k == "sharpness") return set_real(0.0, 1.0, &u.sharpness);
            if (k == "dlss_preset") {   // default, e, f, j, k, l, m
                std::string w = v;
                for (char& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const int pr = extras::dlss_preset_from_word(w);
                if (pr < 0) {
                    if (msg) *msg = "dlss_preset: default, e, f, j, k, l or m";
                    return false;
                }
                u.dlss_preset = pr;
                return true;
            }
            if (k == "fsr_dll") {   // a path: kept as written
                u.fsr_dll = v;
                return true;
            }
            if (k == "dlss_dll") {
                u.dlss_dll = v;
                return true;
            }
            if (k == "xess_dll") {
                u.xess_dll = v;
                return true;
            }
            if (k == "scene_scale") return set_int(25, 100, &u.scene_scale);
            if (k == "frame_generation") return set_bool(&u.frame_generation);
            break;
        }
        case Sec::Latency: {
            extras::LatencySettings& l = s->extras.latency;
            if (k == "mode") {
                int i = 0;
                if (!pick_word(k, v, kLatencyWords, &i, msg)) return false;
                l.mode = static_cast<extras::LatencyMode>(i);
                return true;
            }
            if (k == "frames_ahead") return set_int(-1, 3, &l.frames_ahead);
            if (k == "log_timings") return set_bool(&l.log_timings);
            break;
        }
        case Sec::Picture: {
            extras::PictureSettings& c = s->extras.picture;
            if (k == "preset") {   // read only (the file keeps the four values): fills them in, the keys after it win
                if (!extras::picture_preset(lower(v), &c)) {
                    *msg = "preset must be neutral or vivid";
                    return false;
                }
                return true;
            }
            if (k == "contrast") return set_real(0.5, 1.5, &c.contrast);
            if (k == "saturation") return set_real(0.0, 2.0, &c.saturation);
            if (k == "black") return set_real(-0.5, 0.25, &c.black);
            if (k == "gamma") return set_real(0.5, 2.0, &c.gamma);
            break;
        }
        case Sec::Switch: {
            Act a = Act::None;
            if (!parse_act(k, &a) || a == Act::None) break;   // unknown action name: a warning
            Output o;
            std::string perr;
            if (!parse_output(v, &o, &perr)) {
                *msg = perr;
                return false;
            }
            s->out[static_cast<int>(a)] = o;
            return true;
        }
        default:
            break;
    }
    *known = false;
    return true;
}

// Adds the factory bindings newer than `file_rev` to a file that has bindings sections. A binding is added only when no binding of the
// same context already uses that input. Sets *added to a description of what was added (for the log line).
// A fixed section by its lower-case name ([bindings.*] is not one); Sec::Unknown when there is none.
Sec fixed_sec(const std::string& key) {
    if (key == "meta") return Sec::Meta;
    if (key == "mouse") return Sec::Mouse;
    if (key == "pad") return Sec::Pad;
    if (key == "padmap") return Sec::PadMap;
    if (key == "timing") return Sec::Timing;
    if (key == "display") return Sec::Display;
    if (key == "audio") return Sec::Audio;
    if (key == "hdr") return Sec::Hdr;
    if (key == "upscale") return Sec::Upscale;
    if (key == "latency") return Sec::Latency;
    if (key == "picture") return Sec::Picture;
    if (key == "prompts") return Sec::Prompts;
    if (key == "fps") return Sec::Fps;
    if (key == "aim") return Sec::Aim;
    if (key == "camera") return Sec::Camera;
    if (key == "tweaks") return Sec::Tweaks;
    if (key == "switch") return Sec::Switch;
    return Sec::Unknown;
}

// Factory values a later revision changed (GameDefaults::changes): where the file still has the old factory value, it takes the new one.
// "Still has" = setting the old value changes nothing in the written file, so any value the player picked stays.
void apply_default_changes(Settings* s, int file_rev, std::string* added) {
    const GameDefaults* gd = game_defaults();
    for (unsigned i = 0; gd && i < gd->change_count; ++i) {
        const GameDefaultChange& c = gd->changes[i];
        const Sec sec = fixed_sec(lower(c.section));
        if (c.rev <= file_rev || sec == Sec::Unknown) continue;
        Settings old = *s, now = *s;
        bool known = false;
        std::string msg;
        if (!apply_fixed(sec, lower(c.key), c.from, &old, &known, &msg) || !known) continue;
        if (format_settings(old) != format_settings(*s)) continue;
        if (!apply_fixed(sec, lower(c.key), c.to, &now, &known, &msg) || !known) continue;
        *s = now;
        if (!added->empty()) *added += ", ";
        *added += std::string(c.key) + " = " + c.to + " in [" + c.section + "]";
    }
}

void apply_default_additions(Settings* s, int file_rev, std::string* added) {
    const GameDefaults* gd = game_defaults();
    for (unsigned i = 0; gd && i < gd->addition_count; ++i) {
        const GameDefaultAddition& d = gd->additions[i];
        if (d.rev <= file_rev) continue;
        InputId id;
        if (!parse_input(d.input, &id)) continue;
        bool taken = false;
        for (const Binding& b : s->bindings) {
            if (b.ctx == d.ctx && b.in == id) {
                taken = true;
                break;
            }
        }
        if (taken) continue;
        Binding b;
        b.in = id;
        b.ctx = d.ctx;
        b.act = d.act;
        b.trig = Trig::Press;
        b.ms = 0;
        s->bindings.push_back(b);
        if (!added->empty()) *added += ", ";
        *added += input_name(id) + " = " + act_info(d.act).name + " in [bindings." + ctx_name(d.ctx) + "]";
    }
}

// The core reader. Applies `text` on top of `*s`. Only writes what it parsed into `*s`; the caller decides what to keep.
// If the text has no [bindings.*] section and with_default_bindings is set, the factory bindings are loaded first.
bool parse_onto(const std::string& text, Settings* s, std::string* err, std::vector<std::string>* warnings, bool with_default_bindings) {
    const std::vector<std::string> lines = split_lines(text);

    // Any [bindings.*] section means the file is the whole truth about bindings, so the defaults never apply.
    bool has_bindings = false;
    for (const std::string& raw : lines) {
        const std::string l = trim(raw);
        if (!l.empty() && l[0] == '[' && lower(l).compare(0, 10, "[bindings.") == 0) has_bindings = true;
    }
    if (has_bindings) {
        s->bindings.clear();
    } else if (with_default_bindings) {
        Settings factory = builtin_settings();
        std::string ferr;
        // The factory text has bindings sections, so this inner call never comes back here.
        if (!parse_onto(default_settings_text(), &factory, &ferr, nullptr, false)) {
            if (err) *err = "internal: factory bindings do not parse: " + ferr;
            return false;
        }
        s->bindings = factory.bindings;
    }

    auto bail = [&](size_t i, const std::string& m) {
        if (err) *err = "line " + std::to_string(i + 1) + ": " + m;
    };
    auto warn = [&](size_t i, const std::string& m) {
        if (warnings) warnings->push_back("line " + std::to_string(i + 1) + ": " + m);
    };

    Sec sec = Sec::None;
    Ctx ctx = Ctx::All;
    // Which of the two dead-zone keys the [pad] section has, for the rule after the loop (right_deadzone takes deadzone's value when only deadzone is there).
    bool pad_has_deadzone = false, pad_has_right_deadzone = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string line = trim(lines[i]);
        if (line.empty()) continue;

        if (line[0] == '#' || line[0] == ';') {
            // Full-line comment. Exception: in a bindings section the semicolon key (";") is a real binding; a line whose key
            // before the last '=' parses as an input is kept.
            bool binding_line = false;
            if (sec == Sec::Bindings) {
                const size_t eq = line.rfind('=');
                InputId probe;
                binding_line = eq != kNpos && parse_input(trim(line.substr(0, eq)), &probe);
            }
            if (!binding_line) continue;
        } else {
            line = strip_inline_comment(line);
        }

        if (line[0] == '[') {
            if (line.back() != ']') {
                bail(i, "section header without ']'");
                return false;
            }
            const std::string name = trim(line.substr(1, line.size() - 2));
            const std::string key = lower(name);
            if (key.compare(0, 9, "bindings.") == 0 && parse_ctx(name.substr(9), &ctx)) sec = Sec::Bindings;
            else if ((sec = fixed_sec(key)) == Sec::Unknown) warn(i, "unknown section '[" + name + "]'");
            continue;
        }

        if (sec == Sec::Unknown) continue;   // the keys of an unknown section are skipped

        // Bindings keep the key before the LAST '=' (the key may itself be "="); their value never contains '='.
        const size_t eq = sec == Sec::Bindings ? line.rfind('=') : line.find('=');
        if (eq == kNpos) {
            bail(i, "expected 'key = value', got '" + line + "'");
            return false;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (sec == Sec::None) {
            warn(i, "'" + key + "' is outside any section");
            continue;
        }

        if (sec == Sec::Bindings) {
            InputId id;
            if (!parse_input(key, &id)) {
                bail(i, "unknown input '" + key + "'");
                return false;
            }
            // value: action [trigger [ms]]
            std::vector<std::string> tok;
            std::istringstream words(val);
            for (std::string w; words >> w;) tok.push_back(w);
            if (tok.empty() || tok.size() > 3) {
                bail(i, "expected '<action> [trigger [ms]]', got '" + val + "'");
                return false;
            }
            Binding b;
            b.in = id;
            b.ctx = ctx;
            if (!parse_act(tok[0], &b.act)) {
                bail(i, "unknown action '" + tok[0] + "'");
                return false;
            }
            if (tok.size() >= 2 && !parse_trig(tok[1], &b.trig)) {
                bail(i, "unknown trigger '" + tok[1] + "'");
                return false;
            }
            if (tok.size() == 3) {
                long long ms = 0;
                if (!to_int(tok[2], &ms) || ms < 1 || ms > 5000) {
                    bail(i, "ms must be 1..5000, got '" + tok[2] + "'");
                    return false;
                }
                b.ms = static_cast<int>(ms);
            }
            s->bindings.push_back(b);
            continue;
        }

        bool known = false;
        std::string msg;
        if (!apply_fixed(sec, lower(key), val, s, &known, &msg)) {
            bail(i, msg);
            return false;
        }
        if (!known) warn(i, "unknown key '" + key + "' in " + sec_label(sec));
        if (sec == Sec::Pad) {
            if (lower(key) == "deadzone") pad_has_deadzone = true;
            if (lower(key) == "right_deadzone") pad_has_right_deadzone = true;
        }
    }

    // A file written before right_deadzone existed has one dead zone (deadzone), so the right stick takes its value.
    if (pad_has_deadzone && !pad_has_right_deadzone) s->pad.right_deadzone = s->pad.stick_deadzone;

    if (has_bindings) {
        // A file with bindings written by an older revision gets the newer factory bindings it lacks (see GameDefaults).
        const int file_rev = s->defaults_rev;   // 0 when the file has no [meta]
        if (file_rev < defaults_rev()) {
            std::string added;
            apply_default_additions(s, file_rev, &added);
            apply_default_changes(s, file_rev, &added);
            s->defaults_rev = defaults_rev();
            s->migrated = true;
            s->migration_added = added;
        }
    } else if (with_default_bindings) {
        s->defaults_rev = defaults_rev();   // the factory bindings were applied, so the result is at the current revision
    }
    return true;
}

}  // namespace

// [aim] lock_mode names. Case-insensitive, no trimming (the file parser has already trimmed the value).
const char* lock_mode_name(LockMode m) { return m == LockMode::Free ? "free" : "classic"; }

bool parse_lock_mode(const std::string& s, LockMode* out) {
    const std::string key = lower(s);
    if (key == "classic") {
        if (out) *out = LockMode::Classic;
        return true;
    }
    if (key == "free") {
        if (out) *out = LockMode::Free;
        return true;
    }
    return false;
}

const GameDefaults* game_defaults() { return game::info().play_defaults; }

const char* default_settings_text() {
    const GameDefaults* d = game_defaults();
    return d && d->settings_text ? d->settings_text : "";
}

Settings default_settings() {
    Settings s;
    std::string err;
    // The factory text has a [bindings.*] section, so parse_settings does not look at the factory bindings again.
    if (!parse_settings(default_settings_text(), &s, &err)) {
        s = builtin_settings();
        s.defaults_rev = defaults_rev();
    }
    return s;
}

bool parse_settings(const std::string& text, Settings* out, std::string* err, std::vector<std::string>* warnings) {
    Settings s = builtin_settings();
    std::string perr;
    if (!parse_onto(text, &s, &perr, warnings, true)) {
        if (err) *err = perr;
        return false;
    }
    *out = s;
    return true;
}

std::string format_settings(const Settings& s) {
    std::string o;
    auto kv = [&o](const char* key, const std::string& value) { o += std::string(key) + " = " + value + "\n"; };
    auto flag = [](bool b) { return std::string(b ? "1" : "0"); };

    const char* title = game::info().title;
    o += (title && *title) ? std::string("# ") + title + ", play mode: controls and display.\n" : std::string("# Play mode: controls and display.\n");
    o += "\n[meta]\n";
    kv("defaults_rev", std::to_string(s.defaults_rev));
    o += "\n[mouse]\n";
    kv("yaw", num(s.mouse.yaw_rad));
    kv("pitch", num(s.mouse.pitch_rad));
    kv(key_alt_camera(), flag(s.mouse.alt_camera));
    kv(key_alt_sens(), num(source_sens(s.mouse.alt_yaw_rad)));
    if (s.mouse.alt_pitch_rad != s.mouse.alt_yaw_rad) kv(key_alt_pitch(), num(s.mouse.alt_pitch_rad));   // a vertical of its own, after the pair
    kv("invert_y", flag(s.mouse.invert_y));
    kv("invert_x", flag(s.mouse.invert_x));
    kv("menu_cursor", flag(s.mouse.menu_cursor));
    kv(key_wheel_cycles(), flag(s.mouse.wheel_cycles));
    kv("jc2_speed", num(s.mouse.jc2_speed));
    kv("jc2_wake", num(s.mouse.jc2_wake));
    kv("jc2_capture", flag(s.mouse.jc2_capture));

    o += "\n[aim]\n";
    kv("lock_mode", lock_mode_name(s.aim.lock_mode));
    kv("free_lock_margin", num(s.aim.free_lock_margin));
    o += "\n[camera]\n";
    kv("gun_drives_view", flag(s.camera.gun_drives_view));
    o += "\n[tweaks]\n";
    fastboot_format(s.fastboot, &o);
    o += format_tweaks(s.tweaks);

    o += "\n[pad]\n";
    kv("label_abxy", flag(s.pad.label_abxy));
    kv("deadzone", num(s.pad.stick_deadzone));
    kv("right_deadzone", num(s.pad.right_deadzone));
    kv("invert_y", flag(s.pad.invert_y));
    kv("look_sens", num(s.pad.look_sens));
    kv("trigger_threshold", num(s.pad.trigger_threshold));
    kv("stick_max", num(s.pad.stick_max));
    kv("gyro", flag(s.pad.gyro));
    kv("gyro_scale", num(s.pad.gyro_scale));
    kv("rumble", flag(s.pad.rumble));
    kv("rumble_scale", num(s.pad.rumble_scale));
    kv("haptic", gfx::haptic::mode_name(static_cast<gfx::haptic::Mode>(s.pad.haptic)));
    kv("haptic_strength", std::to_string(s.pad.haptic_strength));
    kv(key_alt_orbit(), num(s.pad.alt_orbit));

    o += "\n[padmap]\n";
    for (int i = 0; i < kPadSrcCount; ++i) {
        Output out;
        out.buttons = s.pad.remap[i];
        kv(pad_src_name(static_cast<PadSrc>(i)), s.pad.remap[i] == kPadDefault ? std::string("default") : output_text(out));
    }

    o += "\n[timing]\n";
    kv("tap_ms", std::to_string(s.timing.tap_ms));
    kv("hold_ms", std::to_string(s.timing.hold_ms));
    kv("double_ms", std::to_string(s.timing.double_ms));
    kv("pulse_ms", std::to_string(s.timing.pulse_ms));

    o += "\n[display]\n";
    kv("window_mode", s.display.window_mode);
    kv("vsync", flag(s.display.vsync));
    kv("fps_limit", s.display.fps_cap < 0 ? std::string("auto") : std::to_string(s.display.fps_cap));
    kv("width", std::to_string(s.display.width));
    kv("height", std::to_string(s.display.height));
    kv("keep_aspect", flag(s.display.keep_aspect));
    kv("render_scale", s.display.render_scale <= 0 ? std::string("auto") : std::to_string(s.display.render_scale));
    kv("render_width", std::to_string(s.display.render_width));
    kv("render_height", std::to_string(s.display.render_height));
    kv("aspect", s.display.aspect <= 0 ? std::string("auto") : num_exact(s.display.aspect));
    kv("fov_offset", num_exact(s.display.fov_offset));
    kv("dynamic_resolution", flag(s.display.dynamic_resolution));
    kv("anisotropy", std::to_string(s.display.anisotropy));
    kv("lod_bias", num_exact(s.display.lod_bias));
    kv("bloom", flag(s.display.bloom));
    kv("chromatic_aberration", flag(s.display.chromatic_aberration));
    kv("quality_preset", s.display.quality_preset);
    kv("view_distance", num_exact(s.display.view_distance));
    kv("fast_gpu_memory", flag(s.display.fast_gpu_memory));
    kv("fewer_gpu_waits", flag(s.display.fewer_gpu_waits));
    kv("shader_prewarm", flag(s.display.shader_prewarm));
    kv("astc_quality", s.display.astc_quality);
    kv("monitor", s.display.monitor <= 0 ? std::string("auto") : std::to_string(s.display.monitor));

    o += "\n[audio]\n";
    kv("volume", num_exact(s.audio.volume));
    kv("mute_unfocused", flag(s.audio.mute_unfocused));
    kv("device", s.audio.device.empty() ? std::string("default") : s.audio.device);
    kv("channels", s.audio.channels == 6 ? std::string("6") : std::string("2"));

    o += "\n[hdr]\n";
    kv("mode", kHdrModeWords[static_cast<int>(s.extras.hdr.mode)]);
    kv("format", kHdrFormatWords[static_cast<int>(s.extras.hdr.format)]);
    kv("peak_nits", num_exact(s.extras.hdr.peak_nits));
    kv("paper_white_nits", num_exact(s.extras.hdr.paper_white_nits));
    kv("highlight_boost", num_exact(s.extras.hdr.highlight_boost));

    o += "\n[upscale]\n";
    kv("mode", kUpscaleWords[static_cast<int>(s.extras.upscale.mode)]);
    kv("quality", kUpscaleQualityWords[static_cast<int>(s.extras.upscale.quality)]);
    kv("sharpness", num_exact(s.extras.upscale.sharpness));
    kv("dlss_preset", extras::dlss_preset_word(s.extras.upscale.dlss_preset));
    kv("scene_scale", std::to_string(s.extras.upscale.scene_scale));
    kv("fsr_dll", s.extras.upscale.fsr_dll);
    kv("dlss_dll", s.extras.upscale.dlss_dll);
    kv("xess_dll", s.extras.upscale.xess_dll);
    kv("frame_generation", flag(s.extras.upscale.frame_generation));

    o += "\n[latency]\n";
    kv("mode", kLatencyWords[static_cast<int>(s.extras.latency.mode)]);
    kv("frames_ahead", std::to_string(s.extras.latency.frames_ahead));
    kv("log_timings", flag(s.extras.latency.log_timings));

    o += "\n[picture]\n";
    kv("contrast", num_exact(s.extras.picture.contrast));
    kv("saturation", num_exact(s.extras.picture.saturation));
    kv("black", num_exact(s.extras.picture.black));
    kv("gamma", num_exact(s.extras.picture.gamma));
    o += "\n[prompts]\n";
    kv("style", prompt_style_name(s.prompts.style));

    o += "\n[fps]\n";
    kv("show", flag(s.fps.show));
    kv("key", input_name(s.fps.key));
    kv("corner", corner_name(s.fps.corner));
    kv("detail", std::to_string(s.fps.detail));
    kv("in_screenshots", flag(s.fps.in_screenshots));

    o += "\n[switch]\n";
    for (int a = 1; a < kActCount; ++a) {   // every action except none
        kv(act_info(static_cast<Act>(a)).name, output_text(s.out[a]));
    }

    // Always written, even when empty, so that a file with every binding removed does not fall back to the defaults.
    for (int c = 0; c < kCtxCount; ++c) {
        o += std::string("\n[bindings.") + ctx_name(static_cast<Ctx>(c)) + "]\n";
        for (const Binding& b : s.bindings) {
            if (static_cast<int>(b.ctx) != c) continue;
            const std::string key = input_name(b.in);
            if (key.empty()) continue;   // an input with no name cannot be written; it cannot be read back either
            o += key + " = " + act_info(b.act).name;
            if (b.trig != Trig::Press || b.ms != 0) o += std::string(" ") + trig_name(b.trig);
            if (b.ms != 0) o += " " + std::to_string(b.ms);
            o += "\n";
        }
    }
    return o;
}

bool load_settings(const std::string& path, Settings* out, std::string* err) {
    namespace fs = std::filesystem;
    if (err) err->clear();
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec) {
        *out = default_settings();
        if (err) *err = "cannot check " + path + ": " + ec.message();
        return false;
    }
    if (!exists) {   // a first run: no file yet, not an error
        *out = default_settings();
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *out = default_settings();
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);   // a UTF-8 byte order mark, written by some editors

    Settings s;
    std::string perr;
    if (!parse_settings(text, &s, &perr)) {
        *out = default_settings();
        if (err) *err = path + ": " + perr;
        return false;
    }
    *out = s;
    return true;
}

bool save_settings(const std::string& path, const Settings& s, std::string* err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path target(path);
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            if (err) *err = "cannot create folder for " + path + ": " + ec.message();
            return false;
        }
    }
    // An existing file keeps what the player wrote: comments, blank lines, key order and keys this writer does not know (nxrt::ini_merge);
    // the bindings sections are written whole (an input may appear in several lines).
    std::string text = format_settings(s);
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (old.compare(0, 3, "\xEF\xBB\xBF") == 0) old.erase(0, 3);
            std::vector<std::string> whole;
            for (int c = 0; c < kCtxCount; ++c) whole.push_back(std::string("bindings.") + ctx_name(static_cast<Ctx>(c)));
            for (std::string& w : whole)
                for (char& ch : w) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            text = ini_merge(old, text, whole);
        }
    }
    // Write the whole file to a temp name first, then rename it over the target, so a crash never leaves half a file.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (err) *err = "cannot write " + tmp;
            return false;
        }
        out << text;
        out.flush();
        if (!out) {
            if (err) *err = "cannot write " + tmp;
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, target, ec);   // replaces an existing file on Windows
    if (ec) {
        std::error_code rm_ec;
        fs::remove(tmp, rm_ec);
        if (err) *err = "cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace nxrt::play
