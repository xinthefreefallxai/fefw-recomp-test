// SPDX-License-Identifier: GPL-3.0-or-later
// State and bind commands of a command buffer (S4d). Each function takes the command buffer first and appends one
// record to the list being recorded (nvn_cmd.h, the lists of api_cmdbuf.cpp). Nothing here touches Vulkan: the replay on the
// render thread applies the records at submit time.
//
// Rules of this family:
//   * State objects (blend, channel mask, colour, multisample, polygon, depth/stencil, vertex attribute and work) are copied
//     by value: the record keeps the raw bytes of the guest object at the time of the call, because the game reuses its
//     objects right after. The replay decodes them with the decode() overloads of nvn_decoded.h.
//   * Host objects in a payload (Program, TexturePool, SamplerPool, Texture) are held (CmdList::hold), so they stay alive while
//     the list does. A guest object that is not one of ours is recorded as null.
//   * Values are stored as given (enum values unchanged). Float and int rectangles are copied as they are.
//   * Functions the replay does not need to reproduce are accepted and record nothing: shader scratch memory, program
//     subroutines, tiled cache, viewport swizzles, separate texture and sampler handles, conservative raster dilate and
//     subpixel precision bias.
//   * BindCoverageModulationTable has no fields of its own: its record carries a zero P_U32 payload and the 16 floats as blob.
// The layouts of the state objects are not read here, so no smo-recomp file was consulted for this family.
#include <algorithm>
#include <cstring>

#include "nvn_cmd.h"
#include "nvn_gpu.h"
#include "nvn_impl.h"

namespace nxrt::nvn {
using namespace be;
namespace {

CommandBuffer* cb_of(NVNcommandBuffer* c) { return host_of<CommandBuffer>(c); }

// Runs fn(list) with the command buffer locked, but only while a list is being recorded.
template <class F>
void with_rec(NVNcommandBuffer* c, F&& fn) {
    CommandBuffer* cb = cb_of(c);
    if (!cb) return;
    std::lock_guard<CmdLock> lk(cb->lock);
    if (!cb->recording || !cb->rec) return;
    fn(*cb->rec);
}

// The raw bytes of a state object, as the record keeps them.
template <class S>
P_RawState raw_state(const S* s) {
    static_assert(sizeof(S) <= 0x18, "state objects fit the record payload");
    P_RawState p{};
    std::memcpy(p.bytes, s, sizeof(S));
    p.size = static_cast<uint32_t>(sizeof(S));
    return p;
}

template <class S>
void record_state(NVNcommandBuffer* c, Op op, const S* s) {
    if (!s) return;
    const P_RawState p = raw_state(s);
    with_rec(c, [&](CmdList& l) { l.emit(op, p); });
}

void emit_u32(NVNcommandBuffer* c, Op op, uint32_t v) {
    const P_U32 p{v, 0};
    with_rec(c, [&](CmdList& l) { l.emit(op, p); });
}

void emit_f32(NVNcommandBuffer* c, Op op, float v) {
    const P_Float p{v, 0};
    with_rec(c, [&](CmdList& l) { l.emit(op, p); });
}

// Buffer bindings: kind and stage, then `count` ranges (address, size) as blob.
void emit_buffers(NVNcommandBuffer* c, uint32_t kind, uint32_t stage, int first, int count, const NVNbufferRange* ranges) {
    if (count <= 0 || !ranges) return;
    const P_BindBuffers p{kind, stage, static_cast<uint32_t>(first), static_cast<uint32_t>(count)};
    const uint32_t bytes = static_cast<uint32_t>(count) * static_cast<uint32_t>(sizeof(NVNbufferRange));
    with_rec(c, [&](CmdList& l) { l.emit(Op::BindBuffers, p, ranges, bytes); });
}

void emit_buffer(NVNcommandBuffer* c, uint32_t kind, uint32_t stage, int index, NVNbufferAddress addr, size_t size) {
    const NVNbufferRange r{addr, size};
    emit_buffers(c, kind, stage, index, 1, &r);
}

// Texture or image handles: stage, first slot, then the 64-bit handles as blob.
void emit_handles(NVNcommandBuffer* c, Op op, NVNshaderStage stage, int first, int count, const uint64_t* handles) {
    if (count <= 0 || !handles) return;
    const P_BindTextures p{static_cast<uint32_t>(stage), static_cast<uint32_t>(first), static_cast<uint32_t>(count), 0};
    const uint32_t bytes = static_cast<uint32_t>(count) * 8u;
    with_rec(c, [&](CmdList& l) {
        l.emit(op, p, handles, bytes);
        if (op == Op::BindTextures) {      // background texture preparation (nvn_texprep.h): the textures behind these handles are about to be sampled
            CommandBuffer* cb = cb_of(c);
            if (cb && cb->tpool) textures_note_bound(cb->tpool, handles, count);
        }
    });
}

// Fills a render target entry: the texture (or null) and its view when one was given.
void set_view(TargetRef& t, const NVNtextureView* v) {
    if (!v) return;
    t.view = decode(v);
    t.has_view = 1;
}

// ---- state objects: the raw bytes are recorded by value -------------------------------------------------------------------
void nvnCommandBufferBindBlendState(NVNcommandBuffer* c, const NVNblendState* s) { record_state(c, Op::BindBlend, s); }
void nvnCommandBufferBindChannelMaskState(NVNcommandBuffer* c, const NVNchannelMaskState* s) { record_state(c, Op::BindChannelMask, s); }
void nvnCommandBufferBindColorState(NVNcommandBuffer* c, const NVNcolorState* s) { record_state(c, Op::BindColor, s); }
void nvnCommandBufferBindMultisampleState(NVNcommandBuffer* c, const NVNmultisampleState* s) { record_state(c, Op::BindMultisample, s); }
void nvnCommandBufferBindPolygonState(NVNcommandBuffer* c, const NVNpolygonState* s) { record_state(c, Op::BindPolygon, s); }
void nvnCommandBufferBindDepthStencilState(NVNcommandBuffer* c, const NVNdepthStencilState* s) { record_state(c, Op::BindDepthStencil, s); }

void nvnCommandBufferBindVertexAttribState(NVNcommandBuffer* c, int count, const NVNvertexAttribState* states) {
    if (count <= 0 || !states) return;
    const P_BindVertexAttribs p{static_cast<uint32_t>(count), 0};
    const uint32_t bytes = static_cast<uint32_t>(count) * static_cast<uint32_t>(sizeof(NVNvertexAttribState));
    with_rec(c, [&](CmdList& l) { l.emit(Op::BindVertexAttribs, p, states, bytes); });
}

void nvnCommandBufferBindVertexStreamState(NVNcommandBuffer* c, int count, const NVNvertexStreamState* states) {
    if (count <= 0 || !states) return;
    const P_BindVertexStreams p{static_cast<uint32_t>(count), 0};
    const uint32_t bytes = static_cast<uint32_t>(count) * static_cast<uint32_t>(sizeof(NVNvertexStreamState));
    with_rec(c, [&](CmdList& l) { l.emit(Op::BindVertexStreams, p, states, bytes); });
}

void nvnCommandBufferBindProgram(NVNcommandBuffer* c, const NVNprogram* program, int stages) {
    Program* prog = host_of<Program>(program);
    if (prog && prog->linked && !prog->gpl_bound.load(std::memory_order_relaxed)) {
        prog->gpl_bound.store(1, std::memory_order_relaxed);
        be::gpu::program_bound(prog->stage);
    }
    const P_BindProgram p{prog, static_cast<uint32_t>(stages), 0};
    with_rec(c, [&](CmdList& l) { l.hold(prog); l.emit(Op::BindProgram, p); });
}

// ---- vertex, uniform, storage and transform feedback buffers ----------------------------------------------------------------
void nvnCommandBufferBindVertexBuffer(NVNcommandBuffer* c, int index, NVNbufferAddress addr, size_t size) {
    emit_buffer(c, kBufVertex, 0, index, addr, size);
}
void nvnCommandBufferBindVertexBuffers(NVNcommandBuffer* c, int first, int count, const NVNbufferRange* ranges) {
    emit_buffers(c, kBufVertex, 0, first, count, ranges);
}
void nvnCommandBufferBindUniformBuffer(NVNcommandBuffer* c, NVNshaderStage stage, int index, NVNbufferAddress addr, size_t size) {
    emit_buffer(c, kBufUniform, static_cast<uint32_t>(stage), index, addr, size);
}
void nvnCommandBufferBindUniformBuffers(NVNcommandBuffer* c, NVNshaderStage stage, int first, int count, const NVNbufferRange* ranges) {
    emit_buffers(c, kBufUniform, static_cast<uint32_t>(stage), first, count, ranges);
}
void nvnCommandBufferBindStorageBuffer(NVNcommandBuffer* c, NVNshaderStage stage, int index, NVNbufferAddress addr, size_t size) {
    emit_buffer(c, kBufStorage, static_cast<uint32_t>(stage), index, addr, size);
}
void nvnCommandBufferBindStorageBuffers(NVNcommandBuffer* c, NVNshaderStage stage, int first, int count, const NVNbufferRange* ranges) {
    emit_buffers(c, kBufStorage, static_cast<uint32_t>(stage), first, count, ranges);
}
void nvnCommandBufferBindTransformFeedbackBuffer(NVNcommandBuffer* c, int index, NVNbufferAddress addr, size_t size) {
    emit_buffer(c, kBufXfb, 0, index, addr, size);
}
void nvnCommandBufferBindTransformFeedbackBuffers(NVNcommandBuffer* c, int first, int count, const NVNbufferRange* ranges) {
    emit_buffers(c, kBufXfb, 0, first, count, ranges);
}

// ---- textures and images ------------------------------------------------------------------------------------------------------
void nvnCommandBufferBindTexture(NVNcommandBuffer* c, NVNshaderStage stage, int slot, NVNtextureHandle h) {
    emit_handles(c, Op::BindTextures, stage, slot, 1, &h);
}
void nvnCommandBufferBindTextures(NVNcommandBuffer* c, NVNshaderStage stage, int first, int count, const NVNtextureHandle* handles) {
    emit_handles(c, Op::BindTextures, stage, first, count, handles);
}
void nvnCommandBufferBindImage(NVNcommandBuffer* c, NVNshaderStage stage, int slot, NVNimageHandle h) {
    emit_handles(c, Op::BindImages, stage, slot, 1, &h);
}
void nvnCommandBufferBindImages(NVNcommandBuffer* c, NVNshaderStage stage, int first, int count, const NVNimageHandle* handles) {
    emit_handles(c, Op::BindImages, stage, first, count, handles);
}

// The *Deferred binds take the same arguments; the console reads the array when the commands run. Games reuse the array after the
// call (SuperMarioWonderRecomp docs/nvn/DEBUG.md #41), so it is read here, at record time. Signatures assumed (the header has none).
void nvnCommandBufferBindUniformBuffersDeferred(NVNcommandBuffer* c, NVNshaderStage stage, int first, int count, const NVNbufferRange* ranges) {
    nvnCommandBufferBindUniformBuffers(c, stage, first, count, ranges);
}
void nvnCommandBufferBindStorageBuffersDeferred(NVNcommandBuffer* c, NVNshaderStage stage, int first, int count, const NVNbufferRange* ranges) {
    nvnCommandBufferBindStorageBuffers(c, stage, first, count, ranges);
}
void nvnCommandBufferBindTexturesDeferred(NVNcommandBuffer* c, NVNshaderStage stage, int first, int count, const NVNtextureHandle* handles) {
    nvnCommandBufferBindTextures(c, stage, first, count, handles);
}
void nvnCommandBufferBindImagesDeferred(NVNcommandBuffer* c, NVNshaderStage stage, int first, int count, const NVNimageHandle* handles) {
    nvnCommandBufferBindImages(c, stage, first, count, handles);
}
// Accepted and ignored: colour reduction, stencil cull criteria and the command memory callback switch are not used by the game.
void nvnCommandBufferSetColorReductionEnable(NVNcommandBuffer*, NVNboolean) {}
void nvnCommandBufferSetColorReductionThresholds(NVNcommandBuffer*, int, int) {}
void nvnCommandBufferSetStencilCullCriteria(NVNcommandBuffer*, int, int, int) {}
void nvnCommandBufferSetCommandMemoryCallbackEnabled(NVNcommandBuffer*, NVNboolean) {}

void nvnCommandBufferSetTexturePool(NVNcommandBuffer* c, const NVNtexturePool* pool) {
    TexturePool* tp = host_of<TexturePool>(pool);
    const P_Pool p{tp};
    with_rec(c, [&](CmdList& l) {
        l.hold(tp);
        l.emit(Op::SetTexturePool, p);
        if (CommandBuffer* cb = cb_of(c)) cb->tpool = tp;
    });
}
void nvnCommandBufferSetSamplerPool(NVNcommandBuffer* c, const NVNsamplerPool* pool) {
    SamplerPool* sp = host_of<SamplerPool>(pool);
    const P_Pool p{sp};
    with_rec(c, [&](CmdList& l) { l.hold(sp); l.emit(Op::SetSamplerPool, p); });
}

// ---- viewports, scissors, depth ranges ----------------------------------------------------------------------------------------
void nvnCommandBufferSetViewport(NVNcommandBuffer* c, int x, int y, int w, int h) {
    const float r[4] = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)};
    const P_Rects p{0, 1};
    with_rec(c, [&](CmdList& l) { l.emit(Op::SetViewports, p, r, static_cast<uint32_t>(sizeof r)); });
}
void nvnCommandBufferSetViewports(NVNcommandBuffer* c, int first, int count, const float* rects) {
    if (count <= 0 || !rects) return;
    const P_Rects p{static_cast<uint32_t>(first), static_cast<uint32_t>(count)};
    const uint32_t bytes = static_cast<uint32_t>(count) * 16u;
    with_rec(c, [&](CmdList& l) { l.emit(Op::SetViewports, p, rects, bytes); });
}
void nvnCommandBufferSetViewportSwizzles(NVNcommandBuffer*, int, int, const NVNviewportSwizzle*) {}   // not recorded

void nvnCommandBufferSetScissor(NVNcommandBuffer* c, int x, int y, int w, int h) {
    const int32_t r[4] = {x, y, w, h};
    const P_Rects p{0, 1};
    with_rec(c, [&](CmdList& l) { l.emit(Op::SetScissors, p, r, static_cast<uint32_t>(sizeof r)); });
}
void nvnCommandBufferSetScissors(NVNcommandBuffer* c, int first, int count, const int* rects) {
    if (count <= 0 || !rects) return;
    const P_Rects p{static_cast<uint32_t>(first), static_cast<uint32_t>(count)};
    const uint32_t bytes = static_cast<uint32_t>(count) * 16u;
    with_rec(c, [&](CmdList& l) { l.emit(Op::SetScissors, p, rects, bytes); });
}

void nvnCommandBufferSetDepthRange(NVNcommandBuffer* c, float near_, float far_) {
    const float r[2] = {near_, far_};
    const P_DepthRanges p{0, 1};
    with_rec(c, [&](CmdList& l) { l.emit(Op::SetDepthRanges, p, r, static_cast<uint32_t>(sizeof r)); });
}
void nvnCommandBufferSetDepthRanges(NVNcommandBuffer* c, int first, int count, const float* ranges) {
    if (count <= 0 || !ranges) return;
    const P_DepthRanges p{static_cast<uint32_t>(first), static_cast<uint32_t>(count)};
    const uint32_t bytes = static_cast<uint32_t>(count) * 8u;
    with_rec(c, [&](CmdList& l) { l.emit(Op::SetDepthRanges, p, ranges, bytes); });
}
void nvnCommandBufferSetDepthBounds(NVNcommandBuffer* c, NVNboolean enable, float near_, float far_) {
    const P_DepthBounds p{static_cast<uint32_t>(enable), near_, far_, 0};
    with_rec(c, [&](CmdList& l) { l.emit(Op::SetDepthBounds, p); });
}

// ---- stencil ------------------------------------------------------------------------------------------------------------------
void stencil(NVNcommandBuffer* c, uint32_t what, NVNface face, int value) {
    const P_Stencil p{what, static_cast<uint32_t>(face), static_cast<uint32_t>(value), 0};
    with_rec(c, [&](CmdList& l) { l.emit(Op::SetStencil, p); });
}
void nvnCommandBufferSetStencilRef(NVNcommandBuffer* c, NVNface face, int ref) { stencil(c, 0, face, ref); }
void nvnCommandBufferSetStencilValueMask(NVNcommandBuffer* c, NVNface face, int mask) { stencil(c, 1, face, mask); }
void nvnCommandBufferSetStencilMask(NVNcommandBuffer* c, NVNface face, int mask) { stencil(c, 2, face, mask); }

// ---- fixed-function values ----------------------------------------------------------------------------------------------------
void nvnCommandBufferSetBlendColor(NVNcommandBuffer* c, const float* rgba) {
    if (!rgba) return;
    P_BlendColor p{};
    std::memcpy(p.c, rgba, sizeof p.c);
    with_rec(c, [&](CmdList& l) { l.emit(Op::SetBlendColor, p); });
}
void nvnCommandBufferSetPointSize(NVNcommandBuffer* c, float size) { emit_f32(c, Op::SetPointSize, size); }
void nvnCommandBufferSetLineWidth(NVNcommandBuffer* c, float width) { emit_f32(c, Op::SetLineWidth, width); }
void nvnCommandBufferSetAlphaRef(NVNcommandBuffer* c, float ref) { emit_f32(c, Op::SetAlphaRef, ref); }
void nvnCommandBufferSetPolygonOffsetClamp(NVNcommandBuffer* c, float factor, float units, float clamp) {
    const P_PolygonOffsetClamp p{factor, units, clamp, 0};
    with_rec(c, [&](CmdList& l) { l.emit(Op::SetPolygonOffsetClamp, p); });
}
void nvnCommandBufferSetSampleMask(NVNcommandBuffer* c, int mask) { emit_u32(c, Op::SetSampleMask, static_cast<uint32_t>(mask)); }
void nvnCommandBufferSetRasterizerDiscard(NVNcommandBuffer* c, NVNboolean enable) {
    emit_u32(c, Op::SetRasterizerDiscard, static_cast<uint32_t>(enable));
}
void nvnCommandBufferSetDepthClamp(NVNcommandBuffer* c, NVNboolean enable) { emit_u32(c, Op::SetDepthClamp, static_cast<uint32_t>(enable)); }
void nvnCommandBufferSetConservativeRasterEnable(NVNcommandBuffer* c, NVNboolean enable) {
    emit_u32(c, Op::SetConservativeRaster, static_cast<uint32_t>(enable));
}
void nvnCommandBufferSetConservativeRasterDilate(NVNcommandBuffer*, float) {}     // not recorded
void nvnCommandBufferSetSubpixelPrecisionBias(NVNcommandBuffer*, int, int) {}     // not recorded
void nvnCommandBufferSetPrimitiveRestart(NVNcommandBuffer* c, NVNboolean enable, int index) {
    const P_PrimitiveRestart p{static_cast<uint32_t>(enable), static_cast<uint32_t>(index)};
    with_rec(c, [&](CmdList& l) { l.emit(Op::SetPrimitiveRestart, p); });
}
void nvnCommandBufferSetPatchSize(NVNcommandBuffer* c, int size) { emit_u32(c, Op::SetPatchSize, static_cast<uint32_t>(size)); }

// Inner levels: 2 floats (outer 0). Outer levels: 4 floats (outer 1).
void nvnCommandBufferSetInnerTessellationLevels(NVNcommandBuffer* c, const float* v) {
    if (!v) return;
    P_TessLevels p{};
    p.outer = 0;
    p.v[0] = v[0];
    p.v[1] = v[1];
    with_rec(c, [&](CmdList& l) { l.emit(Op::SetTessLevels, p); });
}
void nvnCommandBufferSetOuterTessellationLevels(NVNcommandBuffer* c, const float* v) {
    if (!v) return;
    P_TessLevels p{};
    p.outer = 1;
    std::memcpy(p.v, v, sizeof p.v);
    with_rec(c, [&](CmdList& l) { l.emit(Op::SetTessLevels, p); });
}

// ---- render enable --------------------------------------------------------------------------------------------------------------
void nvnCommandBufferSetRenderEnable(NVNcommandBuffer* c, NVNboolean enable) { emit_u32(c, Op::SetRenderEnable, static_cast<uint32_t>(enable)); }
// Conditional render shares Op::SetRenderEnable; its payload is 16 bytes, the plain one 8, so the replay tells them apart.
void nvnCommandBufferSetRenderEnableConditional(NVNcommandBuffer* c, NVNconditionalRenderMode mode, NVNbufferAddress address) {
    const P_RenderEnableCond p{static_cast<uint32_t>(mode), 0, address};
    with_rec(c, [&](CmdList& l) { l.emit(Op::SetRenderEnable, p); });
}

// ---- render targets and coverage ------------------------------------------------------------------------------------------------
void nvnCommandBufferSetRenderTargets(NVNcommandBuffer* c, int count, const NVNtexture* const* colors, const NVNtextureView* const* views,
                                      const NVNtexture* depth, const NVNtextureView* depth_view) {
    P_RenderTargets p;
    std::memset(&p, 0, sizeof p);   // every entry (and every padding word) is deterministic
    p.count = static_cast<uint32_t>(std::clamp(count, 0, 8));
    for (uint32_t i = 0; i < p.count; ++i) {
        TargetRef& t = p.color[i];
        t.tex = colors ? host_of<Texture>(colors[i]) : nullptr;
        set_view(t, views ? views[i] : nullptr);
    }
    p.depth.tex = host_of<Texture>(depth);
    set_view(p.depth, depth_view);
    with_rec(c, [&](CmdList& l) {
        for (uint32_t i = 0; i < p.count; ++i) l.hold(p.color[i].tex);
        l.hold(p.depth.tex);
        l.emit(Op::SetRenderTargets, p);
    });
}

void nvnCommandBufferBindCoverageModulationTable(NVNcommandBuffer* c, const float* table) {
    if (!table) return;
    const P_U32 unused{0, 0};
    with_rec(c, [&](CmdList& l) { l.emit(Op::SetCoverageModulationTable, unused, table, static_cast<uint32_t>(16 * sizeof(float))); });
}

// ---- accepted and not recorded ----------------------------------------------------------------------------------------------------
void nvnCommandBufferSetShaderScratchMemory(NVNcommandBuffer*, const NVNmemoryPool*, ptrdiff_t, size_t) {}
void nvnCommandBufferSetProgramSubroutines(NVNcommandBuffer*, NVNprogram*, NVNshaderStage, const int, const int, const int*) {}
void nvnCommandBufferSetTiledCacheAction(NVNcommandBuffer*, NVNtiledCacheAction) {}
void nvnCommandBufferSetTiledCacheTileSize(NVNcommandBuffer*, int, int) {}
// Separate texture and sampler handles: the thunk layer takes the 8-byte structs; nothing is recorded yet.
void nvnCommandBufferBindSeparateTexture(NVNcommandBuffer*, NVNshaderStage, int, NVNseparateTextureHandle) {}
void nvnCommandBufferBindSeparateSampler(NVNcommandBuffer*, NVNshaderStage, int, NVNseparateSamplerHandle) {}
void nvnCommandBufferBindSeparateTextures(NVNcommandBuffer*, NVNshaderStage, int, int, const NVNseparateTextureHandle*) {}
void nvnCommandBufferBindSeparateSamplers(NVNcommandBuffer*, NVNshaderStage, int, int, const NVNseparateSamplerHandle*) {}

}  // namespace

void register_api_cmd_state() {
    NVN_REG(nvnCommandBufferBindBlendState);
    NVN_REG(nvnCommandBufferBindChannelMaskState);
    NVN_REG(nvnCommandBufferBindColorState);
    NVN_REG(nvnCommandBufferBindMultisampleState);
    NVN_REG(nvnCommandBufferBindPolygonState);
    NVN_REG(nvnCommandBufferBindDepthStencilState);
    NVN_REG(nvnCommandBufferBindVertexAttribState);
    NVN_REG(nvnCommandBufferBindVertexStreamState);
    NVN_REG(nvnCommandBufferBindProgram);
    NVN_REG(nvnCommandBufferBindVertexBuffer);
    NVN_REG(nvnCommandBufferBindVertexBuffers);
    NVN_REG(nvnCommandBufferBindUniformBuffer);
    NVN_REG(nvnCommandBufferBindUniformBuffers);
    NVN_REG(nvnCommandBufferBindStorageBuffer);
    NVN_REG(nvnCommandBufferBindStorageBuffers);
    NVN_REG(nvnCommandBufferBindTransformFeedbackBuffer);
    NVN_REG(nvnCommandBufferBindTransformFeedbackBuffers);
    NVN_REG(nvnCommandBufferBindTexture);
    NVN_REG(nvnCommandBufferBindTextures);
    NVN_REG(nvnCommandBufferBindUniformBuffersDeferred);
    NVN_REG(nvnCommandBufferBindStorageBuffersDeferred);
    NVN_REG(nvnCommandBufferBindTexturesDeferred);
    NVN_REG(nvnCommandBufferBindImagesDeferred);
    NVN_REG(nvnCommandBufferSetColorReductionEnable);
    NVN_REG(nvnCommandBufferSetColorReductionThresholds);
    NVN_REG(nvnCommandBufferSetStencilCullCriteria);
    NVN_REG(nvnCommandBufferSetCommandMemoryCallbackEnabled);
    NVN_REG(nvnCommandBufferBindImage);
    NVN_REG(nvnCommandBufferBindImages);
    NVN_REG(nvnCommandBufferSetTexturePool);
    NVN_REG(nvnCommandBufferSetSamplerPool);
    NVN_REG(nvnCommandBufferSetViewport);
    NVN_REG(nvnCommandBufferSetViewports);
    NVN_REG(nvnCommandBufferSetViewportSwizzles);
    NVN_REG(nvnCommandBufferSetScissor);
    NVN_REG(nvnCommandBufferSetScissors);
    NVN_REG(nvnCommandBufferSetDepthRange);
    NVN_REG(nvnCommandBufferSetDepthRanges);
    NVN_REG(nvnCommandBufferSetDepthBounds);
    NVN_REG(nvnCommandBufferSetStencilRef);
    NVN_REG(nvnCommandBufferSetStencilValueMask);
    NVN_REG(nvnCommandBufferSetStencilMask);
    NVN_REG(nvnCommandBufferSetBlendColor);
    NVN_REG(nvnCommandBufferSetPointSize);
    NVN_REG(nvnCommandBufferSetLineWidth);
    NVN_REG(nvnCommandBufferSetAlphaRef);
    NVN_REG(nvnCommandBufferSetPolygonOffsetClamp);
    NVN_REG(nvnCommandBufferSetSampleMask);
    NVN_REG(nvnCommandBufferSetRasterizerDiscard);
    NVN_REG(nvnCommandBufferSetDepthClamp);
    NVN_REG(nvnCommandBufferSetConservativeRasterEnable);
    NVN_REG(nvnCommandBufferSetConservativeRasterDilate);
    NVN_REG(nvnCommandBufferSetSubpixelPrecisionBias);
    NVN_REG(nvnCommandBufferSetPrimitiveRestart);
    NVN_REG(nvnCommandBufferSetPatchSize);
    NVN_REG(nvnCommandBufferSetInnerTessellationLevels);
    NVN_REG(nvnCommandBufferSetOuterTessellationLevels);
    NVN_REG(nvnCommandBufferSetRenderEnable);
    NVN_REG(nvnCommandBufferSetRenderEnableConditional);
    NVN_REG(nvnCommandBufferSetRenderTargets);
    NVN_REG(nvnCommandBufferBindCoverageModulationTable);
    NVN_REG(nvnCommandBufferSetShaderScratchMemory);
    NVN_REG(nvnCommandBufferSetProgramSubroutines);
    NVN_REG(nvnCommandBufferSetTiledCacheAction);
    NVN_REG(nvnCommandBufferSetTiledCacheTileSize);
    NVN_REG(nvnCommandBufferBindSeparateTexture);
    NVN_REG(nvnCommandBufferBindSeparateSampler);
    NVN_REG(nvnCommandBufferBindSeparateTextures);
    NVN_REG(nvnCommandBufferBindSeparateSamplers);
}

}  // namespace nxrt::nvn
