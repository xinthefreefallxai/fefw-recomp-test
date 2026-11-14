// SPDX-License-Identifier: GPL-3.0-or-later
//! The CPU features the compiled game needs, and which build of the game code Set up makes for this processor. fefw_build.py
//! (game-build/release/fefw_build.py CPU_VARIANTS) compiles the lifted game code in one of two variants:
//!   fma     `-mcpu=x86_64+cx16+fma`: FMA3 (and AVX, which FMA implies in LLVM), SSE4.2, CMPXCHG16B. Intel Haswell (2013) / AMD
//!           Piledriver (2012) and newer. The faster one.
//!   compat  `-mcpu=x86_64+cx16+sse4_2+popcnt`: no FMA3 (fused multiply-adds are exact library calls). SSE4.2, POPCNT, CMPXCHG16B:
//!           Intel Nehalem (2008) / AMD Bulldozer (2011) and newer, so Sandy / Ivy Bridge and Xeon E5 v1 / v2 (issue 33).
//! Set up passes the variant to fefw_build.py (`--cpu`), which keeps it in the build stamp: an ObeliskChamber.exe built for the other one is
//! rebuilt. ObeliskChamber.exe checks the same features before the game starts (game-build/fefw/src/fefw_cpu.h). A processor that runs neither
//! is told so before Set up spends an hour on a build that cannot run.
//! FEFW_CPU_FORCE_MISSING=fma,avx,sse42,popcnt,cx16 simulates missing features (tests); FEFW_BUILD_CPU=fma|compat overrides the choice.

#[derive(Debug, Clone, PartialEq)]
pub struct Cpu {
    pub brand: String,
    pub fma: bool,
    pub avx: bool,
    pub cx16: bool,
    /// SSE3, SSSE3, SSE4.1 and SSE4.2
    pub sse42: bool,
    pub popcnt: bool,
}

#[cfg(target_arch = "x86_64")]
#[allow(unused_unsafe)]
fn brand() -> String {
    use std::arch::x86_64::__cpuid;
    // SAFETY: cpuid is available on every x86-64 CPU (newer Rust marks __cpuid safe: the block is then redundant)
    unsafe {
        if __cpuid(0x8000_0000).eax < 0x8000_0004 {
            return String::new();
        }
        let mut bytes = Vec::with_capacity(48);
        for leaf in 0x8000_0002u32..=0x8000_0004 {
            let r = __cpuid(leaf);
            for v in [r.eax, r.ebx, r.ecx, r.edx] {
                bytes.extend_from_slice(&v.to_le_bytes());
            }
        }
        String::from_utf8_lossy(&bytes).trim_matches(|c: char| c == '\0' || c.is_whitespace()).to_string()
    }
}

#[cfg(target_arch = "x86_64")]
pub fn probe() -> Cpu {
    // is_x86_feature_detected also checks that the OS enabled the YMM state (XGETBV) for avx and fma
    let mut c = Cpu {
        brand: brand(),
        fma: std::is_x86_feature_detected!("fma"),
        avx: std::is_x86_feature_detected!("avx"),
        cx16: std::is_x86_feature_detected!("cmpxchg16b"),
        sse42: std::is_x86_feature_detected!("sse3")
            && std::is_x86_feature_detected!("ssse3")
            && std::is_x86_feature_detected!("sse4.1")
            && std::is_x86_feature_detected!("sse4.2"),
        popcnt: std::is_x86_feature_detected!("popcnt"),
    };
    force(&mut c, std::env::var("FEFW_CPU_FORCE_MISSING").ok().as_deref());
    c
}

#[cfg(not(target_arch = "x86_64"))]
pub fn probe() -> Cpu {
    Cpu { brand: String::new(), fma: true, avx: true, cx16: true, sse42: true, popcnt: true }
}

fn force(c: &mut Cpu, list: Option<&str>) {
    for item in list.unwrap_or("").split(',').map(|s| s.trim().to_ascii_lowercase()) {
        match item.as_str() {
            "fma" => c.fma = false,
            "avx" => {
                c.avx = false;
                c.fma = false;
            }
            "cx16" => c.cx16 = false,
            "sse42" | "sse4.2" => c.sse42 = false,
            "popcnt" => c.popcnt = false,
            _ => {}
        }
    }
}

/// The build of the game code this processor runs (fefw_build.py `--cpu`): "fma" when it can, else "compat", None for neither.
pub fn variant_for(c: &Cpu) -> Option<&'static str> {
    if c.fma && c.avx && c.sse42 && c.cx16 {
        Some("fma")
    } else if c.sse42 && c.popcnt && c.cx16 {
        Some("compat")
    } else {
        None
    }
}

/// The build Set up makes on this PC: FEFW_BUILD_CPU when set to a variant, else the one the processor runs.
pub fn build_variant() -> Option<&'static str> {
    match std::env::var("FEFW_BUILD_CPU").unwrap_or_default().trim().to_ascii_lowercase().as_str() {
        "fma" => Some("fma"),
        "compat" => Some("compat"),
        _ => variant_for(&probe()),
    }
}

/// The plain-language problem, or None when the processor runs one of the builds.
pub fn problem(c: &Cpu) -> Option<String> {
    if variant_for(c).is_some() {
        return None;
    }
    let mut missing = vec![];
    if !c.sse42 {
        missing.push("SSE4.2");
    }
    if !c.popcnt {
        missing.push("POPCNT");
    }
    if !c.cx16 {
        missing.push("CMPXCHG16B");
    }
    let who = if c.brand.is_empty() { "This PC's processor".to_string() } else { format!("This PC's processor ({})", c.brand) };
    Some(format!(
        "{who} does not support {}, which the game needs. Fortune's Weave needs a processor with CMPXCHG16B, SSE4.2 and POPCNT: an Intel Core i3 / i5 / i7 (2008) or newer, or an AMD FX (2011) / Ryzen or newer. Older processors (Core 2, AMD Phenom and Athlon II) cannot run it, so Set up is not started.",
        missing.join(", ")
    ))
}

pub fn check() -> Option<String> {
    problem(&probe())
}

/// A note for the Set up page when this processor gets the compatible build (no FMA3), else None.
pub fn note() -> Option<String> {
    (build_variant() == Some("compat")).then(|| {
        "This processor has no FMA3 (Intel before Haswell, AMD before Piledriver), so Set up builds the compatible version of the game for it. It plays the same, a little slower in busy scenes.".to_string()
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn haswell() -> Cpu {
        Cpu { brand: "Intel(R) Core(TM) i7-4770K".into(), fma: true, avx: true, cx16: true, sse42: true, popcnt: true }
    }

    #[test]
    fn ivy_bridge_gets_the_compatible_build() {
        let mut c = haswell();
        assert_eq!(variant_for(&c), Some("fma"));
        assert_eq!(problem(&c), None);
        c.brand = "Intel(R) Xeon(R) CPU E5-1650 v2".into();
        force(&mut c, Some("fma"));
        assert_eq!(variant_for(&c), Some("compat"));
        assert_eq!(problem(&c), None);
        force(&mut c, Some("avx"));
        assert_eq!(variant_for(&c), Some("compat"));
    }

    #[test]
    fn a_missing_feature_is_named() {
        let mut c = haswell();
        c.brand = "Intel(R) Core(TM)2 Quad CPU Q6600".into();
        force(&mut c, Some("fma,sse42"));
        assert_eq!(variant_for(&c), None);
        let m = problem(&c).unwrap();
        assert!(m.contains("SSE4.2") && m.contains("Q6600") && m.contains("2008"));
        assert!(!m.contains("POPCNT,") && !m.contains("does not support CMPXCHG16B"));
    }
}
