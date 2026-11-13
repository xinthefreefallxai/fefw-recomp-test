// SPDX-License-Identifier: GPL-3.0-or-later
// [gfx] astc_gpu: ASTC textures the GPU cannot sample are decoded and encoded on the GPU instead of by the CPU transcoder (#122, #128, #133:
// a new area kept six to eight cores busy for minutes, its textures drawn as stand-ins meanwhile). Implementation: nvn_astc_gpu.cpp; shaders:
// shaders/astc_decode.comp, bc7_encode.comp, bc1_encode.comp (compiled into nvn_astc_gpu_shaders.inc by scripts/gen_astc_gpu_shaders.py).
//
// The path of a texture without a cache entry:
//   * prepare_texture (TexCaps::gpu_astc) hands out the untiled ASTC blocks instead of transcoding them (PreparedTexture::gpu_astc); a
//     background job (nvn_texprep.cpp) does that as it reads the cache, so nothing waits on the render thread.
//   * tex_ensure_current gives them to astc_gpu_upload. Recorded into the frame's command buffer: the blocks go from staging into a
//     device-local scratch buffer, a compute pass decodes them to RGBA8 behind them, a second one encodes those texels to the image's
//     format (BC7; BC1 for a texture [gfx] astc_bc stores so; none for an RGBA8 image), and a buffer-to-image copy fills the levels. The
//     scratch buffer is reused by every texture (chunks of at most 32 MiB of texels) and released after ten seconds without use.
//   * Budget: a texture the GPU is done with sooner than the render thread would be with its stand-in (1.5 ms at the measured rate) is always
//     transcoded in the frame that first uses it. A larger one goes at once only while the present's budget lasts ([gfx] astc_gpu_ms at 30
//     presents a second, a quarter of the present interval; a long frame raises it for the frames after it). Over it the texture is queued:
//     it shows the CPU path's quick stand-in, or what it showed before, and astc_gpu_pump (start of each submission, once per present)
//     records it over the following presents, a large level in bands of about 2 ms.
//   * Cache ([gfx] astc_gpu_cache; auto: only on a GPU slower than 2.5 ms per M texel, where a transcode costs more than reading an entry):
//     the encoded blocks are also copied to a host buffer; when the GPU has run the frame a worker thread writes them to [nvn] bc7_cache
//     under the GPU encoders' own key, so later runs read them like any entry.
// Fallback to the CPU transcoder: [gfx] astc_gpu = off; a device whose pipelines fail to build or whose self-test (the decode compared with
// the CPU decoder on random blocks, both encoders' blocks decoded again and held to an error bound) fails; and, per texture, sparse and 3D
// textures, host pixel edits and BC5 images. A failed scratch or staging allocation falls back for that texture.
// Tests: tests/gfx/test_nvn_astc_gpu.cpp; [nvn] astc_gpu_probe, astc_gpu_fail_test and astc_gpu_slow (docs/CONFIG.md).
#pragma once
#include <cstdint>
#include <vector>

#include <volk.h>

#include "nxrt/gfx.h"
#include "nvn_core.h"

namespace nxrt::nvn::be {
struct PreparedTexture;
namespace gpu {
struct Frame;

// Any thread. True when the GPU path is on for this device. The first caller with wait builds the pipelines and runs the self-test (0.1 s
// with the driver's shader cache, 1.4 to 2.3 s at a first start; background jobs call it so); without wait the answer is false until that
// has happened (the render thread).
bool astc_gpu_enabled(bool wait);
// The back end is up on a device: the pipelines and the self-test start on a thread of their own, so the first texture does not wait for them.
void astc_gpu_start();
bool astc_gpu_ready();             // any thread: astc_gpu_enabled(true) answers without waiting
// [gfx] astc_gpu_upgrade: textures the GPU encoded are also re-encoded by the background CPU pool of [gfx] astc_quality = high.
bool astc_gpu_upgrade();
// Any thread. [gfx] astc_gpu_cache_read: true when BC7 and BC1 entries of [nvn] bc7_cache are not read and the GPU transcodes such textures again
// (TexCaps::skip_entries). auto: while the GPU is fast (as astc_gpu_cache = auto; also before its rate is measured), unless astc_gpu_cache = on
// or astc_gpu_upgrade asks for entries. On an RTX 4070 Super the hub's load frame took 2.4 to 3.0 s transcoding and 4.0 to 13.3 s reading entries.
bool astc_gpu_skip_entries();

enum class AstcGpu { Done, Deferred, Failed };
// Render thread, no pass open on f.cb. prep: a gpu_astc result for t whose format is the image's. staged: the upload buffer a background
// job copied prep's blocks into, or null (prep.data holds them); taken over (set to null) unless the call fails.
//   Done      the image's levels are filled by commands recorded into f.
//   Deferred  over this present's budget: queued (prep and staged are taken). The caller keeps what the image shows, or makes a stand-in
//             for an image that shows nothing yet; astc_gpu_pump fills it in a later present.
//   Failed    nothing recorded; the caller prepares the texture on the CPU.
AstcGpu astc_gpu_upload(Texture* t, Frame& f, PreparedTexture& prep, gfx::Buffer& staged);
// Render thread: something for astc_gpu_pump to do (queued textures, or a scratch buffer to release); one atomic load.
bool astc_gpu_work();
// Render thread, no pass open: once per present, records queued transcodes within the budget.
void astc_gpu_pump(Frame& f);
void astc_gpu_shutdown();          // device idle: frees the pipelines and buffers; astc_gpu_start builds them again

// nvn_gpu_texture.cpp: copies buffer regions into the image of t (as an upload does) and leaves it sampled. False: t has no usable image.
bool tex_copy_buffer_regions(Texture* t, Frame& f, VkBuffer buf, const std::vector<VkBufferImageCopy>& regions);
// nvn_gpu_texture.cpp: identity of t's image (null: none), to tell whether a queued transcode still belongs to it.
const void* tex_gpu_identity(Texture* t);

}  // namespace gpu
}  // namespace nxrt::nvn::be
