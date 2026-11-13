// SPDX-License-Identifier: GPL-3.0-or-later
// CPU-side texture data preparation (S4d). No Vulkan calls: the output is plain bytes plus the VkFormat that the GPU
// layer copies them into. Implementation: nvn_texdata.cpp.
//
// What prepare_texture does with the bytes of a texture in the game's memory:
//   * block-linear (Tegra X1 layout) is untiled with tegra::untile into a tight linear buffer; pitch-linear rows are copied
//     into tight rows. Either way the result is the image, levels back to back, inside a level all layers, inside a layer
//     the z slices, then rows of row_blocks blocks.
//   * BCn (BC1..BC7, BC6H, BC4/5) blocks are passed through unchanged.
//   * ASTC is kept when caps.astc, otherwise transcoded to BC7 once (caps.bc7_transcode; the result is cached on disk, keyed
//     by a hash of the source bytes), otherwise decoded to RGBA8.
//   * The disk cache entry decides the output format: BC7 as the runtime writes it, or RGBA8 / BC5 entries made by the setup tool
//     (cache_entry_format; docs/S4f.md "Texture cache entries").
//   * Output sizes: BC7 levels are rounded up to whole 4x4 blocks, i.e. (w+3)/4 * (h+3)/4 * 16 bytes per layer and slice.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <volk.h>

#include "nvn_core.h"

namespace nxrt::nvn::be {

struct TexCaps {
    bool bc = true;              // the GPU samples BCn; BCn data is passed through
    bool astc = false;           // the GPU samples ASTC; ASTC data is kept
    bool bc7_transcode = true;   // when astc is false: transcode ASTC to BC7 (else decode it to RGBA8)
    VkFormat image_format = VK_FORMAT_UNDEFINED;   // the format of the image the data goes into, when it exists already: the data comes out in it
    bool cache_only = false;     // background preparation: an ASTC texture without a disk cache entry fails quietly (error "no cache entry") instead of being transcoded
    bool upgrade = false;        // [gfx] astc_quality = high: a texture without an entry of the high preset hands its source to the background upgrade (nvn_texup.h)
    int encode_threads = 0;      // threads of an ASTC -> BC7 transcode (0: all hardware threads; background jobs use 1)
    bool provisional = false;    // render thread, texture needed now: a BC7 transcode without a cache entry uses the quickest preset (0), is not cached and
                                 // comes out with PreparedTexture::provisional set; a background job makes the real one, which replaces it (nvn_texprep.h)
    bool skip_cache = false;     // with provisional, for a texture whose background job is still reading its cache entry ([nvn] tex_prep_standin): the cache is
                                 // not read here, the stand-in is made at once and the job's result replaces it
    bool bc1 = false;            // [gfx] astc_bc = bc1 in effect (astc_bc1_active): an opaque ASTC texture comes out as BC1 when image_format allows it
    bool gpu_astc = false;       // [gfx] astc_gpu in effect (nvn_astc_gpu.h): a 2D ASTC texture without a usable cache entry is not transcoded here; it comes out as
                                 // its untiled ASTC blocks (PreparedTexture::gpu_astc) for the GPU layer to decode and encode. Wins over cache_only and provisional.
    bool skip_entries = false;   // [gfx] astc_gpu_cache_read (gpu::astc_gpu_skip_entries): BC7 and BC1 entries of the disk cache are not read (RGBA8 and BC5 entries
                                 // still are); with gpu_astc the texture goes to the GPU, without it (a queued GPU transcode's stand-in) the quick transcode is made
};

struct PreparedLevel {
    uint32_t width, height, depth;   // texels of the level
    uint64_t offset, size;           // bytes in PreparedTexture::data; the size covers all layers of the level
    uint32_t row_blocks;             // blocks per row of the output data (texels per row for uncompressed formats)
};

struct PreparedTexture {
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t levels = 0, layers = 0;
    uint32_t block_w = 1, block_h = 1;   // texels per block of the output format
    uint32_t block_bytes = 0;            // bytes per block (texel bytes for uncompressed formats)
    bool transcoded = false;             // ASTC was transcoded to BC7 or decoded to RGBA8 (the data is not the source encoding)
    bool from_cache = false;             // the data came from the BC7 disk cache
    bool provisional = false;            // a quick stand-in (TexCaps::provisional): replaced when the background job has the real data
    // TexCaps::gpu_astc: data holds the untiled ASTC blocks, not `format`. level[] then describes the source (offset and size of a level's blocks in
    // data, all layers; row_blocks = ASTC blocks per row); format / block_w / block_h / block_bytes say what the GPU layer is to make of it.
    bool gpu_astc = false;
    uint32_t src_block_w = 0, src_block_h = 0;   // the ASTC footprint
    bool src_srgb = false;
    uint64_t gpu_cache_key = 0;          // where the encoded result may be kept (cache_store_encoded); empty file: not cached
    std::string gpu_cache_file;
    std::vector<PreparedLevel> level;
    std::vector<uint8_t> data;
    std::string error;                   // set when prepare_texture returns false
};

// Prepares the data of t for the GPU described by caps. Returns false with out.error set on failure (no storage, bad layout,
// no Vulkan format, a codec error, ...). Never crashes on a bad texture.
bool prepare_texture(const Texture& t, const TexCaps& caps, PreparedTexture& out);

// [gfx] astc_gpu: the blocks the GPU encoded from a gpu_astc result (src: its format, levels, layers and level sizes; blocks: BC7 or BC1 in
// prepare_texture's output layout, every level) become the cache entry src names. Any thread. False: no cache file named, or the write failed.
bool cache_store_encoded(const PreparedTexture& src, std::vector<uint8_t>&& blocks);
// The GPU encoders' entries have their own cache key (seeded with this and the format), so they never meet a bc7e or rgbcx entry of the same
// texture; a change of the shaders' output that should retire old entries bumps it.
constexpr int kGpuEncoderVersion = 1;

// The format the disk cache entry of an ASTC texture holds when that is not BC7: R8G8B8A8 (UNORM or SRGB as the texture) or BC5_UNORM (a
// two-channel normal map). VK_FORMAT_UNDEFINED: no entry, or a BC7 entry. The GPU layer makes the image in this format (docs/S4f.md).
// With [gfx] astc_bc = bc1 in effect (bc1) it also answers BC1_RGB (UNORM or SRGB as the texture) for a texture that has a BC1 entry or that the
// alpha scan proves opaque (astc_bc1_candidate), unless an RGBA8 / BC5 entry exists.
VkFormat cache_entry_format(const Texture& t, bool bc1 = false);
// Texture content override (nxrt/tex_override.h). apply: when a hook is set and it answers for the texture, replaces bc7 (the tight
// linear BC7 blocks of the w x h texture) with the encoded answer.
bool texture_override_active();
bool texture_override_apply(const Texture& t, uint32_t w, uint32_t h, std::vector<uint8_t>& bc7);

// Texture pixel edit (nxrt/tex_edit.h; nvn_tex_edit.cpp). prepare_texture asks match for 2D ASTC textures it transcodes or decodes; for a
// non-zero id it serves the kept data of (id, generation, format) or makes it (decode, the host's edit per level, encode at tex_edit_quality)
// and keeps it; edited data never reaches the BC7 disk cache.
struct TexEditKey {
    uint64_t id = 0, gen = 0;
};
bool tex_edit_active();
TexEditKey tex_edit_match(const Texture& t, const uint8_t* src);
// true: data holds the kept data. false: this thread makes it and must call tex_edit_store or tex_edit_abandon (another thread making the same
// data is waited for first).
bool tex_edit_lookup(const TexEditKey& k, VkFormat f, std::vector<uint8_t>& data);
void tex_edit_store(const TexEditKey& k, VkFormat f, const std::vector<uint8_t>& data, double ms);
void tex_edit_abandon(const TexEditKey& k, VkFormat f);
void tex_edit_level(uint64_t id, uint32_t level, uint32_t w, uint32_t h, uint8_t* rgba);
void tex_edit_note(const Texture& t);   // the texture was given edited data (uploaded again when the answers change)
int tex_edit_quality();                 // [nvn] tex_edit_quality, bc7e preset 0..6 (default 1)

// [gfx] astc_quality (env NXRT_GFX_ASTC_QUALITY), read at every call so a test can change it; the GPU layer reads it once per run (it applies on restart):
//   fast         ASTC -> BC7 at the preset [nvn] bc7_quality (default 3; 1 with the C++ build of bc7e), written to the disk cache, as before.
//                The default with the C++ build (a2b_bc7_encoder: A2B_ENCODER_CPP, about 3 to 4 times slower than the ISPC build).
//   high         (default with the ISPC build) the same on the first use of a texture, and a background thread re-encodes it with the slow preset (kHighPreset; 3 with the C++ build; or
//                [nvn] bc7_quality when that is higher) into the cache; the next start finds the high entry. Entries of the fast preset
//                (including the BC5 / RGBA8 entries of the setup tool) stay valid and are used until the high entry exists.
//   uncompressed ASTC -> RGBA8 for every texture (4 to 16 times the size of ASTC in video memory); the disk cache is not used.
enum class AstcQuality { Fast, High, Uncompressed };
AstcQuality astc_quality();
AstcQuality astc_quality_default();   // when the key is unset or not one of the three: high, or fast with the C++ build of bc7e
const char* astc_quality_name(AstcQuality q);
int bc7_fast_preset();                // [nvn] bc7_quality as the transcodes use it (default 3, 1 with the C++ build of bc7e)
constexpr int kHighPreset = 5;   // bc7e "veryslow", the preset the setup tool can pre-build with (--bc7-quality 5)

// Uncompressed mode: counts a texture whose image was made as RGBA8 and logs the running video memory estimate (RGBA8 now against BC7 and ASTC).
void note_uncompressed_image(const Texture& t);

// Encodes a w x h RGBA8 image to BC7 in bands of rows on the calling thread, checking stop between bands (false: stopped or error; dst is then partial).
// The blocks are identical to one a2b_encode_bc7 call over the whole image (bc7e blocks are independent), which is what the test checks.
bool encode_bc7_banded(uint32_t w, uint32_t h, const uint8_t* rgba, int quality, uint8_t* dst, size_t dst_cap, const std::atomic<bool>* stop);

// [gfx] astc_bc (env NXRT_GFX_ASTC_BC; read once per run, images keep their format): how a transcoded ASTC texture is stored.
//   off   BC7 for every texture (8 bits a texel).
//   bc1   a texture the alpha scan proves opaque (every block decodes alpha 255; astc_storage_alpha_possible) is stored as BC1_RGB (4 bits a texel,
//         half of BC7; 565 endpoints, so smooth gradients and normal maps lose precision); the rest stay BC7 (BC3 would be the same size as BC7 and
//         worse, so it is not used). Its own cache entries (another key than BC7's: the key is seeded with the format).
//   auto  (default) bc1 on a GPU with under kAstcBcAutoHeap of device-local memory or an integrated GPU, off otherwise.
// Not for sparse (virtual pool) or 3D textures, nor textures a host pixel edit matches. Implementation: nvn_gpu_texture.cpp (the choice),
// nvn_texdata.cpp (the data), nvn_astc_alpha.cpp (the scan), nvn_bc1.cpp (the encoder).
bool astc_bc1_active();                                  // GPU layer: the resolved setting (false before the device exists)
constexpr uint64_t kAstcBcAutoHeap = 7ull << 30;         // auto: BC1 below this device-local heap (so 6 GB cards and smaller)
// Whether any texel of the raw ASTC storage (all levels and layers, any layout) can decode with an alpha other than 255; true when unsure.
bool astc_storage_alpha_possible(const uint8_t* mem, uint64_t bytes, uint32_t bw, uint32_t bh);
// The BC1 choice for t (as the image is made and as its data is prepared, so both agree): the setting, a 2D ASTC texture with storage, no
// host edit, and no alpha. Counts the scans (astc_bc1_log_stats).
bool astc_bc1_candidate(const Texture& t, const uint8_t* src);
// rgbcx (nvn_bc1.cpp): false when the build has no BC1 encoder. encode_bc1_image: w x h RGBA8 to BC1 blocks (alpha ignored); threads 1 = this
// thread only, 0 = up to 8.
bool bc1_encoder_available();
bool encode_bc1_image(uint32_t w, uint32_t h, const uint8_t* rgba, int level, uint8_t* dst, size_t dst_cap, int threads);
void bc1_solid_block(const uint8_t* rgba, uint8_t* out);
constexpr int kBc1Level = 5;    // rgbcx level of the cached BC1 data (0..18; 5: 66 Mpix/s and 38.9 dB on a game frame, 10: 11 Mpix/s and 39.1 dB); stand-ins use 0

// Fast 64-bit hash of a byte range (xxHash64-style mix). Used for the cache key.
uint64_t hash_bytes(const void* data, size_t size);

// Cache file of a key: <[nvn] bc7_cache dir>/<first 2 hex digits>/<16 hex digits>.bc7. Empty when the cache is disabled.
std::string bc7_cache_path(const Texture& t, uint64_t hash);

}  // namespace nxrt::nvn::be
