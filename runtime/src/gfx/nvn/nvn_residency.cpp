// SPDX-License-Identifier: GPL-3.0-or-later
// Texture residency: the pool feedback marks (nvn_residency.h).
#include "nvn_residency.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nvn_gpu_internal.h"
#include "nvn_shaders.h"

namespace nxrt::nvn::be::gpu {
namespace {
constexpr const char* kTag = "nvn.resid";

enum : uint32_t {
    OpExtension = 10, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17, OpTypeInt = 21, OpTypeRuntimeArray = 29,
    OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpFunction = 54, OpVariable = 59, OpStore = 62, OpAccessChain = 65,
    OpInBoundsAccessChain = 66, OpDecorate = 71, OpMemberDecorate = 72, OpDecorationGroup = 73, OpGroupDecorate = 74,
    OpGroupMemberDecorate = 75, OpKill = 252, OpDecorateId = 332, OpTerminateInvocation = 4416, OpDemoteToHelperInvocation = 5380,
    OpDecorateString = 5632, OpMemberDecorateString = 5633,
    DecBlock = 2, DecArrayStride = 6, DecBuiltIn = 11, DecBinding = 33, DecDescriptorSet = 34, DecOffset = 35,
    BuiltInSampleMask = 20, BuiltInFragDepth = 22, StorageBuffer = 12, ModelFragment = 4, ModeEarlyFragmentTests = 9,
};

bool is_annotation(uint32_t op) {
    return (op >= OpDecorate && op <= OpGroupMemberDecorate) || op == OpDecorateId || op == OpDecorateString || op == OpMemberDecorateString;
}
}  // namespace

bool residency_on() {
    static const bool on = [] {
        std::string v = config_get_str("gfx", "residency", "auto");
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool r = v == "auto" || v.empty() ? true : config_get_bool("gfx", "residency", true);
        NXLOG_I(kTag, "texture residency ([gfx] residency = %s): %s", v.c_str(), r ? "the pool shaders mark what they sample; idle textures are demoted near the budget" : "off");
        return r;
    }();
    return on;
}

bool spirv_pool_feedback(std::vector<uint32_t>& w) {
    if (w.size() < 5 || w[0] != 0x07230203u || w[1] < 0x10300u) return false;
    const uint32_t version = w[1];
    struct I { size_t at; uint32_t op, wc; };
    std::vector<I> ins;
    for (size_t i = 5; i < w.size();) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (wc == 0 || i + wc > w.size()) return false;
        ins.push_back({i, op, wc});
        i += wc;
    }
    // the pool texture arrays (moved to kPoolSet), the uint type, an existing StorageBuffer pointer to it, what the fragment shader may do
    std::unordered_map<uint32_t, uint32_t> set_of, binding_of;
    uint32_t uint_t = 0;
    bool kills = false, depth_or_mask = false, early = false, fragment = false;
    size_t last_entry = SIZE_MAX, last_annot = SIZE_MAX, first_fn = SIZE_MAX;
    for (size_t k = 0; k < ins.size(); ++k) {
        const I& x = ins[k];
        const uint32_t* a = &w[x.at + 1];
        if (x.op == OpDecorate && x.wc >= 4) {
            if (a[1] == DecDescriptorSet) set_of[a[0]] = a[2];
            if (a[1] == DecBinding) binding_of[a[0]] = a[2];
            if (a[1] == DecBuiltIn && (a[2] == BuiltInFragDepth || a[2] == BuiltInSampleMask)) depth_or_mask = true;
        }
        if (x.op == OpMemberDecorate && x.wc >= 5 && a[2] == DecBuiltIn && (a[3] == BuiltInFragDepth || a[3] == BuiltInSampleMask)) depth_or_mask = true;
        if (is_annotation(x.op)) last_annot = k;
        if (x.op == OpEntryPoint) { last_entry = k; fragment |= a[0] == ModelFragment; }
        if (x.op == OpExecutionMode && x.wc >= 3 && a[1] == ModeEarlyFragmentTests) early = true;
        if (x.op == OpTypeInt && x.wc == 4 && a[1] == 32 && a[2] == 0 && !uint_t) uint_t = a[0];
        if (x.op == OpKill || x.op == OpTerminateInvocation || x.op == OpDemoteToHelperInvocation) kills = true;
        if (x.op == OpFunction && first_fn == SIZE_MAX) first_fn = k;
    }
    std::unordered_set<uint32_t> pool_vars;
    for (auto& [id, s] : set_of)
        if (s == kPoolSet) {
            auto b = binding_of.find(id);
            if (b != binding_of.end() && (b->second == kPoolTex2D || b->second == kPoolTexCube)) pool_vars.insert(id);
        }
    if (pool_vars.empty() || last_entry == SIZE_MAX || last_annot == SIZE_MAX || first_fn == SIZE_MAX) return true;
    bool any = false;
    for (const I& x : ins)
        if ((x.op == OpAccessChain || x.op == OpInBoundsAccessChain) && x.wc >= 5 && pool_vars.count(w[x.at + 3])) any = true;
    if (!any) return true;
    uint32_t ptr_u = 0;
    if (uint_t)
        for (const I& x : ins)
            if (x.op == OpTypePointer && x.wc == 4 && w[x.at + 2] == StorageBuffer && w[x.at + 3] == uint_t) { ptr_u = w[x.at + 1]; break; }

    uint32_t bound = w[3];
    const bool new_uint = uint_t == 0;
    if (new_uint) uint_t = bound++;
    const bool new_ptr_u = ptr_u == 0;
    if (new_ptr_u) ptr_u = bound++;
    const uint32_t rta = bound++, st = bound++, ptr_st = bound++, var = bound++, c0 = bound++, c1 = bound++;
    const bool add_early = fragment && !kills && !depth_or_mask && !early;

    std::vector<uint32_t> out;
    out.reserve(w.size() + 64);
    out.insert(out.end(), w.begin(), w.begin() + 5);
    auto emit = [&](std::initializer_list<uint32_t> v) {
        out.push_back(static_cast<uint32_t>(v.size()) << 16 | *v.begin());
        out.insert(out.end(), v.begin() + 1, v.end());
    };
    for (size_t k = 0; k < ins.size(); ++k) {
        const I& x = ins[k];
        if (k == first_fn) {   // the declarations, at the end of the global section
            if (new_uint) emit({OpTypeInt, uint_t, 32, 0});
            emit({OpTypeRuntimeArray, rta, uint_t});
            emit({OpTypeStruct, st, rta});
            emit({OpTypePointer, ptr_st, StorageBuffer, st});
            if (new_ptr_u) emit({OpTypePointer, ptr_u, StorageBuffer, uint_t});
            emit({OpConstant, uint_t, c0, 0});
            emit({OpConstant, uint_t, c1, 1});
            emit({OpVariable, ptr_st, var, StorageBuffer});
        }
        if (x.op == OpEntryPoint && version >= 0x10400) {   // SPIR-V 1.4: every global the entry point uses is in its interface
            out.push_back((x.wc + 1) << 16 | OpEntryPoint);
            out.insert(out.end(), w.begin() + static_cast<std::ptrdiff_t>(x.at + 1), w.begin() + static_cast<std::ptrdiff_t>(x.at + x.wc));
            out.push_back(var);
        } else {
            out.insert(out.end(), w.begin() + static_cast<std::ptrdiff_t>(x.at), w.begin() + static_cast<std::ptrdiff_t>(x.at + x.wc));
        }
        if (k == last_entry && add_early)
            for (const I& e : ins)
                if (e.op == OpEntryPoint && w[e.at + 1] == ModelFragment) emit({OpExecutionMode, w[e.at + 2], ModeEarlyFragmentTests});
        if (k == last_annot) {
            emit({OpDecorate, rta, DecArrayStride, 4});
            emit({OpDecorate, st, DecBlock});
            emit({OpMemberDecorate, st, 0, DecOffset, 0});
            emit({OpDecorate, var, DecDescriptorSet, kPoolSet});
            emit({OpDecorate, var, DecBinding, kPoolFeedback});
        }
        if ((x.op == OpAccessChain || x.op == OpInBoundsAccessChain) && x.wc >= 5 && pool_vars.count(w[x.at + 3])) {
            const uint32_t p = bound++;
            emit({OpAccessChain, ptr_u, p, var, c0, w[x.at + 4]});
            emit({OpStore, p, c1});
        }
    }
    out[3] = bound;
    w.swap(out);
    return true;
}

// ---- feedback buffers ------------------------------------------------------------------------------------------------------
thread_local bool g_resid_pool_view = false;

namespace {
std::atomic<bool> g_unmarked{false};
constexpr VkDeviceSize kFbBytes = sizeof(uint32_t) * kPoolTextureCount;
}  // namespace

struct PoolFeedback {
    gfx::Buffer dev, rb;
    uint64_t serial = 0;    // submission that copied dev into rb (pending)
    bool pending = false;
    bool fresh = true;      // dev was never cleared (its first readback is ignored)
    bool first = true;      // the readback in flight is the uncleared one
};

void residency_unmarked() {
    if (!g_unmarked.exchange(true))
        NXLOG_W(kTag, "a shader that samples through the texture pools could not be marked: textures are no longer demoted ([gfx] residency)");
}
bool residency_marks_complete() { return !g_unmarked.load(std::memory_order_relaxed); }

PoolFeedback* feedback_create() {
    auto* fb = new PoolFeedback();
    if (device().create_buffer(kFbBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               gfx::MemoryUse::GpuOnly, &fb->dev, "nvn pool feedback") != VK_SUCCESS ||
        device().create_buffer(kFbBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Readback, &fb->rb, "nvn pool feedback readback") != VK_SUCCESS) {
        feedback_destroy(fb);
        NXLOG_W(kTag, "pool feedback buffer failed: textures are no longer demoted");
        residency_unmarked();
        return nullptr;
    }
    return fb;
}

void feedback_destroy(PoolFeedback* fb) {
    if (!fb) return;
    if (fb->dev) device().destroy_buffer(fb->dev);
    if (fb->rb) device().destroy_buffer(fb->rb);
    delete fb;
}

VkBuffer feedback_buffer(PoolFeedback* fb) { return fb ? fb->dev.buffer : VK_NULL_HANDLE; }

// Copies the marks to the readback and clears them, in submission order (the stores of the submissions before, the draws after).
void feedback_record(PoolFeedback* fb, Frame& f) {
    if (!fb || fb->pending) return;
    memory_barrier(f.cb);
    VkBufferCopy c = {0, 0, kFbBytes};
    vkCmdCopyBuffer(f.cb, fb->dev.buffer, fb->rb.buffer, 1, &c);
    vkCmdFillBuffer(f.cb, fb->dev.buffer, 0, kFbBytes, 0);
    memory_barrier(f.cb);
    fb->pending = true;
    fb->first = fb->fresh;
    fb->fresh = false;
    fb->serial = f.serial;
}

const uint32_t* feedback_take(PoolFeedback* fb, uint64_t completed) {
    if (!fb || !fb->pending || completed < fb->serial) return nullptr;
    fb->pending = false;
    if (fb->first) return nullptr;   // what the buffer held before its first clear
    device().invalidate_buffer(fb->rb);
    return static_cast<const uint32_t*>(fb->rb.mapped);
}

void residency_begin_submit(Frame& f) {
    static uint32_t last = 0, last_restore = UINT32_MAX;
    const uint32_t now = static_cast<uint32_t>(counters().presents.load(std::memory_order_relaxed));
    if (now - last >= kResidencyPeriod) {
        last = now;
        if (residency_on()) pool_feedback_tick(f);   // nvn_pipeline.cpp: readbacks done stamp their textures; every pool pair's marks copied and cleared
        tex_residency_tick(f);   // nvn_gpu_texture.cpp: failed images retried; demote idle ones under pressure
    }
    // the first submission of each present (a frame has several): restore the demoted textures seen again, within the budget
    if (residency_on() && now != last_restore) {
        last_restore = now;
        tex_residency_restore(f);
    }
}

}  // namespace nxrt::nvn::be::gpu
