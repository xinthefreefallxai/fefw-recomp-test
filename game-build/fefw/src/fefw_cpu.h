// SPDX-License-Identifier: GPL-3.0-or-later
// The CPU features the game needs, checked before any game code runs (fefw_main.cpp check_cpu_or_exit, first thing).
//
// What a build requires is the lifted game code's: fefw_rt.dll itself is baseline x86-64 (SSE2); its AVX2 / FMA3 code (the BC7
// encoder, the UCRT's math, dav1d's assembly) is picked by CPUID at run time. The lifted code is compiled in one of two variants
// (game-build/release/fefw_build.py CPU_VARIANTS), and ObeliskChamber.exe tells the DLL which (fefw_rt_api.h FefwRtModules.cpu_needs):
//   fma     -mcpu=x86_64+cx16+fma: the guest's FMADD / FMLA family is a single vfmadd. Needs FMA3, AVX (FMA implies it in LLVM: VEX
//           encodings anywhere; the OS must have enabled the YMM state, OSXSAVE and XGETBV), SSE4.2 (implied by AVX) and CMPXCHG16B.
//           Intel Haswell (2013) / AMD Piledriver (2012) and newer. Without FMA3 the first one is an illegal instruction (issues 33, 5).
//   compat  -mcpu=x86_64+cx16+sse4_2+popcnt: no FMA3 (fma / fmaf are correctly rounded library calls, sdk/fefw_fma.c). Needs
//           SSE4.2, POPCNT and CMPXCHG16B: Intel Nehalem (2008) / AMD Bulldozer (2011) and newer, so Sandy / Ivy Bridge and Xeon E5 v1 / v2.
// CMPXCHG16B (-mcx16) in both: the guest's 128-bit exclusive load/store pairs. Set up builds the variant the processor runs
// (launcher/src-tauri/src/cpu.rs, the same rule), so the check fails only when the processor changed or runs neither.
//
// FEFW_CPU_FORCE_MISSING=fma,avx,sse42,popcnt,cx16 (comma list; a test hook) makes the check behave as if those features were absent.
#pragma once

#include <intrin.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "fefw_rt_api.h"   // FEFW_CPU_*

namespace fefw_cpu {

struct Report {
    uint32_t have = 0;                              // FEFW_CPU_* present (AVX and FMA only when the OS saves the YMM state)
    bool forced = false;                            // a feature was hidden by FEFW_CPU_FORCE_MISSING
    char brand[64] = {};
    uint32_t missing(uint32_t needs) const { return needs & ~have; }
};

inline Report probe() {
    Report r;
    int b[4] = {};
    __cpuid(b, 0x80000000);
    if (static_cast<unsigned>(b[0]) >= 0x80000004u) {
        int* p = reinterpret_cast<int*>(r.brand);
        for (unsigned i = 0; i < 3; i++) __cpuid(p + 4 * i, static_cast<int>(0x80000002u + i));
        r.brand[48] = 0;
    }
    __cpuid(b, 0);
    const int max_leaf = b[0];
    if (max_leaf >= 1) {
        __cpuid(b, 1);
        const unsigned ecx = static_cast<unsigned>(b[2]);
        const bool sse3 = ecx & 1, ssse3 = (ecx >> 9) & 1, sse41 = (ecx >> 19) & 1, sse42 = (ecx >> 20) & 1;
        if ((ecx >> 13) & 1) r.have |= FEFW_CPU_CX16;
        if (sse3 && ssse3 && sse41 && sse42) r.have |= FEFW_CPU_SSE42;
        if ((ecx >> 23) & 1) r.have |= FEFW_CPU_POPCNT;
        const bool osxsave = (ecx >> 27) & 1;
        bool ymm = false;
        if (osxsave) {
            unsigned lo = 0, hi = 0;
            __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
            ymm = (lo & 6) == 6;                     // XMM and YMM state enabled by the OS
        }
        const bool avx = ((ecx >> 28) & 1) && ymm;
        if (avx) r.have |= FEFW_CPU_AVX;
        if (((ecx >> 12) & 1) && avx) r.have |= FEFW_CPU_FMA;   // VEX-encoded FMA needs the YMM state as well
    }
    if (const char* f = std::getenv("FEFW_CPU_FORCE_MISSING"); f && *f) {
        const std::string s = std::string(",") + f + ",";
        const struct { const char* name; uint32_t bit; } names[] = {{",fma,", FEFW_CPU_FMA}, {",avx,", FEFW_CPU_AVX | FEFW_CPU_FMA},
            {",sse42,", FEFW_CPU_SSE42}, {",sse4.2,", FEFW_CPU_SSE42}, {",popcnt,", FEFW_CPU_POPCNT}, {",cx16,", FEFW_CPU_CX16}};
        for (const auto& n : names)
            if (s.find(n.name) != std::string::npos) r.have &= ~n.bit, r.forced = true;
    }
    return r;
}

inline std::string feature_names(uint32_t bits) {
    std::string s;
    auto add = [&](uint32_t bit, const char* n) {
        if (!(bits & bit)) return;
        s += s.empty() ? "" : ", ";
        s += n;
    };
    add(FEFW_CPU_FMA, "FMA3 (fused multiply-add)");
    add(FEFW_CPU_AVX, "AVX");
    add(FEFW_CPU_SSE42, "SSE4.2");
    add(FEFW_CPU_POPCNT, "POPCNT");
    add(FEFW_CPU_CX16, "CMPXCHG16B");
    return s;
}

inline const char* build_name(uint32_t needs) {
    return needs == FEFW_CPU_NEEDS_COMPAT_BUILD ? "compatible (no FMA3)" : needs == FEFW_CPU_NEEDS_FMA_BUILD ? "FMA3" : "unknown";
}

// The plain-language message when this build cannot run on the processor ("" when it can). `needs` is the build's FEFW_CPU_* set.
inline std::string message(const Report& r, uint32_t needs) {
    const uint32_t missing = r.missing(needs);
    if (!missing) return {};
    std::string m = "This PC's processor";
    if (r.brand[0]) {
        std::string b = r.brand;
        b.erase(0, b.find_first_not_of(' '));
        b.erase(b.find_last_not_of(' ') + 1);
        m += " (" + b + ")";
    }
    if (!r.missing(FEFW_CPU_NEEDS_COMPAT_BUILD)) {
        // a build for another processor (the game folder moved to this PC, or the processor changed): Set up builds the right one
        m += " does not support " + feature_names(missing) + ", which this build of ObeliskChamber.exe was made for.\n\n"
             "Run Set up again in the launcher: it builds the compatible version of the game for this processor.";
    } else {
        m += " does not support " + feature_names(r.missing(FEFW_CPU_NEEDS_COMPAT_BUILD)) + ", which the game needs.\n\n"
             "Fortune's Weave needs a processor with CMPXCHG16B, SSE4.2 and POPCNT: an Intel Core i3 / i5 / i7 (2008) or newer, or an "
             "AMD FX (2011) / Ryzen or newer. Older processors (Core 2, AMD Phenom and Athlon II) cannot run it.";
    }
    if (r.forced) m += "\n\n(FEFW_CPU_FORCE_MISSING is set: the missing feature is simulated.)";
    return m;
}

}  // namespace fefw_cpu
