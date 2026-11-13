// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Sink/DeviceSink.cs (Ryubing fork, MIT). Server state of a device sink.
#pragma once
#include <cstring>
#include <vector>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "nxrt/log.h"
#include "../MemoryPool/PoolMapper.h"
#include "../Upsampler/UpsamplerManager.h"
#include "BaseSink.h"

namespace nxrt::audren {

class DeviceSink : public BaseSink {
  public:
    // The downmix coefficients (C# float[4], allocated once).
    std::vector<float> DownMixCoefficients;
    // The device parameters.
    DeviceParameter Parameter{};
    // The upsampler instance used by this sink, or null if no upsampling is needed.
    // Written as nxrt::audren::UpsamplerInfo because the member name is the same as the type name.
    nxrt::audren::UpsamplerInfo* UpsamplerInfo = nullptr;

    DeviceSink() : DownMixCoefficients(4, 0.0f) {
        // See BaseSink.h: C# runs the override here, C++ needs the explicit call.
        CleanUp();
    }

    void CleanUp() override {
        if (UpsamplerInfo != nullptr) {
            UpsamplerInfo->Release();
        }

        UpsamplerInfo = nullptr;

        BaseSink::CleanUp();
    }

    SinkType TargetSinkType() const override { return SinkType::Device; }

    void Update(BehaviourParameter::ErrorInfo& errorInfo, const SinkInParameter& parameter, SinkOutStatus& outStatus,
                PoolMapper& mapper) override {
        (void)mapper;
        NXAUDREN_ASSERT(IsTypeValid(parameter));

        // C# MemoryMarshal.Cast<byte, DeviceParameter>(SpecificData)[0], copied out because the bytes may be unaligned.
        DeviceParameter inputDeviceParameter{};
        std::memcpy(&inputDeviceParameter, parameter.specific_data, sizeof(DeviceParameter));

        if (parameter.is_used != IsUsed) {
            UpdateStandardParameter(parameter);
            Parameter = inputDeviceParameter;
            // Not in the C#: what the game mixes for the device (2 = stereo, 6 = 5.1), for [audio] channels = 6.
            if (parameter.is_used)
                NXLOG_I("AudioRenderer", "device sink: %u inputs, downmix parameter %s", static_cast<unsigned>(Parameter.input_count),
                        Parameter.down_mix_parameter_enabled ? "on" : "off");
        } else {
            Parameter.down_mix_parameter_enabled = inputDeviceParameter.down_mix_parameter_enabled;
            Span<const float>(inputDeviceParameter.down_mix_parameter, 4).CopyTo(Span<float>(Parameter.down_mix_parameter, 4));
        }

        Span<const float>(Parameter.down_mix_parameter, 4).CopyTo(Span<float>(DownMixCoefficients));

        errorInfo = BehaviourParameter::ErrorInfo{};
        outStatus = SinkOutStatus{};
    }
};

}  // namespace nxrt::audren
