// astc2bc7 library core. GPL-3.0-or-later.
// Uses: ARM astcenc 5.7.0 (Apache-2.0) through per-ISA DLLs, bc7e.ispc from bc7enc_rdo (Apache-2.0, Binomial LLC).
// Build switches: A2B_BC7E_CPP uses bc7e translated to C++ (src/bc7e_cpp.cpp) instead of the ISPC object; A2B_ASTC_STATIC calls an
// astcenc decoder linked into the program instead of loading the DLLs (cmake/a2b_cpp.cmake sets both).
#include "astc2bc7.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "astcenc.h"
#ifdef A2B_BC7E_CPP
#include "bc7e_cpp.h"
namespace ispc = bc7e_cpp;
#else
#include "bc7e_ispc.h"
#endif

namespace {

// ---------------------------------------------------------------- astcenc DLL dispatch
struct AstcApi {
    HMODULE module = nullptr;
    const char* isa = nullptr;
    decltype(&astcenc_config_init) config_init = nullptr;
    decltype(&astcenc_context_alloc) context_alloc = nullptr;
    decltype(&astcenc_decompress_image) decompress = nullptr;
    decltype(&astcenc_context_free) context_free = nullptr;
    decltype(&astcenc_get_error_string) error_string = nullptr;
};

AstcApi g_astc;
std::mutex g_astc_mutex;
std::once_flag g_bc7e_once;

#ifdef A2B_ASTC_STATIC
#ifndef A2B_ASTC_STATIC_ISA
#define A2B_ASTC_STATIC_ISA "static"
#endif
a2b_status ensure_astc(const char*) {
    std::lock_guard<std::mutex> lock(g_astc_mutex);
    if (!g_astc.decompress) {
        g_astc.isa = A2B_ASTC_STATIC_ISA;
        g_astc.config_init = &astcenc_config_init;
        g_astc.context_alloc = &astcenc_context_alloc;
        g_astc.decompress = &astcenc_decompress_image;
        g_astc.context_free = &astcenc_context_free;
        g_astc.error_string = &astcenc_get_error_string;
    }
    return A2B_OK;
}
#else
bool cpu_has_avx2() {
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    const bool osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1, f16c = (r[2] >> 29) & 1, fma = (r[2] >> 12) & 1;
    const bool popcnt = (r[2] >> 23) & 1;
    if (!(osxsave && avx && f16c && fma && popcnt)) return false;
    if ((_xgetbv(0) & 6) != 6) return false;  // OS saves YMM state
    __cpuidex(r, 7, 0);
    return (r[1] >> 5) & 1;                   // AVX2
}

bool cpu_has_sse41() {
    int r[4];
    __cpuid(r, 1);
    return ((r[2] >> 19) & 1) && ((r[2] >> 23) & 1);  // SSE4.1 + POPCNT
}

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    std::wstring s(buf, n);
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"." : s.substr(0, p);
}

std::wstring widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

bool load_isa(const std::wstring& dir, const char* isa, AstcApi& api) {
    std::wstring path = dir + L"\\astcdec-" + widen(isa) + L"-shared.dll";
    HMODULE m = LoadLibraryW(path.c_str());
    if (!m) return false;
    api.module = m;
    api.isa = isa;
    api.config_init = (decltype(api.config_init))GetProcAddress(m, "astcenc_config_init");
    api.context_alloc = (decltype(api.context_alloc))GetProcAddress(m, "astcenc_context_alloc");
    api.decompress = (decltype(api.decompress))GetProcAddress(m, "astcenc_decompress_image");
    api.context_free = (decltype(api.context_free))GetProcAddress(m, "astcenc_context_free");
    api.error_string = (decltype(api.error_string))GetProcAddress(m, "astcenc_get_error_string");
    if (api.config_init && api.context_alloc && api.decompress && api.context_free && api.error_string) return true;
    FreeLibrary(m);
    api = AstcApi{};
    return false;
}

a2b_status ensure_astc(const char* dll_dir) {
    std::lock_guard<std::mutex> lock(g_astc_mutex);
    if (g_astc.module) return A2B_OK;
    std::wstring dir = dll_dir ? widen(dll_dir) : exe_dir();
    std::vector<const char*> order;
    if (cpu_has_avx2()) order.push_back("avx2");
    if (cpu_has_sse41()) order.push_back("sse4.1");
    order.push_back("sse2");
    for (const char* isa : order)
        if (load_isa(dir, isa, g_astc)) return A2B_OK;
    return A2B_ERR_NO_ASTC_LIB;
}
#endif

// ---------------------------------------------------------------- helpers
template <class F>
void parallel_for(size_t count, int threads, F&& fn) {
    if (threads <= 0) threads = (int)std::max(1u, std::thread::hardware_concurrency());
    threads = (int)std::min<size_t>((size_t)threads, count);
    if (threads <= 1) {
        for (size_t i = 0; i < count; i++) fn(i, 0);
        return;
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; t++)
        pool.emplace_back([&, t] {
            for (size_t i; (i = next.fetch_add(1)) < count;) fn(i, t);
        });
    for (auto& th : pool) th.join();
}

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

uint64_t div_up(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

}  // namespace

extern "C" {

a2b_status a2b_init(const char* dll_dir) { return ensure_astc(dll_dir); }

a2b_encoder a2b_bc7_encoder(const char** name) {
#ifdef A2B_BC7E_CPP
    if (name) *name = std::strcmp(bc7e_cpp::bc7e_cpp_variant(), "avx2") == 0 ? "bc7e C++ (AVX2)" : "bc7e C++ (x86-64)";
    return A2B_ENCODER_CPP;
#else
    if (name) *name = "bc7e ISPC";
    return A2B_ENCODER_ISPC;
#endif
}

const char* a2b_status_string(a2b_status s) {
    switch (s) {
        case A2B_OK: return "ok";
        case A2B_ERR_ARGS: return "bad arguments";
        case A2B_ERR_SIZE: return "buffer size does not match dimensions";
        case A2B_ERR_UNSUPPORTED: return "unsupported format";
        case A2B_ERR_NO_ASTC_LIB: return "astcdec-*-shared.dll not found";
        case A2B_ERR_DECODE: return "astcenc decode error";
    }
    return "?";
}

int a2b_is_astc(a2b_format f) { return f == A2B_ASTC_UNORM || f == A2B_ASTC_SRGB; }

int a2b_is_srgb(a2b_format f) {
    return f == A2B_ASTC_SRGB || f == A2B_BC1_SRGB || f == A2B_BC2_SRGB || f == A2B_BC3_SRGB || f == A2B_BC7_SRGB;
}

uint32_t a2b_bcn_block_bytes(a2b_format f) {
    switch (f) {
        case A2B_BC1_UNORM: case A2B_BC1_SRGB: case A2B_BC4_UNORM: case A2B_BC4_SNORM: return 8;
        case A2B_BC2_UNORM: case A2B_BC2_SRGB: case A2B_BC3_UNORM: case A2B_BC3_SRGB: case A2B_BC5_UNORM:
        case A2B_BC5_SNORM: case A2B_BC6H_UF16: case A2B_BC6H_SF16: case A2B_BC7_UNORM: case A2B_BC7_SRGB: return 16;
        default: return 0;
    }
}

uint64_t a2b_astc_size(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh) {
    if (!w || !h || bw < 4 || bw > 12 || bh < 4 || bh > 12) return 0;
    return div_up(w, bw) * div_up(h, bh) * 16;
}

uint64_t a2b_output_size(const a2b_input* in) {
    if (!in || !in->width || !in->height) return 0;
    if (a2b_is_astc(in->format)) return div_up(in->width, 4) * div_up(in->height, 4) * 16;
    uint32_t bb = a2b_bcn_block_bytes(in->format);
    return bb ? div_up(in->width, 4) * div_up(in->height, 4) * bb : 0;
}

// astcenc contexts are costly to make (block-size tables: tens of ms for some block sizes) and every decode used to make and free
// one per thread; kept here per (profile, block size) and handed out one user at a time.
namespace {
struct CtxPool {
    std::mutex m;
    std::map<uint32_t, std::vector<astcenc_context*>> free;
};
static CtxPool& ctx_pool() {
    static CtxPool* p = new CtxPool;   // never freed: contexts may still be in use at exit
    return *p;
}
uint32_t ctx_key(astcenc_profile prof, uint32_t bw, uint32_t bh) { return ((uint32_t)prof << 16) | (bw << 8) | bh; }
astcenc_context* ctx_take(uint32_t key, const astcenc_config& cfg) {
    {
        CtxPool& p = ctx_pool();
        std::lock_guard<std::mutex> lk(p.m);
        auto& v = p.free[key];
        if (!v.empty()) {
            astcenc_context* c = v.back();
            v.pop_back();
            return c;
        }
    }
    astcenc_context* c = nullptr;
    if (g_astc.context_alloc(&cfg, 1, &c, nullptr) != ASTCENC_SUCCESS) return nullptr;
    return c;
}
void ctx_give(uint32_t key, astcenc_context* c) {
    if (!c) return;
    CtxPool& p = ctx_pool();
    std::lock_guard<std::mutex> lk(p.m);
    p.free[key].push_back(c);
}
}  // namespace

a2b_status a2b_decode_astc_rgba8(const a2b_input* in, const void* data, size_t data_size, int threads, uint8_t* rgba,
                                 a2b_stats* stats) {
    if (!in || !data || !rgba || !a2b_is_astc(in->format)) return A2B_ERR_ARGS;
    const uint64_t expect = a2b_astc_size(in->width, in->height, in->block_w, in->block_h);
    if (!expect) return A2B_ERR_ARGS;
    if (data_size != expect) return A2B_ERR_SIZE;
    a2b_status st = ensure_astc(nullptr);
    if (st != A2B_OK) return st;

    const double t0 = now_s();
    const uint32_t w = in->width, h = in->height, bw = in->block_w, bh = in->block_h;
    const uint32_t blocks_x = (uint32_t)div_up(w, bw), blocks_y = (uint32_t)div_up(h, bh);
    // A band is a run of whole block rows: their blocks are contiguous in the stream and their texels contiguous in
    // the image, so each band decodes independently with its own single-threaded astcenc context.
    const uint32_t rows_per_band = std::max(1u, 64u / bh);
    const size_t bands = (size_t)div_up(blocks_y, rows_per_band);

    astcenc_config cfg;
    const astcenc_profile prof = in->format == A2B_ASTC_SRGB ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;
    if (g_astc.config_init(prof, bw, bh, 1, ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY, &cfg) != ASTCENC_SUCCESS)
        return A2B_ERR_ARGS;

    if (threads <= 0) threads = (int)std::max(1u, std::thread::hardware_concurrency());
    const int nthreads = (int)std::min<size_t>((size_t)threads, bands);
    const uint32_t key = ctx_key(prof, bw, bh);
    std::vector<astcenc_context*> ctx(nthreads, nullptr);
    std::atomic<int> failed{0};
    const astcenc_swizzle swz{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};

    parallel_for(bands, nthreads, [&](size_t band, int t) {
        if (failed.load()) return;
        if (!ctx[t] && !(ctx[t] = ctx_take(key, cfg))) { failed = 1; return; }
        const uint32_t by0 = (uint32_t)band * rows_per_band, by1 = std::min(blocks_y, by0 + rows_per_band);
        const uint32_t y0 = by0 * bh, y1 = std::min(h, by1 * bh);
        uint8_t* slice = rgba + (size_t)y0 * w * 4;
        void* slices[1] = {slice};
        astcenc_image img{w, y1 - y0, 1, ASTCENC_TYPE_U8, slices};
        const uint8_t* src = (const uint8_t*)data + (size_t)by0 * blocks_x * 16;
        const size_t len = (size_t)(by1 - by0) * blocks_x * 16;
        if (g_astc.decompress(ctx[t], src, len, &img, &swz, 0) != ASTCENC_SUCCESS) failed = 1;
    });
    for (auto* c : ctx) ctx_give(key, c);   // a context is reusable after any decompress (decompress_reset is only for multi-thread use of one context)
    if (stats) {
        stats->decode_seconds = now_s() - t0;
        stats->astc_isa = g_astc.isa;
    }
    return failed ? A2B_ERR_DECODE : A2B_OK;
}

// Sets bits [pos, pos+len) of the 128-bit block (lo = bits 0..63, hi = bits 64..127), least significant bit first.
static void put_bits(uint64_t& lo, uint64_t& hi, int pos, int len, uint32_t v) {
    for (int i = 0; i < len; i++) {
        const int b = pos + i;
        uint64_t& w = b < 64 ? lo : hi;
        const uint64_t m = 1ull << (b & 63);
        if ((v >> i) & 1) w |= m; else w &= ~m;
    }
}

// Makes a BC7 block of an opaque source decode to alpha exactly 255 without touching any data channel.
//   modes 0..3 have no alpha (always 255). Mode 4 (rotation r, 5-bit colour, 6-bit alpha endpoints) and mode 5 (7-bit colour,
//   8-bit alpha endpoints): the original alpha lives in the alpha slot when r = 0, else in colour slot r (the data channel went
//   to the alpha slot); that slot's two endpoints are set to all ones, which decodes to 255 whatever the indices say. Mode 6:
//   A0 = A1 = 127 and both p-bits 1. Mode 7 (alpha 5 bits + p-bit) does not occur for opaque blocks in bc7e; treated like mode 6
//   would be wrong, so it is left alone (the caller's tests check alpha).
static void pin_opaque_alpha(uint64_t& lo, uint64_t& hi) {
    if ((lo & 0x1f) == 0x10) {   // mode 4
        const int rot = (int)((lo >> 5) & 3);
        if (rot == 0) { put_bits(lo, hi, 38, 6, 63); put_bits(lo, hi, 44, 6, 63); }
        else { put_bits(lo, hi, 8 + (rot - 1) * 10, 5, 31); put_bits(lo, hi, 13 + (rot - 1) * 10, 5, 31); }
    } else if ((lo & 0x3f) == 0x20) {   // mode 5
        const int rot = (int)((lo >> 6) & 3);
        if (rot == 0) { put_bits(lo, hi, 50, 8, 255); put_bits(lo, hi, 58, 8, 255); }
        else { put_bits(lo, hi, 8 + (rot - 1) * 14, 7, 127); put_bits(lo, hi, 15 + (rot - 1) * 14, 7, 127); }
    } else if ((lo & 0x7f) == 0x40) {   // mode 6
        put_bits(lo, hi, 49, 14, (127u << 7) | 127u);
        put_bits(lo, hi, 63, 1, 1);
        put_bits(lo, hi, 64, 1, 1);
    }
}

a2b_status a2b_encode_bc7(uint32_t width, uint32_t height, const uint8_t* rgba, const a2b_options* opt, void* out,
                          size_t out_capacity, a2b_stats* stats) {
    if (!width || !height || !rgba || !out) return A2B_ERR_ARGS;
    const uint32_t bx = (uint32_t)div_up(width, 4), by = (uint32_t)div_up(height, 4);
    if (out_capacity < (size_t)bx * by * 16) return A2B_ERR_SIZE;
    std::call_once(g_bc7e_once, [] { ispc::bc7e_compress_block_init(); });

    int quality = opt && opt->quality >= 0 ? opt->quality : A2B_DEFAULT_QUALITY;
    const bool perceptual = opt && opt->perceptual;
    ispc::bc7e_compress_block_params params;
    std::memset(&params, 0, sizeof params);
    switch (std::min(quality, 6)) {
        case 0: ispc::bc7e_compress_block_params_init_ultrafast(&params, perceptual); break;
        case 1: ispc::bc7e_compress_block_params_init_veryfast(&params, perceptual); break;
        case 2: ispc::bc7e_compress_block_params_init_fast(&params, perceptual); break;
        case 3: ispc::bc7e_compress_block_params_init_basic(&params, perceptual); break;
        case 4: ispc::bc7e_compress_block_params_init_slow(&params, perceptual); break;
        case 5: ispc::bc7e_compress_block_params_init_veryslow(&params, perceptual); break;
        default: ispc::bc7e_compress_block_params_init_slowest(&params, perceptual); break;
    }

    // Opaque source blocks must keep alpha exactly 255. Left alone, bc7e (a) stores opaque mode-6 alpha in the shared p-bit
    // (decodes as 254 half the time) and (b) uses mode 4/5 channel rotation, which swaps a colour channel with alpha, and does
    // not pin the swapped-in original alpha (alpha 244..253 on normal maps). The first version of this library therefore turned
    // mode 6 and rotation off for opaque blocks, which costs 2 to 12 dB on two-channel (RG, B = 0) normal maps: rotation is what
    // gives the data channel in the alpha slot its own index set, i.e. a BC5-like encoding. Now opaque blocks are encoded with
    // mode 6 off (pinning it costs more RGB PSNR than the mode gains) and the preset's rotations, and pin_opaque_alpha then
    // fixes the alpha of the result exactly. Rotation on every opaque block costs about 45% more encode time at quality 3 than
    // the first version; limiting it to blocks with a flat channel saved only a fifth of that and lost up to 1.3 dB on
    // colour textures.
    // allow_opaque_mode6: 0 (default) that; 1 raw bc7e output (alpha may drift); 2 the first version (no mode 6, no rotation).
    const int m6 = opt ? opt->allow_opaque_mode6 : 0;
    const bool exact_opaque = m6 != 1;
    ispc::bc7e_compress_block_params opaque_params = params;
    if (m6 != 1) opaque_params.m_opaque_settings.m_use_mode[6] = false;
    if (m6 == 2) {
        opaque_params.m_mode4_rotation_mask = 1;
        opaque_params.m_mode5_rotation_mask = 1;
    }
    const bool pin_alpha = m6 == 0 || (m6 == 2 && params.m_mode6_only);   // preset 0 ignores m_use_mode: pin afterwards

    const double t0 = now_s();
    constexpr uint32_t kChunk = 64;  // blocks per bc7e call
    const uint32_t chunks_per_row = (bx + kChunk - 1) / kChunk;
    const size_t tasks = (size_t)by * chunks_per_row;
    const int threads = opt ? opt->threads : 0;
    parallel_for(tasks, threads, [&](size_t task, int) {
        const uint32_t row = (uint32_t)(task / chunks_per_row), c0 = (uint32_t)(task % chunks_per_row) * kChunk;
        const uint32_t n = std::min(kChunk, bx - c0);
        uint32_t pix[2][kChunk * 16];   // [0] opaque blocks, [1] blocks with alpha, compacted
        uint8_t slot[kChunk];           // which list, and implicitly the order within it
        uint32_t count[2] = {0, 0};
        const uint32_t* src = (const uint32_t*)rgba;
        for (uint32_t b = 0; b < n; b++) {
            uint32_t block[16];
            bool opaque = true;
            for (uint32_t py = 0; py < 4; py++) {
                const uint32_t y = std::min(row * 4 + py, height - 1);  // replicate edges into partial blocks
                for (uint32_t px = 0; px < 4; px++) {
                    const uint32_t x = std::min((c0 + b) * 4 + px, width - 1);
                    const uint32_t v = src[(size_t)y * width + x];
                    block[py * 4 + px] = v;
                    opaque &= (v >> 24) == 0xff;
                }
            }
            const int list = exact_opaque && opaque ? 0 : 1;
            slot[b] = (uint8_t)list;
            std::memcpy(&pix[list][count[list]++ * 16], block, sizeof block);
        }
        uint64_t res[2][kChunk * 2];
        if (count[0]) ispc::bc7e_compress_blocks(count[0], res[0], pix[0], &opaque_params);
        if (count[1]) ispc::bc7e_compress_blocks(count[1], res[1], pix[1], &params);
        uint32_t next[2] = {0, 0};
        uint64_t* dst = (uint64_t*)((uint8_t*)out + ((size_t)row * bx + c0) * 16);
        for (uint32_t b = 0; b < n; b++) {
            const int list = slot[b];
            uint64_t lo = res[list][next[list] * 2], hi = res[list][next[list] * 2 + 1];
            next[list]++;
            if (list == 0 && pin_alpha) pin_opaque_alpha(lo, hi);
            dst[b * 2] = lo;
            dst[b * 2 + 1] = hi;
        }
    });
    if (stats) stats->encode_seconds = now_s() - t0;
    return A2B_OK;
}

a2b_status a2b_transcode(const a2b_input* in, const void* data, size_t data_size, const a2b_options* opt,
                         a2b_format* out_format, void* out, size_t out_capacity, a2b_stats* stats) {
    if (!in || !data || !out) return A2B_ERR_ARGS;
    if (stats) *stats = a2b_stats{};
    const uint64_t need = a2b_output_size(in);
    if (!need) return a2b_is_astc(in->format) ? A2B_ERR_ARGS : A2B_ERR_UNSUPPORTED;
    if (out_capacity < need) return A2B_ERR_SIZE;

    if (!a2b_is_astc(in->format)) {  // BCn: untouched
        if (data_size != need) return A2B_ERR_SIZE;
        std::memcpy(out, data, (size_t)need);
        if (out_format) *out_format = in->format;
        return A2B_OK;
    }
    std::vector<uint8_t> rgba((size_t)in->width * in->height * 4);
    a2b_status st = a2b_decode_astc_rgba8(in, data, data_size, opt ? opt->threads : 0, rgba.data(), stats);
    if (st != A2B_OK) return st;
    st = a2b_encode_bc7(in->width, in->height, rgba.data(), opt, out, out_capacity, stats);
    if (st == A2B_OK && out_format) *out_format = in->format == A2B_ASTC_SRGB ? A2B_BC7_SRGB : A2B_BC7_UNORM;
    return st;
}

}  // extern "C"
