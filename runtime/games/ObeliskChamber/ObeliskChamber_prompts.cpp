// SPDX-License-Identifier: GPL-3.0-or-later
// Fire Emblem: Fortune's Weave, button prompts for the controller in use (notes/prompts.md).
//
// How the game draws its prompts: every button icon is a character of the text (" Text Log", "Press ."), a glyph of the
// system's extension font __nintendo_ext_003.bffnt (romfs Font/, zstd), which the game's composite fonts (Font/*.bfcpx) use for
// U+E000..U+E1FF. The button glyphs it uses are the filled set: U+E0E0 A, E0E1 B, E0E2 X, E0E3 Y, E0E4 L, E0E5 R, E0E6 ZL, E0E7 ZR,
// E0EF +, E0F0 - (found in Msg/*/System.msbt, Tips.msbt, Tutorial.msbt ...). The font comes in three sizes (_Small 256x1024, normal
// 1024x1024, _Large 1024x1024 x 4 layers), each a BC4 glyph sheet (a BNTX inside the bffnt, Switch 2 block-linear, rows stored bottom
// up). The game uploads each sheet once into an NVN texture whose storage is the file's bytes.
//
// What this does: no hook in the game's code. It finds those textures by their content (nxrt/tex_patch.h texture_find_by_content, against
// the sheet read from the player's romfs), and overwrites the glyph cells of the ten buttons with the icon of the button or key that does
// that Switch button now: PlayStation (Cross, Circle, ... L1, R2, Options, Create), Xbox (A, B, ... LB, RT, Menu, View), keyboard keys
// (from the bindings in controls.ini), or the game's own glyphs again (Switch style). The style is the base's ([prompts] style, auto
// follows the device used last: play/prompts.h prompts_effective_style). Every text that uses the glyph then shows it, at once and in all
// three sizes. The game's pixels are kept and put back for the Switch style.
//
// The icons are drawn here, from simple shapes: the button's footprint is the game's own glyph with its letter filled in (the disc of A,
// the tab of L, the larger tab of ZL), and the symbol or text is cut out of it, so they sit and scale like the game's. Xbox A/B/X/Y use
// the game's own A/B/X/Y glyphs (the same letters), mouse buttons its own mouse glyphs (U+E11A..E11C). Letters come from a small stroke
// font defined below. No artwork is copied from anywhere.
#include <zstd.h>

#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "ObeliskChamber_game.h"
#include "nxrt/config.h"
#include "nxrt/input.h"
#include "nxrt/log.h"
#include "nxrt/services.h"
#include "nxrt/tex_patch.h"
#include "play_game.h"
#include "play_types.h"
#include "prompt_resolve.h"
#include "prompts.h"

namespace nxrt::games::ObeliskChamber {

namespace {

constexpr const char* kTag = "fefw.prompts";
constexpr int kBc4 = 0x49;   // NVN_FORMAT_RGTC1_UNORM

// ---- the glyphs ------------------------------------------------------------------------------------------------------------------------
// Face-button positions, as the bits of a dot cluster (the game's "position" glyphs: four dots, the meant ones filled).
enum : uint8_t { kTop = 1, kRight = 2, kBottom = 4, kLeft = 8 };
struct Target {
    uint32_t code;
    play::Btn btn;        // the button of a single-button glyph; None for a pair
    uint8_t dots = 0;     // dot clusters: the Switch positions filled (A right, B bottom, X top, Y left); 0 = a button glyph
};
constexpr Target kTargets[] = {
    {0xE0E0, play::Btn::A}, {0xE0E1, play::Btn::B}, {0xE0E2, play::Btn::X}, {0xE0E3, play::Btn::Y}, {0xE0E4, play::Btn::L},
    {0xE0E5, play::Btn::R}, {0xE0E6, play::Btn::ZL}, {0xE0E7, play::Btn::ZR}, {0xE0EF, play::Btn::Plus}, {0xE0F0, play::Btn::Minus},
    // the position glyphs (the tutorial pager, "press <Y/A> during Auto-Battle"): singles A B X Y, then the six pairs
    {0xE0AB, play::Btn::A, kRight}, {0xE0AC, play::Btn::B, kBottom}, {0xE0AD, play::Btn::X, kTop}, {0xE0AE, play::Btn::Y, kLeft},
    {0xE0CA, play::Btn::None, kRight | kBottom}, {0xE0CB, play::Btn::None, kLeft | kRight}, {0xE0CC, play::Btn::None, kTop | kRight},
    {0xE0CD, play::Btn::None, kBottom | kLeft}, {0xE0CE, play::Btn::None, kTop | kBottom}, {0xE0CF, play::Btn::None, kTop | kLeft},
};
constexpr int kTargetCount = static_cast<int>(sizeof kTargets / sizeof kTargets[0]);
constexpr int kSingleDots = 10;   // index of E0AB; E0AB..E0AE are kTargets[10..13], one filled dot each (right, bottom, top, left)
// Cells only read: the mouse glyphs (plain mouse, left click, right click).
constexpr uint32_t kMouse = 0xE11A, kMouseLeft = 0xE11B, kMouseRight = 0xE11C;
constexpr uint32_t kSources[] = {kMouse, kMouseLeft, kMouseRight};

// ---- a glyph image: upright, cw x ch, 0..255 coverage --------------------------------------------------------------------------------
struct Img {
    int w = 0, h = 0;
    std::vector<uint8_t> a;
    uint8_t at(int x, int y) const { return (x < 0 || y < 0 || x >= w || y >= h) ? 0 : a[static_cast<size_t>(y) * w + x]; }
};

struct Box {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // inclusive-exclusive
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    float cx() const { return 0.5f * (x0 + x1); }
    float cy() const { return 0.5f * (y0 + y1); }
    float w() const { return static_cast<float>(x1 - x0); }
    float h() const { return static_cast<float>(y1 - y0); }
};

Box bbox(const Img& im, int thr = 16) {
    Box b{im.w, im.h, 0, 0};
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x)
            if (im.at(x, y) >= thr) {
                b.x0 = std::min(b.x0, x); b.y0 = std::min(b.y0, y);
                b.x1 = std::max(b.x1, x + 1); b.y1 = std::max(b.y1, y + 1);
            }
    return b;
}

// The footprint of a glyph: its holes (the letter cut out of the disc or tab) filled in. Pixels the outside does not reach through
// low coverage are inside.
Img filled(const Img& im) {
    Img o = im;
    std::vector<uint8_t> out(im.a.size(), 0);
    std::vector<int> st;
    auto push = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= im.w || y >= im.h) return;
        const size_t i = static_cast<size_t>(y) * im.w + x;
        if (out[i] || im.a[i] >= 128) return;
        out[i] = 1;
        st.push_back(static_cast<int>(i));
    };
    for (int x = 0; x < im.w; ++x) { push(x, 0); push(x, im.h - 1); }
    for (int y = 0; y < im.h; ++y) { push(0, y); push(im.w - 1, y); }
    while (!st.empty()) {
        const int i = st.back();
        st.pop_back();
        const int x = i % im.w, y = i / im.w;
        push(x + 1, y); push(x - 1, y); push(x, y + 1); push(x, y - 1);
    }
    for (size_t i = 0; i < o.a.size(); ++i)
        if (!out[i]) o.a[i] = 255;
    return o;
}

// ---- vector shapes, 4x4 supersampled -------------------------------------------------------------------------------------------------
struct Seg { float ax, ay, bx, by; };

float seg_dist(float px, float py, const Seg& s) {
    const float dx = s.bx - s.ax, dy = s.by - s.ay;
    const float l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((px - s.ax) * dx + (py - s.ay) * dy) / l2 : 0.f;
    t = std::clamp(t, 0.f, 1.f);
    const float qx = s.ax + t * dx - px, qy = s.ay + t * dy - py;
    return std::sqrt(qx * qx + qy * qy);
}

// A cut-out: strokes (segments of half width hw) and rings.
struct Cut {
    std::vector<Seg> segs;
    float hw = 1.f;
    struct Ring { float cx, cy, r, hw; };
    std::vector<Ring> rings;
    bool inside(float x, float y) const {
        for (const Seg& s : segs)
            if (seg_dist(x, y, s) <= hw) return true;
        for (const Ring& r : rings)
            if (std::fabs(std::hypot(x - r.cx, y - r.cy) - r.r) <= r.hw) return true;
        return false;
    }
};

// base coverage minus the cut, supersampled
Img carve(const Img& base, const Cut& cut) {
    Img o = base;
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x) {
            uint8_t& p = o.a[static_cast<size_t>(y) * o.w + x];
            if (!p) continue;
            int n = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx)
                    if (cut.inside(x + (sx + 0.5f) / 4.f, y + (sy + 0.5f) / 4.f)) ++n;
            p = static_cast<uint8_t>(p * (16 - n) / 16);
        }
    return o;
}

// A rounded rectangle filled, supersampled.
Img round_rect(int w, int h, float x0, float y0, float x1, float y1, float r) {
    Img o{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h, 0)};
    const float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1), hx = 0.5f * (x1 - x0) - r, hy = 0.5f * (y1 - y0) - r;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int n = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const float px = std::fabs(x + (sx + 0.5f) / 4.f - cx) - hx, py = std::fabs(y + (sy + 0.5f) / 4.f - cy) - hy;
                    const float d = std::hypot(std::max(px, 0.f), std::max(py, 0.f)) + std::min(std::max(px, py), 0.f) - r;
                    if (d <= 0) ++n;
                }
            o.a[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(n * 255 / 16);
        }
    return o;
}

// ---- stroke font: capital letters, digits and a few key symbols, on a 4 x 6 grid (symbols 6 x 6), y down ------------------------------
// Polylines separated by '|', points "x,y" separated by spaces. Drawn here for this purpose (simple geometric strokes).
struct StrokeGlyph { char c; float w; const char* path; };
constexpr StrokeGlyph kStroke[] = {
    {'A', 4, "0,6 2,0 4,6|0.7,4 3.3,4"},
    {'B', 4, "0,3 3,3 4,4 4,5 3,6 0,6 0,0 3,0 4,1 4,2 3,3"},
    {'C', 4, "4,1 3,0 1,0 0,1 0,5 1,6 3,6 4,5"},
    {'D', 4, "0,0 0,6 2.4,6 4,4.4 4,1.6 2.4,0 0,0"},
    {'E', 4, "4,0 0,0 0,6 4,6|0,3 3,3"},
    {'F', 4, "4,0 0,0 0,6|0,3 3,3"},
    {'G', 4, "4,1 3,0 1,0 0,1 0,5 1,6 3,6 4,5 4,3.2 2.2,3.2"},
    {'H', 4, "0,0 0,6|4,0 4,6|0,3 4,3"},
    {'I', 2, "1,0 1,6|0,0 2,0|0,6 2,6"},
    {'J', 4, "4,0 4,5 3,6 1,6 0,5"},
    {'K', 4, "0,0 0,6|4,0 0,4|1.4,2.9 4,6"},
    {'L', 4, "0,0 0,6 4,6"},
    {'M', 5, "0,6 0,0 2.5,3.5 5,0 5,6"},
    {'N', 4, "0,6 0,0 4,6 4,0"},
    {'O', 4, "1,0 3,0 4,1 4,5 3,6 1,6 0,5 0,1 1,0"},
    {'P', 4, "0,6 0,0 3,0 4,1 4,2 3,3 0,3"},
    {'Q', 4, "1,0 3,0 4,1 4,5 3,6 1,6 0,5 0,1 1,0|2.6,4.6 4,6"},
    {'R', 4, "0,6 0,0 3,0 4,1 4,2 3,3 0,3|2,3 4,6"},
    {'S', 4, "4,1 3,0 1,0 0,1 0,2 1,3 3,3 4,4 4,5 3,6 1,6 0,5"},
    {'T', 4, "0,0 4,0|2,0 2,6"},
    {'U', 4, "0,0 0,5 1,6 3,6 4,5 4,0"},
    {'V', 4, "0,0 2,6 4,0"},
    {'W', 5, "0,0 1.2,6 2.5,2.5 3.8,6 5,0"},
    {'X', 4, "0,0 4,6|4,0 0,6"},
    {'Y', 4, "0,0 2,3 4,0|2,3 2,6"},
    {'Z', 4, "0,0 4,0 0,6 4,6"},
    {'0', 4, "1,0 3,0 4,1 4,5 3,6 1,6 0,5 0,1 1,0"},
    {'1', 3, "0.5,1 1.5,0 1.5,6|0,6 3,6"},
    {'2', 4, "0,1 1,0 3,0 4,1 4,2 0,6 4,6"},
    {'3', 4, "0,1 1,0 3,0 4,1 4,2 3,3 1.5,3|3,3 4,4 4,5 3,6 1,6 0,5"},
    {'4', 4, "3,6 3,0 0,4 4,4"},
    {'5', 4, "4,0 0,0 0,3 3,3 4,4 4,5 3,6 0,6"},
    {'6', 4, "3.5,0 2,0 0,2 0,5 1,6 3,6 4,5 4,4 3,3 0,3"},
    {'7', 4, "0,0 4,0 1.5,6"},
    {'8', 4, "1,3 0,2 0,1 1,0 3,0 4,1 4,2 3,3 1,3 0,4 0,5 1,6 3,6 4,5 4,4 3,3"},
    {'9', 4, "4,3 1,3 0,2 0,1 1,0 3,0 4,1 4,4 2,6 0.5,6"},
    // key symbols (6 x 6)
    {'\x01', 6, "5,0.5 5,4 1,4|2.6,2.4 1,4 2.6,5.6"},                    // enter
    {'\x02', 6, "0.3,2.5 0.3,4.7 5.7,4.7 5.7,2.5"},                       // space
    {'\x03', 6, "3,0.4 0.4,3 2,3 2,5.6 4,5.6 4,3 5.6,3 3,0.4"},          // shift
    {'\x04', 6, "0.3,3 4.8,3|2.8,1 4.8,3 2.8,5|5.7,0.8 5.7,5.2"},         // tab
    {'\x05', 6, "5.7,3 1,3|3,1 1,3 3,5"},                                 // backspace / left
    {'\x06', 6, "3,5.7 3,1|1,3 3,1 5,3"},                                 // up
    {'\x07', 6, "3,0.3 3,5|1,3 3,5 5,3"},                                 // down
    {'\x08', 6, "0.3,3 5,3|3,1 5,3 3,5"},                                 // right
};

const StrokeGlyph* stroke_glyph(char c) {
    for (const StrokeGlyph& g : kStroke)
        if (g.c == c) return &g;
    return nullptr;
}

// Text centred on (cx, cy), at most maxw x maxh pixels including the stroke, added to the cut.
void add_text(Cut& cut, const std::string& text, float cx, float cy, float maxw, float maxh, float weight = 0.62f) {
    constexpr float kGap = 1.5f, kH = 6.f;
    float units = 0;
    int n = 0;
    for (char c : text)
        if (const StrokeGlyph* g = stroke_glyph(c)) { units += g->w; ++n; }
    if (!n) return;
    units += kGap * (n - 1);
    const float thick = 2 * weight;   // stroke width in grid units
    float s = std::min(maxw / (units + thick), maxh / (kH + thick));
    cut.hw = std::max(0.7f, weight * s);
    float x = cx - 0.5f * units * s;
    const float y0 = cy - 0.5f * kH * s;
    for (char c : text) {
        const StrokeGlyph* g = stroke_glyph(c);
        if (!g) continue;
        const char* p = g->path;
        float px = 0, py = 0;
        bool have = false;
        while (*p) {
            if (*p == '|') { have = false; ++p; continue; }
            if (*p == ' ') { ++p; continue; }
            char* e = nullptr;
            const float gx = std::strtof(p, &e);
            p = e + 1;   // the comma
            const float gy = std::strtof(p, &e);
            p = e;
            const float qx = x + gx * s, qy = y0 + gy * s;
            if (have) cut.segs.push_back({px, py, qx, qy});
            px = qx; py = qy; have = true;
        }
        x += (g->w + kGap) * s;
    }
}

// The label of a key on a key cap: one or two capital letters or digits, a symbol, or a short name.
std::string key_label(const std::string& key) {
    static const std::map<std::string, std::string> kNames = {
        {"return", "\x01"}, {"keypad enter", "\x01"}, {"space", "\x02"}, {"left shift", "\x03"}, {"right shift", "\x03"},
        {"tab", "\x04"}, {"backspace", "\x05"}, {"left", "\x05"}, {"up", "\x06"}, {"down", "\x07"}, {"right", "\x08"},
        {"escape", "ESC"}, {"left ctrl", "CTL"}, {"right ctrl", "CTL"}, {"left alt", "ALT"}, {"right alt", "ALT"},
        {"delete", "DEL"}, {"insert", "INS"}, {"home", "HOM"}, {"end", "END"}, {"page up", "PGU"}, {"page down", "PGD"},
    };
    if (auto it = kNames.find(key); it != kNames.end()) return it->second;
    if (key.size() == 1 && std::isalnum(static_cast<unsigned char>(key[0]))) return std::string(1, static_cast<char>(std::toupper(key[0])));
    if (key.size() <= 3 && key[0] == 'f') {   // f1 .. f12
        std::string s = "F" + key.substr(1);
        bool ok = s.size() > 1;
        for (size_t i = 1; i < s.size(); ++i) ok &= std::isdigit(static_cast<unsigned char>(s[i])) != 0;
        if (ok) return s;
    }
    if (key.rfind("keypad ", 0) == 0 && key.size() == 8 && std::isdigit(static_cast<unsigned char>(key[7]))) return std::string(1, key[7]);
    std::string s;
    for (char c : key)
        if (std::isalnum(static_cast<unsigned char>(c)) && s.size() < 3) s += static_cast<char>(std::toupper(c));
    return s;
}

// ---- one font size -------------------------------------------------------------------------------------------------------------------
struct Font {
    const char* file;              // under romfs/Font
    bool loaded = false;
    std::vector<uint8_t> sheet;    // the BNTX image data (all layers), tiled as the game loads it
    uint32_t sw = 0, sh = 0, layers = 1;
    size_t layer_bytes = 0;
    uint32_t cw = 0, ch = 0, cols = 0, rows = 0;
    std::map<uint32_t, uint32_t> cmap;   // code -> glyph index
    // per live texture: the name shown in each target cell ("" = the game's)
    struct Live { uint32_t id; std::array<std::string, kTargetCount> shown; };
    std::vector<Live> live;
    std::vector<uint32_t> ids;
    // the game's pixels (read from the first texture found): each target's covering blocks and image, the source cells' images
    bool have_orig = false;
    struct Cell { uint32_t layer = 0, bx = 0, by = 0, bw = 0, bh = 0; std::vector<uint8_t> blocks; Img img; uint32_t x = 0, yu = 0; };
    std::array<Cell, kTargetCount> tcell;
    std::map<uint32_t, Img> src;
};

uint16_t rd16(const std::vector<uint8_t>& b, size_t o) { return o + 2 <= b.size() ? static_cast<uint16_t>(b[o] | (b[o + 1] << 8)) : 0; }
uint32_t rd32(const std::vector<uint8_t>& b, size_t o) {
    return o + 4 <= b.size() ? static_cast<uint32_t>(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (static_cast<uint32_t>(b[o + 3]) << 24)) : 0;
}

bool read_zst(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> comp((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const unsigned long long n = ZSTD_getFrameContentSize(comp.data(), comp.size());
    if (n == ZSTD_CONTENTSIZE_ERROR || n == ZSTD_CONTENTSIZE_UNKNOWN || n > (64ull << 20)) return false;
    out.resize(static_cast<size_t>(n));
    const size_t r = ZSTD_decompress(out.data(), out.size(), comp.data(), comp.size());
    return !ZSTD_isError(r) && r == n;
}

// FFNT 4.x: FINF (glyph, width and map pointers), TGLP (cells and the sheet: a BNTX with one BRTI and its BRTD data), CMAP chain.
bool load_font(Font& f, const std::string& romfs) {
    std::vector<uint8_t> b;
    const std::filesystem::path p = std::filesystem::path(romfs) / "Font" / f.file;
    if (!read_zst(p, b) || b.size() < 0x40 || std::memcmp(b.data(), "FFNT", 4) != 0) {
        NXLOG_W(kTag, "%s: cannot read the font", p.string().c_str());
        return false;
    }
    const size_t finf = rd16(b, 6);
    if (std::memcmp(b.data() + finf, "FINF", 4) != 0) return false;
    const uint32_t pglyph = rd32(b, finf + 0x14), pmap = rd32(b, finf + 0x1c);
    const size_t t = pglyph - 8;
    if (t + 0x20 > b.size() || std::memcmp(b.data() + t, "TGLP", 4) != 0) return false;
    f.cw = b[t + 8];
    f.ch = b[t + 9];
    const uint32_t fmt = rd16(b, t + 18);
    f.cols = rd16(b, t + 20);
    f.rows = rd16(b, t + 22);
    f.sw = rd16(b, t + 24);
    f.sh = rd16(b, t + 26);
    const uint32_t data_off = rd32(b, t + 28);
    if (fmt != 12 || !f.cols || !f.rows) { NXLOG_W(kTag, "%s: sheet format %u, expected BC4 (12)", f.file, fmt); return false; }
    // the BNTX: BRTI (format 0x1d01 = BC4 unorm, width, height, array count), BRTD (the image data after a 16-byte header)
    auto find = [&](const char* tag) -> size_t {
        for (size_t i = data_off; i + 4 <= b.size(); i += 4)
            if (std::memcmp(b.data() + i, tag, 4) == 0) return i;
        return 0;
    };
    const size_t brti = find("BRTI"), brtd = find("BRTD");
    if (!brti || !brtd) return false;
    const uint32_t tex_fmt = rd32(b, brti + 0x1c), w = rd32(b, brti + 0x24), h = rd32(b, brti + 0x28), arr = rd32(b, brti + 0x30);
    if (tex_fmt != 0x1d01 || w != f.sw || h != f.sh || !arr) { NXLOG_W(kTag, "%s: unexpected texture %#x %ux%u", f.file, tex_fmt, w, h); return false; }
    f.layers = arr;
    f.layer_bytes = static_cast<size_t>(w) * h / 2;   // BC4: 8 bytes per 4x4 block (the 16-GOB block height fits these sizes exactly)
    const size_t data = brtd + 0x10;
    if (data + f.layer_bytes * f.layers > b.size()) return false;
    f.sheet.assign(b.begin() + data, b.begin() + data + f.layer_bytes * f.layers);
    // CMAP chain: direct (0), table (1), scan (2: u32 count, then u32 code + u16 index + pad)
    for (uint32_t m = pmap; m && m + 16 <= b.size();) {
        const uint32_t first = rd32(b, m), last = rd32(b, m + 4);
        const uint16_t method = rd16(b, m + 8);
        const uint32_t next = rd32(b, m + 12);
        const size_t d = m + 16;
        if (method == 0) {
            const uint16_t base = rd16(b, d);
            for (uint32_t c = first; c <= last && c - first < 0x10000; ++c) f.cmap[c] = base + (c - first);
        } else if (method == 1) {
            for (uint32_t c = first; c <= last && c - first < 0x10000; ++c)
                if (const uint16_t v = rd16(b, d + 2 * (c - first)); v != 0xffff) f.cmap[c] = v;
        } else {
            const uint32_t n = rd32(b, d);
            for (uint32_t i = 0; i < n && d + 4 + 8 * i + 6 <= b.size(); ++i) f.cmap[rd32(b, d + 4 + 8 * i)] = rd16(b, d + 4 + 8 * i + 4);
        }
        m = next;
    }
    for (const Target& tg : kTargets)
        if (!f.cmap.count(tg.code)) { NXLOG_W(kTag, "%s: no glyph U+%04X", f.file, tg.code); return false; }
    NXLOG_I(kTag, "%s: %ux%u x%u BC4 sheet, cells %ux%u (%u x %u), %zu codes", f.file, f.sw, f.sh, f.layers, f.cw, f.ch, f.cols, f.rows,
            f.cmap.size());
    f.loaded = true;
    return true;
}

// Where a glyph's cell is: layer, x and the upright y of its top-left texel (cells start at 1 and are cw+1 / ch+1 apart).
bool cell_of(const Font& f, uint32_t code, uint32_t* layer, uint32_t* x, uint32_t* yu) {
    auto it = f.cmap.find(code);
    if (it == f.cmap.end()) return false;
    const uint32_t per = f.cols * f.rows, gi = it->second;
    *layer = gi / per;
    const uint32_t k = gi % per;
    *x = 1 + (k % f.cols) * (f.cw + 1);
    *yu = 1 + (k / f.cols) * (f.ch + 1);
    return *layer < f.layers && *x + f.cw <= f.sw && *yu + f.ch <= f.sh;
}

// ---- BC4 -------------------------------------------------------------------------------------------------------------------------------
void bc4_decode_block(const uint8_t* blk, uint8_t out[16]) {
    const int a0 = blk[0], a1 = blk[1];
    int pal[8] = {a0, a1};
    if (a0 > a1) for (int i = 1; i < 7; ++i) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    else { for (int i = 1; i < 5; ++i) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5; pal[6] = 0; pal[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(blk[2 + i]) << (8 * i);
    for (int p = 0; p < 16; ++p) out[p] = static_cast<uint8_t>(pal[(bits >> (3 * p)) & 7]);
}

void bc4_encode_block(const uint8_t in[16], uint8_t* blk) {
    int lo = 255, hi = 0;
    for (int p = 0; p < 16; ++p) { lo = std::min<int>(lo, in[p]); hi = std::max<int>(hi, in[p]); }
    std::memset(blk, 0, 8);
    blk[0] = static_cast<uint8_t>(hi);
    blk[1] = static_cast<uint8_t>(lo);
    if (hi == lo) return;
    // 8-value mode (a0 > a1): index 0 = hi, 1 = lo, 2..7 = the six steps from hi to lo
    int pal[8] = {hi, lo};
    for (int i = 1; i < 7; ++i) pal[i + 1] = ((7 - i) * hi + i * lo) / 7;
    uint64_t bits = 0;
    for (int p = 0; p < 16; ++p) {
        int best = 0, bd = 1 << 30;
        for (int i = 0; i < 8; ++i)
            if (const int d = std::abs(pal[i] - in[p]); d < bd) { bd = d; best = i; }
        bits |= static_cast<uint64_t>(best) << (3 * p);
    }
    for (int i = 0; i < 6; ++i) blk[2 + i] = static_cast<uint8_t>(bits >> (8 * i));
}

// The covering block rectangle of a cell in storage rows (bottom up), its decoded texels, and the cell's upright image.
struct Covering { uint32_t bx, by, bw, bh; };
Covering covering(const Font& f, uint32_t x, uint32_t yu) {
    const uint32_t ys0 = f.sh - yu - f.ch, ys1 = f.sh - yu;   // storage rows of the cell
    Covering c;
    c.bx = x & ~3u;
    c.by = ys0 & ~3u;
    c.bw = ((x + f.cw + 3) & ~3u) - c.bx;
    c.bh = ((ys1 + 3) & ~3u) - c.by;
    return c;
}

std::vector<uint8_t> decode_rect(const std::vector<uint8_t>& blocks, uint32_t bw, uint32_t bh) {
    std::vector<uint8_t> px(static_cast<size_t>(bw) * bh);
    uint8_t tmp[16];
    for (uint32_t by = 0; by < bh / 4; ++by)
        for (uint32_t bx = 0; bx < bw / 4; ++bx) {
            bc4_decode_block(&blocks[(static_cast<size_t>(by) * (bw / 4) + bx) * 8], tmp);
            for (int p = 0; p < 16; ++p) px[(static_cast<size_t>(by) * 4 + p / 4) * bw + bx * 4 + p % 4] = tmp[p];
        }
    return px;
}

std::vector<uint8_t> encode_rect(const std::vector<uint8_t>& px, uint32_t bw, uint32_t bh) {
    std::vector<uint8_t> blocks(static_cast<size_t>(bw / 4) * (bh / 4) * 8);
    uint8_t tmp[16];
    for (uint32_t by = 0; by < bh / 4; ++by)
        for (uint32_t bx = 0; bx < bw / 4; ++bx) {
            for (int p = 0; p < 16; ++p) tmp[p] = px[(static_cast<size_t>(by) * 4 + p / 4) * bw + bx * 4 + p % 4];
            bc4_encode_block(tmp, &blocks[(static_cast<size_t>(by) * (bw / 4) + bx) * 8]);
        }
    return blocks;
}

// The upright cell image out of a decoded covering rectangle.
Img cell_image(const Font& f, const Covering& c, const std::vector<uint8_t>& px, uint32_t x, uint32_t yu) {
    Img im{static_cast<int>(f.cw), static_cast<int>(f.ch), std::vector<uint8_t>(static_cast<size_t>(f.cw) * f.ch)};
    for (uint32_t cy = 0; cy < f.ch; ++cy)
        for (uint32_t cx = 0; cx < f.cw; ++cx) {
            const uint32_t sx = x + cx - c.bx, sy = (f.sh - 1 - (yu + cy)) - c.by;
            im.a[static_cast<size_t>(cy) * f.cw + cx] = px[static_cast<size_t>(sy) * c.bw + sx];
        }
    return im;
}

// ---- the icons -----------------------------------------------------------------------------------------------------------------------
// A copy of another glyph, centred horizontally on the target's footprint.
Img copy_centred(const Img& src, const Box& target) {
    const Box sb = bbox(src);
    Img o{src.w, src.h, std::vector<uint8_t>(src.a.size(), 0)};
    if (sb.empty()) return o;
    const int dx = static_cast<int>(std::lround(target.cx() - sb.cx()));
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x) {
            const int tx = x + dx;
            if (tx >= 0 && tx < o.w) o.a[static_cast<size_t>(y) * o.w + tx] = src.at(x, y);
        }
    return o;
}

// The icon `name` ("ps/cross", "xbox/lb", "kbm/e", ...) in the cell of target `k`. False: no icon for it (the game's glyph stays).
bool make_icon(const Font& f, int k, const std::string& name, Img* out) {
    const Img& orig = f.tcell[k].img;
    const Box ob = bbox(orig);
    if (ob.empty()) return false;
    const Img solid = filled(orig);
    const float cx = ob.cx(), cy = ob.cy(), r = 0.5f * std::min(ob.w(), ob.h());
    Cut cut;
    auto symbol_hw = [&] { return std::max(0.8f, 0.095f * r); };
    auto in_shape = [&](const std::string& text, float wfrac, float hfrac) { add_text(cut, text, cx, cy, ob.w() * wfrac, ob.h() * hfrac); };
    const std::string kind = name.substr(0, name.find('/'));
    const std::string what = name.substr(name.find('/') + 1);
    if (kind == "dots") {   // a dot cluster from the game's four single-dot glyphs: each quarter from one whose dot there is filled or not
        const int mask = std::atoi(what.c_str());
        const Img* single[4] = {&f.tcell[kSingleDots + 2].img, &f.tcell[kSingleDots].img, &f.tcell[kSingleDots + 1].img,
                                &f.tcell[kSingleDots + 3].img};   // filled dot at top, right, bottom, left
        const Box sb = bbox(*single[0]);
        if (sb.empty()) return false;
        Img o = *single[0];
        for (int y = 0; y < o.h; ++y)
            for (int x = 0; x < o.w; ++x) {
                const float dx = x + 0.5f - sb.cx(), dy = y + 0.5f - sb.cy();
                const int q = std::fabs(dx) > std::fabs(dy) ? (dx > 0 ? 1 : 3) : (dy < 0 ? 0 : 2);   // top, right, bottom, left
                const Img* from = (mask & (1 << q)) ? single[q] : single[(q + 2) % 4];   // the opposite single has a ring there
                o.a[static_cast<size_t>(y) * o.w + x] = from->at(x, y);
            }
        *out = o;
        return true;
    }
    auto source = [&](uint32_t code) -> const Img* { auto it = f.src.find(code); return it == f.src.end() ? nullptr : &it->second; };
    auto target_img = [&](play::Btn b) -> const Img* {
        for (int i = 0; i < kTargetCount; ++i) if (kTargets[i].btn == b) return &f.tcell[i].img;
        return nullptr;
    };
    if (kind == "ps") {
        if (what == "cross" || what == "circle" || what == "square" || what == "triangle") {
            cut.hw = symbol_hw();
            if (what == "cross") {
                const float s = 0.38f * r;
                cut.segs.push_back({cx - s, cy - s, cx + s, cy + s});
                cut.segs.push_back({cx - s, cy + s, cx + s, cy - s});
            } else if (what == "circle") {
                cut.rings.push_back({cx, cy, 0.44f * r, cut.hw});
            } else if (what == "square") {
                const float s = 0.38f * r;
                cut.segs = {{cx - s, cy - s, cx + s, cy - s}, {cx + s, cy - s, cx + s, cy + s}, {cx + s, cy + s, cx - s, cy + s}, {cx - s, cy + s, cx - s, cy - s}};
            } else {
                const float R = 0.52f * r, yc = cy + 0.08f * r;
                const float ax = cx, ay = yc - R, bx = cx + 0.866f * R, by = yc + 0.5f * R, ccx = cx - 0.866f * R;
                cut.segs = {{ax, ay, bx, by}, {bx, by, ccx, by}, {ccx, by, ax, ay}};
            }
            *out = carve(solid, cut);
            return true;
        }
        if (what == "options" || what == "create") {
            cut.hw = symbol_hw();
            if (what == "options") {
                const float s = 0.42f * r;
                for (float dy : {-0.3f, 0.f, 0.3f}) cut.segs.push_back({cx - s, cy + dy * r, cx + s, cy + dy * r});
            } else {   // an upward arrow from a tray (share)
                const float s = 0.4f * r;
                cut.segs = {{cx, cy + 0.15f * r, cx, cy - s}, {cx - 0.28f * r, cy - s + 0.28f * r, cx, cy - s},
                            {cx, cy - s, cx + 0.28f * r, cy - s + 0.28f * r},
                            {cx - s, cy + 0.05f * r, cx - s, cy + s}, {cx - s, cy + s, cx + s, cy + s}, {cx + s, cy + s, cx + s, cy + 0.05f * r}};
            }
            *out = carve(solid, cut);
            return true;
        }
        static const std::map<std::string, std::string> kShoulder = {{"l1", "L1"}, {"r1", "R1"}, {"l2", "L2"}, {"r2", "R2"}};
        if (auto it = kShoulder.find(what); it != kShoulder.end()) {
            in_shape(it->second, 0.70f, 0.50f);
            *out = carve(solid, cut);
            return true;
        }
        return false;
    }
    if (kind == "xbox") {
        static const std::map<std::string, play::Btn> kFace = {{"a", play::Btn::A}, {"b", play::Btn::B}, {"x", play::Btn::X}, {"y", play::Btn::Y}};
        if (auto it = kFace.find(what); it != kFace.end()) {   // the game's own glyph with that letter
            const Img* s = target_img(it->second);
            if (!s) return false;
            *out = copy_centred(*s, ob);
            return true;
        }
        if (what == "menu" || what == "view") {
            cut.hw = symbol_hw();
            if (what == "menu") {
                const float s = 0.4f * r;
                for (float dy : {-0.28f, 0.f, 0.28f}) cut.segs.push_back({cx - s, cy + dy * r, cx + s, cy + dy * r});
            } else {   // two overlapping windows
                const float w2 = 0.26f * r, h2 = 0.2f * r, o = 0.13f * r;
                for (float sgn : {-1.f, 1.f}) {
                    const float x0 = cx + sgn * o - w2, x1 = cx + sgn * o + w2, y0 = cy + sgn * o - h2, y1 = cy + sgn * o + h2;
                    cut.segs.insert(cut.segs.end(), {{x0, y0, x1, y0}, {x1, y0, x1, y1}, {x1, y1, x0, y1}, {x0, y1, x0, y0}});
                }
            }
            *out = carve(solid, cut);
            return true;
        }
        static const std::map<std::string, std::string> kShoulder = {{"lb", "LB"}, {"rb", "RB"}, {"lt", "LT"}, {"rt", "RT"}};
        if (auto it = kShoulder.find(what); it != kShoulder.end()) {
            in_shape(it->second, 0.70f, 0.50f);
            *out = carve(solid, cut);
            return true;
        }
        return false;
    }
    if (kind == "kbm") {
        if (what == "mouse1" || what == "mouse3" || what == "mouse2" || what == "wheelup" || what == "wheeldown") {
            const Img* s = source(what == "mouse1" ? kMouseLeft : what == "mouse3" ? kMouseRight : kMouse);
            if (!s) return false;
            *out = copy_centred(*s, ob);
            return true;
        }
        const std::string label = key_label(what);
        if (label.empty()) return false;
        // a key cap over the footprint's height, as wide as the label needs (at most the footprint's width)
        const float hgt = ob.h() * 0.92f, wid = std::min(ob.w(), std::max(hgt, label.size() > 2 ? ob.w() : hgt * (label.size() == 2 ? 1.15f : 1.f)));
        const Img cap = round_rect(orig.w, orig.h, cx - 0.5f * wid, cy - 0.5f * hgt, cx + 0.5f * wid, cy + 0.5f * hgt, 0.2f * hgt);
        const bool symbol = label.size() == 1 && static_cast<unsigned char>(label[0]) < 0x20;
        add_text(cut, label, cx, cy, wid * (label.size() > 1 ? 0.78f : 0.62f), hgt * (symbol ? 0.62f : label.size() > 2 ? 0.42f : 0.56f),
                 label.size() > 2 ? 0.55f : 0.62f);
        *out = carve(cap, cut);
        return true;
    }
    return false;
}

// ---- state -----------------------------------------------------------------------------------------------------------------------------
struct State {
    bool enabled = true;
    bool tried = false;
    play::GameHost* host = nullptr;
    std::array<Font, 3> fonts{Font{"__nintendo_ext_003.bffnt.zst"}, Font{"__nintendo_ext_003_Small.bffnt.zst"},
                              Font{"__nintendo_ext_003_Large.bffnt.zst"}};
    uint64_t frames = 0;
    std::array<std::string, kTargetCount> want;
    play::PromptStyle last_style = play::PromptStyle::Auto;
    uint32_t writes = 0;
    std::string dump_dir;      // [fefw] prompt_dump: the generated cells as .pgm (checks)
};

State& st() {
    static State s;
    return s;
}

void dump_pgm(const std::string& dir, const std::string& name, const Img& im) {
    if (dir.empty()) return;
    std::string fn = name;
    for (char& c : fn) if (c == '/' || c == ' ') c = '_';
    std::ofstream o(std::filesystem::path(dir) / (fn + ".pgm"), std::ios::binary);
    o << "P5\n" << im.w << " " << im.h << "\n255\n";
    o.write(reinterpret_cast<const char*>(im.a.data()), static_cast<std::streamsize>(im.a.size()));
}

// The game's pixels of every target and source cell, from one live texture of the font.
bool read_originals(Font& f, uint32_t id) {
    for (int k = 0; k < kTargetCount; ++k) {
        Font::Cell& c = f.tcell[k];
        if (!cell_of(f, kTargets[k].code, &c.layer, &c.x, &c.yu)) return false;
        const Covering cv = covering(f, c.x, c.yu);
        c.bx = cv.bx; c.by = cv.by; c.bw = cv.bw; c.bh = cv.bh;
        if (!gfx::texture_read_blocks(id, c.layer, c.bx, c.by, c.bw, c.bh, &c.blocks)) return false;
        c.img = cell_image(f, cv, decode_rect(c.blocks, c.bw, c.bh), c.x, c.yu);
    }
    for (uint32_t code : kSources) {
        uint32_t layer, x, yu;
        if (!cell_of(f, code, &layer, &x, &yu)) continue;
        const Covering cv = covering(f, x, yu);
        std::vector<uint8_t> blocks;
        if (!gfx::texture_read_blocks(id, layer, cv.bx, cv.by, cv.bw, cv.bh, &blocks)) continue;
        f.src[code] = cell_image(f, cv, decode_rect(blocks, cv.bw, cv.bh), x, yu);
    }
    f.have_orig = true;
    return true;
}

// Shows `name` in target k of one texture ("" = the game's glyph). False when the texture is gone.
bool show(Font& f, uint32_t id, int k, const std::string& name) {
    const Font::Cell& c = f.tcell[k];
    if (name.empty()) return gfx::texture_write_blocks(id, c.layer, c.bx, c.by, c.bw, c.bh, c.blocks.data(), c.blocks.size());
    Img icon;
    if (!make_icon(f, k, name, &icon)) return gfx::texture_write_blocks(id, c.layer, c.bx, c.by, c.bw, c.bh, c.blocks.data(), c.blocks.size());
    std::vector<uint8_t> px = decode_rect(c.blocks, c.bw, c.bh);
    for (uint32_t cy = 0; cy < f.ch; ++cy)
        for (uint32_t cx = 0; cx < f.cw; ++cx) {
            const uint32_t sx = c.x + cx - c.bx, sy = (f.sh - 1 - (c.yu + cy)) - c.by;
            px[static_cast<size_t>(sy) * c.bw + sx] = icon.a[static_cast<size_t>(cy) * f.cw + cx];
        }
    const std::vector<uint8_t> blocks = encode_rect(px, c.bw, c.bh);
    return gfx::texture_write_blocks(id, c.layer, c.bx, c.by, c.bw, c.bh, blocks.data(), blocks.size());
}

void scan(State& s) {
    for (Font& f : s.fonts) {
        if (!f.loaded) continue;
        gfx::TexContentMatch m;
        m.nvn_format = kBc4;
        m.width = f.sw;
        m.height = f.sh;
        m.layers = f.layers;
        m.data = f.sheet.data();
        m.size = f.layer_bytes;   // layer 0 is plenty to tell it apart
        for (uint32_t id : gfx::texture_find_by_content(m, &f.ids)) {
            f.ids.push_back(id);
            if (!f.have_orig && !read_originals(f, id)) {
                NXLOG_W(kTag, "%s: texture #%u found, but its glyph cells could not be read", f.file, id);
                continue;
            }
            f.live.push_back({id, {}});
            NXLOG_I(kTag, "%s: the game's glyph sheet is texture #%u", f.file, id);
            if (!s.dump_dir.empty())
                for (int k = 0; k < kTargetCount; ++k) dump_pgm(s.dump_dir, std::string(f.file).substr(2, 22) + "_orig_" + std::to_string(k), f.tcell[k].img);
        }
    }
}

// Where the pad button of a pad icon sits in the diamond (South bottom, East right, West left, North top); 0 = not a face button.
uint8_t pad_position(const std::string& n) {
    if (n == "ps/cross" || n == "xbox/a") return kBottom;
    if (n == "ps/circle" || n == "xbox/b") return kRight;
    if (n == "ps/square" || n == "xbox/x") return kLeft;
    if (n == "ps/triangle" || n == "xbox/y") return kTop;
    return 0;
}

// The pack-style name of the icon for target t in style `style` ("" = the game's glyph). Dot clusters: "dots/<mask>", the positions of
// the pad buttons that do the Switch buttons the game's glyph points at (with the label layout, Switch A is the bottom button).
std::string icon_for(const play::Settings& set, play::PromptStyle style, const Target& t) {
    using play::PromptStyle;
    if (style == PromptStyle::Xbox || style == PromptStyle::PlayStation) {
        if (!t.dots) return play::pad_button_name(style, set.pad, set.pad.label_abxy, t.btn);
        constexpr std::pair<uint8_t, play::Btn> kSwitchPos[] = {{kRight, play::Btn::A}, {kBottom, play::Btn::B}, {kTop, play::Btn::X}, {kLeft, play::Btn::Y}};
        uint8_t mask = 0;
        for (const auto& [pos, b] : kSwitchPos) {
            if (!(t.dots & pos)) continue;
            const uint8_t p = pad_position(play::pad_button_name(style, set.pad, set.pad.label_abxy, b));
            if (!p) return {};
            mask |= p;
        }
        return mask == t.dots ? std::string() : "dots/" + std::to_string(mask);
    }
    if (style == PromptStyle::Keyboard) {
        play::InputId id;
        if (t.btn != play::Btn::None && play::input_for_button(set, play::Ctx::Menu, t.btn, &id)) return play::kbm_pack_name(id);
        // no key does this button (or a pair): the connected pad's icon (keyboard and mouse used last, but the button is the pad's), else
        // the game's
        input::GamepadInfo pads[1];
        if (input::gamepad_list(pads, 1) > 0) {
            const PromptStyle ps = pads[0].kind == input::PadKind::PlayStation ? PromptStyle::PlayStation
                                 : pads[0].kind == input::PadKind::Switch      ? PromptStyle::Switch
                                                                               : PromptStyle::Xbox;
            return icon_for(set, ps, t);
        }
    }
    return {};
}

}  // namespace

void prompts_install(play::GameHost* host) {
    State& s = st();
    s.host = host;
    s.enabled = config_get_bool("fefw", "prompt_glyphs", true);
    s.dump_dir = config_get_str("fefw", "prompt_dump", "");
    if (!s.enabled) NXLOG_I(kTag, "off ([fefw] prompt_glyphs = 0): the game's Switch glyphs stay");
}

void prompts_remove() {
    State& s = st();
    for (Font& f : s.fonts)
        for (Font::Live& l : f.live)
            for (int k = 0; k < kTargetCount; ++k)
                if (!l.shown[k].empty()) show(f, l.id, k, "");
    s.host = nullptr;
}

// Main thread, once per frame (play mode's frame): finds the glyph sheets now and then, and redraws the cells whose icon changed.
void prompts_frame() {
    State& s = st();
    if (!s.enabled || !s.host) return;
    if (!s.tried) {
        s.tried = true;
        const std::string romfs = services::romfs_dir();
        int n = 0;
        for (Font& f : s.fonts) n += load_font(f, romfs) ? 1 : 0;
        if (!n) { s.enabled = false; NXLOG_W(kTag, "no extension font read: the game's glyphs stay"); return; }
    }
    // the sheets are uploaded once the game first draws with them; look every half second (only textures of their size and format)
    if (s.frames++ % 30 == 0) scan(s);
    const play::Settings& set = s.host->settings();
    const play::PromptStyle style = play::prompts_effective_style();
    if (style != s.last_style) {
        NXLOG_I(kTag, "button glyphs: %s", play::prompt_style_name(style));
        s.last_style = style;
    }
    for (int k = 0; k < kTargetCount; ++k) s.want[k] = icon_for(set, style, kTargets[k]);
    for (Font& f : s.fonts) {
        for (size_t i = 0; i < f.live.size();) {
            Font::Live& l = f.live[i];
            bool gone = false;
            for (int k = 0; k < kTargetCount && !gone; ++k) {
                if (l.shown[k] == s.want[k]) continue;
                if (!show(f, l.id, k, s.want[k])) { gone = true; break; }
                if (s.writes++ < 60)
                    NXLOG_I(kTag, "%s #%u: U+%04X -> %s", f.file, l.id, kTargets[k].code, s.want[k].empty() ? "game" : s.want[k].c_str());
                if (!s.dump_dir.empty() && !s.want[k].empty()) {
                    Img icon;
                    if (make_icon(f, k, s.want[k], &icon)) dump_pgm(s.dump_dir, std::string(f.file).substr(2, 22) + "_" + s.want[k], icon);
                }
                l.shown[k] = s.want[k];
            }
            if (gone) {   // the game let go of the texture (a font unloaded): forget it, a new one is found by the scan
                NXLOG_I(kTag, "%s: texture #%u is gone", f.file, l.id);
                f.ids.erase(std::remove(f.ids.begin(), f.ids.end(), l.id), f.ids.end());
                f.live.erase(f.live.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }
    }
}

}  // namespace nxrt::games::ObeliskChamber
