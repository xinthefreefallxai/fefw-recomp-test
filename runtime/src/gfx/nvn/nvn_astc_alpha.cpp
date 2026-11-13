// SPDX-License-Identifier: GPL-3.0-or-later
// ASTC alpha scan for [gfx] astc_bc = bc1 (nvn_texdata.h): can any texel of a raw ASTC storage decode to an alpha other than 255?
// Adapted from Astral Chain PC Recomp (runtime/tex_astc.c: astc_block_alpha_possible, astc_storage_alpha_possible; MIT, Copyright (c) 2026
// Valiantsin Dzerakh), whose ASTC decoder is ported from LAS Recomp (runtime/nvn/astc.c @ da35b83a, MIT, Copyright (c) 2026 Peiikai). Only the
// parts the scan needs are kept (block mode, colour endpoint modes, the integer sequence decode of the colour values and their unquantization);
// see THIRD_PARTY_NOTICES.md. Changes: C++, a void extent counts as opaque only when its alpha is exactly 0xFFFF.
//
// How the verdict is reached, per 128-bit block (Khronos Data Format spec, ASTC chapter; the decode is astcenc's 8-bit LDR decode):
//   * error blocks (reserved block modes, illegal encodings, HDR void extents) decode to magenta, alpha 255: opaque;
//   * a void extent is one constant: opaque when its UNORM16 alpha is 0xFFFF;
//   * the alpha-free LDR endpoint modes (luminance, RGB, RGB scale) put both endpoint alphas at 255 and the HDR modes decode their partition to
//     magenta: opaque without looking at the values;
//   * an alpha-carrying mode (LA, RGBA, RGB scale + two alphas) has its endpoint values decoded: a texel's alpha interpolates the two endpoint
//     alphas, so it can differ from 255 only when an endpoint alpha does.
// The Tegra block-linear swizzle moves whole 16-byte units (GOB rows are 64 bytes), so every ASTC block of the storage is one aligned 16-byte unit
// and the walk at 16-byte steps sees each of them; it also reads the padding between levels and layers as blocks, which can only answer "alpha".
#include <cstdint>
#include <cstring>

#include "nvn_texdata.h"

namespace nxrt::nvn::be {
namespace {

// Quantization levels QUANT_2 .. QUANT_256 (index 0..20): bits / trits / quints per level.
constexpr uint8_t kQBits[21] = {1, 0, 2, 0, 1, 3, 1, 2, 4, 2, 3, 5, 3, 4, 6, 4, 5, 7, 5, 6, 8};
constexpr uint8_t kQTrits[21] = {0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0};
constexpr uint8_t kQQuints[21] = {0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0};
constexpr int kQuant6 = 4, kQuant256 = 20;

// Colour unquantization, indexed by the raw ISE value (trit/quint << bits | bits): QUANT_6..QUANT_256 -> 0..255 (QUANT_256 is the identity).
constexpr uint8_t kCunq[] = {
    /* 6 */
    0,255,51,204,102,153,
    /* 8 */
    0,36,73,109,146,182,219,255,
    /* 10 */
    0,255,28,227,56,199,84,171,113,142,
    /* 12 */
    0,255,69,186,23,232,92,163,46,209,116,139,
    /* 16 */
    0,17,34,51,68,85,102,119,136,153,170,187,204,221,238,255,
    /* 20 */
    0,255,67,188,13,242,80,175,27,228,94,161,40,215,107,148,54,201,121,134,
    /* 24 */
    0,255,33,222,66,189,99,156,11,244,44,211,77,178,110,145,22,233,55,200,88,167,121,134,
    /* 32 */
    0,8,16,24,33,41,49,57,66,74,82,90,99,107,115,123,132,140,148,156,165,173,181,189,198,206,214,222,
    231,239,247,255,
    /* 40 */
    0,255,32,223,65,190,97,158,6,249,39,216,71,184,104,151,13,242,45,210,78,177,110,145,19,236,52,203,
    84,171,117,138,26,229,58,197,91,164,123,132,
    /* 48 */
    0,255,16,239,32,223,48,207,65,190,81,174,97,158,113,142,5,250,21,234,38,217,54,201,70,185,86,169,
    103,152,119,136,11,244,27,228,43,212,59,196,76,179,92,163,108,147,124,131,
    /* 64 */
    0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,65,69,73,77,81,85,89,93,97,101,105,109,113,117,121,125,
    130,134,138,142,146,150,154,158,162,166,170,174,178,182,186,190,195,199,203,207,211,215,219,223,227,
    231,235,239,243,247,251,255,
    /* 80 */
    0,255,16,239,32,223,48,207,64,191,80,175,96,159,112,143,3,252,19,236,35,220,51,204,67,188,83,172,
    100,155,116,139,6,249,22,233,38,217,54,201,71,184,87,168,103,152,119,136,9,246,25,230,42,213,58,197,
    74,181,90,165,106,149,122,133,13,242,29,226,45,210,61,194,77,178,93,162,109,146,125,130,
    /* 96 */
    0,255,8,247,16,239,24,231,32,223,40,215,48,207,56,199,64,191,72,183,80,175,88,167,96,159,104,151,
    112,143,120,135,2,253,10,245,18,237,26,229,35,220,43,212,51,204,59,196,67,188,75,180,83,172,91,164,
    99,156,107,148,115,140,123,132,5,250,13,242,21,234,29,226,37,218,45,210,53,202,61,194,70,185,78,177,
    86,169,94,161,102,153,110,145,118,137,126,129,
    /* 128 */
    0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,66,68,
    70,72,74,76,78,80,82,84,86,88,90,92,94,96,98,100,102,104,106,108,110,112,114,116,118,120,122,124,
    126,129,131,133,135,137,139,141,143,145,147,149,151,153,155,157,159,161,163,165,167,169,171,173,175,
    177,179,181,183,185,187,189,191,193,195,197,199,201,203,205,207,209,211,213,215,217,219,221,223,225,
    227,229,231,233,235,237,239,241,243,245,247,249,251,253,255,
    /* 160 */
    0,255,8,247,16,239,24,231,32,223,40,215,48,207,56,199,64,191,72,183,80,175,88,167,96,159,104,151,
    112,143,120,135,1,254,9,246,17,238,25,230,33,222,41,214,49,206,57,198,65,190,73,182,81,174,89,166,
    97,158,105,150,113,142,121,134,3,252,11,244,19,236,27,228,35,220,43,212,51,204,59,196,67,188,75,180,
    83,172,91,164,99,156,107,148,115,140,123,132,4,251,12,243,20,235,28,227,36,219,44,211,52,203,60,195,
    68,187,76,179,84,171,92,163,100,155,108,147,116,139,124,131,6,249,14,241,22,233,30,225,38,217,46,
    209,54,201,62,193,70,185,78,177,86,169,94,161,102,153,110,145,118,137,126,129,
    /* 192 */
    0,255,4,251,8,247,12,243,16,239,20,235,24,231,28,227,32,223,36,219,40,215,44,211,48,207,52,203,56,
    199,60,195,64,191,68,187,72,183,76,179,80,175,84,171,88,167,92,163,96,159,100,155,104,151,108,147,
    112,143,116,139,120,135,124,131,1,254,5,250,9,246,13,242,17,238,21,234,25,230,29,226,33,222,37,218,
    41,214,45,210,49,206,53,202,57,198,61,194,65,190,69,186,73,182,77,178,81,174,85,170,89,166,93,162,
    97,158,101,154,105,150,109,146,113,142,117,138,121,134,125,130,2,253,6,249,10,245,14,241,18,237,22,
    233,26,229,30,225,34,221,38,217,42,213,46,209,50,205,54,201,58,197,62,193,66,189,70,185,74,181,78,
    177,82,173,86,169,90,165,94,161,98,157,102,153,106,149,110,145,114,141,118,137,122,133,126,129,
};
constexpr uint16_t kCunqOff[16] = {0, 6, 14, 24, 36, 52, 72, 96, 128, 168, 216, 280, 360, 456, 584, 744};   // QUANT_256: identity
static_assert(sizeof kCunq == 936, "colour unquantization table size");

struct Bits128 {
    uint64_t lo, hi;
};

inline uint64_t load64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);   // little-endian host
    return v;
}

// n (<= 32) bits at pos; bits past 127 read as zero.
inline uint32_t rbits(const Bits128& b, int pos, int n) {
    if (n == 0 || pos >= 128 || pos < 0) return 0;
    uint64_t v;
    if (pos >= 64) v = b.hi >> (pos - 64);
    else if (pos == 0) v = b.lo;
    else v = b.lo >> pos | b.hi << (64 - pos);
    return static_cast<uint32_t>(v & ((1ull << n) - 1));
}

int ise_bitcount(int count, int q) {
    int n = count * kQBits[q];
    if (kQTrits[q]) n += (8 * count + 4) / 5;
    if (kQQuints[q]) n += (7 * count + 2) / 3;
    return n;
}

void trits_of(uint32_t T, int t[5]) {
    uint32_t C;
    if ((T >> 2 & 7) == 7) {
        C = (T >> 5 & 7) << 2 | (T & 3);
        t[4] = t[3] = 2;
    } else {
        C = T & 0x1F;
        if ((T >> 5 & 3) == 3) { t[4] = 2; t[3] = T >> 7 & 1; }
        else { t[4] = T >> 7 & 1; t[3] = T >> 5 & 3; }
    }
    if ((C & 3) == 3) {
        t[2] = 2; t[1] = C >> 4 & 1;
        t[0] = static_cast<int>((C >> 3 & 1) << 1 | ((C >> 2 & 1) & ~(C >> 3) & 1));
    } else if ((C >> 2 & 3) == 3) {
        t[2] = 2; t[1] = 2; t[0] = C & 3;
    } else {
        t[2] = C >> 4 & 1; t[1] = C >> 2 & 3;
        t[0] = static_cast<int>((C >> 1 & 1) << 1 | ((C & 1) & ~(C >> 1) & 1));
    }
}

void quints_of(uint32_t Q, int q[3]) {
    if ((Q >> 1 & 3) == 3 && (Q >> 5 & 3) == 0) {
        const uint32_t q0 = Q & 1;
        q[2] = static_cast<int>(q0 << 2 | ((Q >> 4 & 1) & ~q0 & 1) << 1 | ((Q >> 3 & 1) & ~q0 & 1));
        q[1] = q[0] = 4;
        return;
    }
    uint32_t C;
    if ((Q >> 1 & 3) == 3) { q[2] = 4; C = (Q >> 3 & 3) << 3 | (~(Q >> 5) & 3) << 1 | (Q & 1); }
    else { q[2] = Q >> 5 & 3; C = Q & 0x1F; }
    if ((C & 7) == 5) { q[1] = 4; q[0] = C >> 3 & 3; }
    else { q[1] = C >> 3 & 3; q[0] = C & 7; }
}

// Decodes count raw ISE values (tq << bits | bits) starting at bit pos.
void ise_decode(const Bits128& b, int pos, int count, int q, uint8_t* out) {
    const int nb = kQBits[q];
    if (kQTrits[q]) {
        static constexpr uint8_t tb[5] = {2, 2, 1, 2, 1}, ts[5] = {0, 2, 4, 5, 7};
        for (int i = 0; i < count; i += 5) {
            uint32_t m[5] = {}, T = 0;
            int k = 0, t[5];
            for (; k < 5 && i + k < count; k++) {
                m[k] = rbits(b, pos, nb); pos += nb;
                T |= rbits(b, pos, tb[k]) << ts[k]; pos += tb[k];
            }
            trits_of(T, t);
            for (int j = 0; j < k; j++) out[i + j] = static_cast<uint8_t>(static_cast<uint32_t>(t[j]) << nb | m[j]);
        }
    } else if (kQQuints[q]) {
        static constexpr uint8_t qb[3] = {3, 2, 2}, qs[3] = {0, 3, 5};
        for (int i = 0; i < count; i += 3) {
            uint32_t m[3] = {}, Q = 0;
            int k = 0, v[3];
            for (; k < 3 && i + k < count; k++) {
                m[k] = rbits(b, pos, nb); pos += nb;
                Q |= rbits(b, pos, qb[k]) << qs[k]; pos += qb[k];
            }
            quints_of(Q, v);
            for (int j = 0; j < k; j++) out[i + j] = static_cast<uint8_t>(static_cast<uint32_t>(v[j]) << nb | m[j]);
        }
    } else {
        for (int i = 0; i < count; i++, pos += nb) out[i] = static_cast<uint8_t>(rbits(b, pos, nb));
    }
}

struct BlockMode {
    int wx, wy, dual, quant, wbits;
};

bool block_mode_2d(uint32_t m, uint32_t bw, uint32_t bh, BlockMode* bm) {
    uint32_t r = m >> 4 & 1, h = m >> 9 & 1, d = m >> 10 & 1;
    const uint32_t a = m >> 5 & 3;
    int wx, wy;
    if (m & 3) {
        r |= (m & 3) << 1;
        uint32_t b = m >> 7 & 3;
        switch (m >> 2 & 3) {
            case 0: wx = static_cast<int>(b + 4); wy = static_cast<int>(a + 2); break;
            case 1: wx = static_cast<int>(b + 8); wy = static_cast<int>(a + 2); break;
            case 2: wx = static_cast<int>(a + 2); wy = static_cast<int>(b + 8); break;
            default:
                b &= 1;
                if (m & 0x100) { wx = static_cast<int>(b + 2); wy = static_cast<int>(a + 2); }
                else { wx = static_cast<int>(a + 2); wy = static_cast<int>(b + 6); }
                break;
        }
    } else {
        r |= (m >> 2 & 3) << 1;
        if ((m >> 2 & 3) == 0) return false;
        const uint32_t b = m >> 9 & 3;
        switch (m >> 7 & 3) {
            case 0: wx = 12; wy = static_cast<int>(a + 2); break;
            case 1: wx = static_cast<int>(a + 2); wy = 12; break;
            case 2: wx = static_cast<int>(a + 6); wy = static_cast<int>(b + 6); d = 0; h = 0; break;
            default:
                if (a == 0) { wx = 6; wy = 10; }
                else if (a == 1) { wx = 10; wy = 6; }
                else return false;
                break;
        }
    }
    const int count = wx * wy * static_cast<int>(d + 1);
    bm->wx = wx;
    bm->wy = wy;
    bm->dual = static_cast<int>(d);
    bm->quant = static_cast<int>(r - 2 + 6 * h);
    bm->wbits = ise_bitcount(count, bm->quant);
    return count <= 64 && bm->wbits >= 24 && bm->wbits <= 96 && wx <= static_cast<int>(bw) && wy <= static_cast<int>(bh);
}

inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
inline bool alpha_cem(int cem) { return cem == 4 || cem == 5 || cem == 10 || cem == 12 || cem == 13; }

// The two endpoint alphas of an alpha-carrying endpoint mode (4, 5, 10, 12, 13) from its unquantized values. The blue-contraction swap of
// modes 12 and 13 exchanges the endpoints, which leaves the pair of alphas as it is.
void endpoint_alphas(int cem, const uint8_t* v, int& a0, int& a1) {
    switch (cem) {
        case 4:   // LA direct
            a0 = v[2]; a1 = v[3];
            return;
        case 5: {   // LA base + offset
            int base = v[2], off = v[3];
            base |= (off & 0x80) << 1;
            off &= 0x7F;
            if (off & 0x40) off -= 0x80;
            base >>= 1;
            off >>= 1;   // arithmetic shift of the signed offset
            a0 = base;
            a1 = clamp255(off + base);
            return;
        }
        case 10:   // RGB scale + two alphas
            a0 = v[4]; a1 = v[5];
            return;
        case 12:   // RGBA direct
            a0 = v[6]; a1 = v[7];
            return;
        default: {   // 13: RGBA base + offset
            const int base = v[6] >> 1 | (v[7] & 0x80);
            int off = v[7] >> 1 & 0x3F;
            if (off & 0x20) off -= 0x40;
            a0 = clamp255(base);
            a1 = clamp255(base + off);
            return;
        }
    }
}

bool block_alpha_possible(const uint8_t block[16], uint32_t bw, uint32_t bh) {
    const Bits128 b = {load64(block), load64(block + 8)};
    const uint32_t mode = static_cast<uint32_t>(b.lo & 0x7FF);

    if ((mode & 0x1FF) == 0x1FC) {   // void extent
        if (mode & 0x200) return false;           // HDR constant: an error block in LDR (magenta)
        if ((b.lo >> 10 & 3) != 3) return false;  // reserved bits: error
        const uint32_t s0 = rbits(b, 12, 13), s1 = rbits(b, 25, 13), t0 = rbits(b, 38, 13), t1 = rbits(b, 51, 13);
        const bool all_ones = s0 == 0x1FFF && s1 == 0x1FFF && t0 == 0x1FFF && t1 == 0x1FFF;
        if ((s0 >= s1 || t0 >= t1) && !all_ones) return false;   // error
        return !(block[14] == 0xFF && block[15] == 0xFF);
    }

    BlockMode bm;
    if (!block_mode_2d(mode, bw, bh, &bm)) return false;   // error block
    const int pcount = static_cast<int>(b.lo >> 11 & 3) + 1;
    if (bm.dual && pcount == 4) return false;

    int cem[4] = {};
    int high = 0;
    if (pcount == 1) {
        cem[0] = static_cast<int>(b.lo >> 13 & 0xF);
    } else {
        high = 3 * pcount - 4;
        const int below = 128 - bm.wbits - high;
        const uint32_t enc = static_cast<uint32_t>(b.lo >> 23 & 0x3F) | rbits(b, below, high) << 6;
        uint32_t base = enc & 3;
        if (base == 0) {
            for (int i = 0; i < pcount; i++) cem[i] = static_cast<int>(enc >> 2 & 0xF);
            high = 0;   // every CEM bit was in the low field
        } else {
            base--;
            for (int i = 0; i < pcount; i++) cem[i] = static_cast<int>(((enc >> (2 + i) & 1) + base) << 2);
            for (int i = 0; i < pcount; i++) cem[i] |= static_cast<int>(enc >> (2 + pcount + 2 * i) & 3);
        }
    }
    int ccount = 0;
    for (int i = 0; i < pcount; i++) ccount += ((cem[i] >> 2) + 1) * 2;
    if (ccount > 18) return false;   // error
    int cbits = (pcount == 1 ? 111 : 99) - bm.wbits - high - (bm.dual ? 2 : 0);
    if (cbits < 0) cbits = 0;
    int cq = -1;
    for (int q = 20; q >= 0; q--)
        if (ise_bitcount(ccount, q) <= cbits) { cq = q; break; }
    if (cq < kQuant6) return false;   // error

    bool carries = false;
    for (int i = 0; i < pcount; i++) carries = carries || alpha_cem(cem[i]);
    if (!carries) return false;

    uint8_t cv[18];
    ise_decode(b, pcount == 1 ? 17 : 29, ccount, cq, cv);
    if (cq != kQuant256) {
        const uint8_t* ct = kCunq + kCunqOff[cq - kQuant6];
        for (int i = 0; i < ccount; i++) cv[i] = ct[cv[i]];
    }
    for (int p = 0, k = 0; p < pcount; p++) {
        if (alpha_cem(cem[p])) {
            int a0 = 255, a1 = 255;
            endpoint_alphas(cem[p], cv + k, a0, a1);
            if (a0 != 255 || a1 != 255) return true;
        }
        k += ((cem[p] >> 2) + 1) * 2;
    }
    return false;
}

}  // namespace

bool astc_storage_alpha_possible(const uint8_t* mem, uint64_t bytes, uint32_t bw, uint32_t bh) {
    if (!mem || bw < 4 || bh < 4 || bw > 12 || bh > 12) return true;   // not a 2D footprint this scan can judge
    for (uint64_t off = 0; off + 16 <= bytes; off += 16)
        if (block_alpha_possible(mem + off, bw, bh)) return true;
    return false;
}

}  // namespace nxrt::nvn::be
