// SPDX-License-Identifier: GPL-3.0-or-later
// BC1 encoding for [gfx] astc_bc = bc1 (nvn_texdata.h): opaque ASTC textures are stored as BC1 (4 bits a texel) instead of BC7 (8 bits).
// Encoder: rgbcx (Richard Geldreich, Jr., MIT or public domain; tools/textures/third_party/bc7enc_rdo/rgbcx.h, fetched with the transcoder's
// other sources), compiled into this file when CMake finds it (NXRT_HAVE_RGBCX, cmake/gfx.cmake). Without it bc1_encoder_available() is false
// and astc_bc falls back to off.
#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "nvn_texdata.h"

#if defined(NXRT_HAVE_RGBCX)
#include "rgbcx.h"   // its implementation, rgbcx.cpp, is a source of nxrt_gfx (cmake/gfx.cmake)
#endif

namespace nxrt::nvn::be {

#if defined(NXRT_HAVE_RGBCX)
namespace {
void bc1_init_once() {
    static std::once_flag once;
    std::call_once(once, [] { rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal); });   // vendor-neutral endpoint interpolation
}

// One row of blocks: the 4x4 texels of each block gathered (edge blocks repeat the last row / column), then encoded.
void encode_rows(uint32_t w, uint32_t h, const uint8_t* rgba, int level, uint8_t* dst, uint32_t by0, uint32_t by1) {
    const uint32_t bw = (w + 3) / 4;
    uint8_t px[64];
    for (uint32_t by = by0; by < by1; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            for (uint32_t y = 0; y < 4; ++y) {
                const uint32_t sy = std::min(h - 1, by * 4 + y);
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t sx = std::min(w - 1, bx * 4 + x);
                    std::memcpy(px + (y * 4 + x) * 4, rgba + (static_cast<size_t>(sy) * w + sx) * 4, 4);
                }
            }
            // 3-colour blocks allowed, and their index 3 (black) used for near-black texels: the image is BC1_RGB, which samples that index as
            // opaque black, so alpha never shows.
            rgbcx::encode_bc1(static_cast<uint32_t>(level), dst + (static_cast<size_t>(by) * bw + bx) * 8, px, true, true);
        }
}
}  // namespace

bool bc1_encoder_available() { return true; }

bool encode_bc1_image(uint32_t w, uint32_t h, const uint8_t* rgba, int level, uint8_t* dst, size_t dst_cap, int threads) {
    if (!w || !h || !rgba || !dst) return false;
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (dst_cap < static_cast<size_t>(bw) * bh * 8) return false;
    bc1_init_once();
    level = std::clamp(level, static_cast<int>(rgbcx::MIN_LEVEL), static_cast<int>(rgbcx::MAX_LEVEL));
    // threads: 1 on a background job; 0 (all hardware threads, at most 8) for a texture the render thread waits for. Small levels stay on one thread.
    uint32_t n = threads == 1 ? 1u : std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    if (threads > 1) n = std::min<uint32_t>(n, static_cast<uint32_t>(threads));
    n = std::min<uint32_t>(n, std::max(1u, bw * bh / 4096));
    if (n <= 1) {
        encode_rows(w, h, rgba, level, dst, 0, bh);
        return true;
    }
    std::vector<std::thread> pool;
    pool.reserve(n - 1);
    const uint32_t step = (bh + n - 1) / n;
    for (uint32_t i = 1; i < n; ++i) {
        const uint32_t a = std::min(bh, i * step), b = std::min(bh, a + step);
        if (a < b) pool.emplace_back(encode_rows, w, h, rgba, level, dst, a, b);
    }
    encode_rows(w, h, rgba, level, dst, 0, std::min(bh, step));
    for (std::thread& t : pool) t.join();
    return true;
}
#else
bool bc1_encoder_available() { return false; }
bool encode_bc1_image(uint32_t, uint32_t, const uint8_t*, int, uint8_t*, size_t, int) { return false; }
#endif

// One BC1 block of a single colour: both endpoints the colour in RGB565, every index 0 (4-colour mode needs c0 > c1, which equal endpoints
// are not; equal endpoints decode as 3-colour mode, whose index 0 is still c0).
void bc1_solid_block(const uint8_t* rgba, uint8_t* out) {
    const uint16_t c = static_cast<uint16_t>(((rgba[0] * 31 + 127) / 255) << 11 | ((rgba[1] * 63 + 127) / 255) << 5 | ((rgba[2] * 31 + 127) / 255));
    out[0] = out[2] = static_cast<uint8_t>(c);
    out[1] = out[3] = static_cast<uint8_t>(c >> 8);
    out[4] = out[5] = out[6] = out[7] = 0;
}

}  // namespace nxrt::nvn::be
