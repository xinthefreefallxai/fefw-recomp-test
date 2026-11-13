// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Dsp/AudioProcessor.cs (Ryubing fork, MIT): the DSP thread. It takes one CommandList per renderer
// session, runs it against that session's output device, and paces itself to 5 ms frames (Constants.AudioProcessorMaxUpdateTimeTarget).
//
// Deviations from the C#, all forced by the port conventions or by the C++ threading model:
//   * The C# worker thread is a std::thread; the Mailbox messages, the Start/Stop handshake and the Wait pacing are the same.
//   * PerformanceCounter.ElapsedNanoseconds is std::chrono::steady_clock in nanoseconds (ElapsedNanoseconds below).
//   * C# ManualResetEvent `_event.WaitOne(1)` is a 1 ms sleep (it was never set, so it always timed out): std::this_thread::sleep_for.
//   * The C# `_pauseEvent` (IHardwareDeviceDriver.GetPauseEvent) is not part of the driver interface here, so the worker never waits on it.
//   * C# Stop() only waits for the response; the C++ Stop() also joins the worker thread, so no joinable std::thread is destroyed.
//   * C# CommandList.Dispose() becomes destroying the std::unique_ptr<CommandList> that Send() received.
//   * C# `IHardwareDeviceDriver.GetRealDeviceDriver()` (compat layer) does not exist here; the driver passed is used directly.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../Constants.h"
#include "../Dsp/Command/CommandList.h"
#include "../Integration/IHardwareDevice.h"
#include "../Integration/IHardwareDeviceDriver.h"
#include "../Integration/ThreadBoost.h"
#include "../Utils/Mailbox.h"
#include "../audren_common.h"
#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/log.h"

namespace nxrt::audren {

class AudioProcessor {
  private:
    static constexpr int32_t MaxBufferedFrames = 5;
    static constexpr int32_t TargetBufferedFrames = 3;
    // Queue level the renderer keeps on a device that has a clock (see Wait): eight frames of 5 ms.
    static constexpr int64_t TargetQueuedNanoseconds = 40000000;

    enum class MailboxMessage : uint32_t {
        Start,
        Stop,
        RenderStart,
        RenderEnd,
    };

    // One session's pending work (C# RendererSession). Owns the CommandList until it is processed.
    struct RendererSession {
        std::unique_ptr<::nxrt::audren::CommandList> CommandList;
        int32_t RenderingLimit = 0;
        uint64_t AppletResourceId = 0;
    };

    Mailbox<MailboxMessage> _mailbox;
    // Guards the session slots. The worker thread writes a slot in Send and the processor thread reads and clears it while processing;
    // DiscardSession takes the same lock, so a renderer can be closed while its CommandList is queued without a race.
    mutable std::mutex _slotMutex;
    std::vector<std::unique_ptr<RendererSession>> _sessionCommandList;
    std::thread _workerThread;

  public:
    // C# IHardwareDevice[] OutputDevices { get; private set; }: one device per session, owned here.
    std::vector<std::unique_ptr<IHardwareDevice>> OutputDevices;

  private:
    int64_t _lastTime = 0;
    int64_t _playbackEnds = 0;

    // C# PerformanceCounter.ElapsedNanoseconds.
    static int64_t ElapsedNanoseconds() {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // C# GetHardwareChannelCount: 6 channels when the driver supports them, otherwise stereo (downmixed by the device side).
    static uint32_t GetHardwareChannelCount(IHardwareDeviceDriver* deviceDriver) {
        if (deviceDriver->SupportsChannelCount(6)) {
            return 6;
        }

        // NOTE: We default to stereo as this will get downmixed to mono by the compat layer if it's not compatible.
        return 2;
    }

    void StartThread() {
        _workerThread = std::thread([this] { Work(); });
    }

    void Work() {
        BoostAudioThread();
        // [prof] sample_hz: the sampling profiler sees this thread too (it is a fifth of a core at any frame rate)
        if (nxrt::config_get_int("prof", "sample_hz", 0) > 0) nxrt::diag_register_thread("audio-dsp", nullptr);

        if (_mailbox.ReceiveMessage() != MailboxMessage::Start) {
            NXAUDREN_FATAL("Audio Processor Start message was invalid!");
        }

        _mailbox.SendResponse(MailboxMessage::Start);
        _mailbox.SendResponse(MailboxMessage::RenderEnd);

        NXLOG_I("AudioRenderer", "Starting audio processor");

        int slowLogs = 0;

        while (true) {
            MailboxMessage message = _mailbox.ReceiveMessage();

            if (message == MailboxMessage::Stop) {
                break;
            }

            if (message == MailboxMessage::RenderStart) {
                int64_t startTicks = ElapsedNanoseconds();

                std::lock_guard<std::mutex> guard(_slotMutex);

                for (size_t i = 0; i < _sessionCommandList.size(); i++) {
                    if (_sessionCommandList[i] != nullptr) {
                        // C# CommandList.Dispose() and the slot reset: the session's CommandList is destroyed with its slot.
                        _sessionCommandList[i]->CommandList->Process(OutputDevices[i].get());
                        _sessionCommandList[i].reset();
                    }
                }

                int64_t endTicks = ElapsedNanoseconds();

                int64_t elapsedTime = endTicks - startTicks;

                if (Constants::AudioProcessorMaxUpdateTime < elapsedTime) {
                    NXLOG_D("AudioRenderer", "DSP too slow (exceeded by %lldns)", static_cast<long long>(elapsedTime - Constants::AudioProcessorMaxUpdateTime));
                }

                if (elapsedTime > 15000000 && slowLogs < 40) {
                    slowLogs++;
                    NXLOG_W("AudioRenderer", "slow DSP frame: %.1f ms", static_cast<double>(elapsedTime) / 1e6);
                }

                _mailbox.SendResponse(MailboxMessage::RenderEnd);
            }
        }

        NXLOG_I("AudioRenderer", "Stopping audio processor");
        _mailbox.SendResponse(MailboxMessage::Stop);
    }

  public:
    AudioProcessor() = default;
    AudioProcessor(const AudioProcessor&) = delete;
    AudioProcessor& operator=(const AudioProcessor&) = delete;

    ~AudioProcessor() {
        // C# Dispose(): the thread is stopped if it is still running. The Stop handshake is only valid while the worker is running.
        if (_workerThread.joinable()) {
            Stop();
        }
    }

    // Start the worker thread and open one output device per session (C# Start).
    void Start(IHardwareDeviceDriver* deviceDriver) {
        OutputDevices.clear();
        OutputDevices.resize(static_cast<size_t>(Constants::AudioRendererSessionCountMax));

        uint32_t channelCount = GetHardwareChannelCount(deviceDriver);

        for (size_t i = 0; i < OutputDevices.size(); i++) {
            // TODO (C#): Don't hardcode sample rate.
            OutputDevices[i] = deviceDriver->OpenHardwareDevice(static_cast<uint32_t>(Constants::TargetSampleRate), channelCount);
        }

        _sessionCommandList.clear();
        _sessionCommandList.resize(static_cast<size_t>(Constants::AudioRendererSessionCountMax));
        _lastTime = ElapsedNanoseconds();

        StartThread();

        _mailbox.SendMessage(MailboxMessage::Start);

        if (_mailbox.ReceiveResponse() != MailboxMessage::Start) {
            NXAUDREN_FATAL("Audio Processor Start response was invalid!");
        }
    }

    // Stop the worker thread and release the output devices (C# Stop, plus the join the C++ thread needs).
    void Stop() {
        _mailbox.SendMessage(MailboxMessage::Stop);

        if (_mailbox.ReceiveResponse() != MailboxMessage::Stop) {
            NXAUDREN_FATAL("Audio Processor Stop response was invalid!");
        }

        if (_workerThread.joinable()) {
            _workerThread.join();
        }

        for (auto& device : OutputDevices) {
            device.reset();
        }
    }

    // Queue a CommandList for a session. The processor owns it from now on (C# Send).
    void Send(int32_t sessionId, std::unique_ptr<::nxrt::audren::CommandList> commands, int32_t renderingLimit, uint64_t appletResourceId) {
        auto session = std::make_unique<RendererSession>();
        session->CommandList = std::move(commands);
        session->RenderingLimit = renderingLimit;
        session->AppletResourceId = appletResourceId;

        std::lock_guard<std::mutex> guard(_slotMutex);

        _sessionCommandList[static_cast<size_t>(sessionId)] = std::move(session);
    }

    // True while a session's previous CommandList has not been processed yet (C# HasRemainingCommands).
    bool HasRemainingCommands(int32_t sessionId) const {
        std::lock_guard<std::mutex> guard(_slotMutex);

        return _sessionCommandList[static_cast<size_t>(sessionId)] != nullptr;
    }

    // Drop a session's queued CommandList without processing it. Used when the renderer that owns the memory it references is closed.
    // Waits for an in-progress processing pass, so after it returns the processor no longer touches that renderer's memory.
    void DiscardSession(int32_t sessionId) {
        std::lock_guard<std::mutex> guard(_slotMutex);

        // The slots exist only after Start: a renderer that never registered (its Initialize failed) has nothing queued.
        if (static_cast<size_t>(sessionId) < _sessionCommandList.size()) {
            _sessionCommandList[static_cast<size_t>(sessionId)].reset();
        }
    }

    // Ask the worker to process the queued CommandLists (C# Signal). Pair every Signal with a Wait.
    void Signal() { _mailbox.SendMessage(MailboxMessage::RenderStart); }

    // How much audio the output devices hold queued, in ns, measured on the device clock (the fullest device); -1 when no device has a clock.
    int64_t DeviceQueuedNanoseconds() {
        int64_t queued = -1;

        for (auto& device : OutputDevices) {
            if (device != nullptr) {
                queued = std::max(queued, device->GetQueuedNanoseconds());
            }
        }

        return queued;
    }

    // Wait for the end of the processing and pace the caller to the 5 ms frame clock (C# Wait).
    //
    // Deviation from the C#: when the output device has a clock of its own (XAudio2, or the paced sink) the device sets the pace. The
    // renderer keeps about TargetQueuedNanoseconds of audio queued: it waits while the queue holds more and renders at once when it
    // holds less. A host clock and a sound card never run at exactly the same rate; pacing by the host clock alone slowly fills the queue
    // (frames are dropped) or empties it (a click). The Ryujinx devices get this for free because their AppendBuffer blocks.
    void Wait() {
        if (_mailbox.ReceiveResponse() != MailboxMessage::RenderEnd) {
            NXAUDREN_FATAL("Audio Processor Wait response was invalid!");
        }

        int64_t increment = Constants::AudioProcessorMaxUpdateTimeTarget;

        const int64_t queued = DeviceQueuedNanoseconds();

        if (queued >= 0) {
            int64_t level = queued;

            // Longest wait is a few frames' worth, so a stuck device (the voice stopped, the queue never drains) cannot stall the guest.
            const int64_t giveUp = ElapsedNanoseconds() + 8 * increment;

            while (level > TargetQueuedNanoseconds && ElapsedNanoseconds() < giveUp) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));

                level = DeviceQueuedNanoseconds();
            }

            // Keep the host-clock bookkeeping current for the moment the device goes away.
            _lastTime = ElapsedNanoseconds();
            _playbackEnds = _lastTime;

            return;
        }

        int64_t timeNow = ElapsedNanoseconds();

        if (timeNow > _playbackEnds) {
            // Playback has restarted.
            _playbackEnds = timeNow;
        }

        _playbackEnds += increment;

        // The number of frames we are behind where the timer says we should be.
        int64_t framesBehind = (timeNow - _lastTime) / increment;

        // The number of frames yet to play on the backend.
        int64_t bufferedFrames = (_playbackEnds - timeNow) / increment + framesBehind;

        // If we've entered a situation where a lot of buffers will be queued on the backend,
        // Skip some audio frames so that playback can catch up.
        if (bufferedFrames > MaxBufferedFrames) {
            // Skip a few frames so that we're not too far behind. (the target number of frames)
            _lastTime += increment * (bufferedFrames - TargetBufferedFrames);
        }

        while (timeNow < _lastTime + increment) {
            // C# _event.WaitOne(1): a 1 ms wait on an event that is never set.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

            timeNow = ElapsedNanoseconds();
        }

        _lastTime += increment;
    }
};

}  // namespace nxrt::audren
