// SPDX-License-Identifier: GPL-3.0-or-later
// Host audio output for the audio services (S3c). The audio services (audren / audout) produce interleaved 16-bit PCM and
// hand it to the active AudioSink; the sink is pulled by the host device clock. The default sink discards samples (silence), which
// is also what runs under tests. src/services/audio_xaudio2.cpp provides the XAudio2 sink (selected with [audio] sink = xaudio2).
// Options (all under [audio], see docs/CONFIG.md): wav_dump (tee every frame into a WAV file), volume (process-wide master volume)
// and device (which output endpoint the XAudio2 sink plays on).
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace nxrt::audio {

// Counters a sink keeps over its life. open() does not reset them. A sink with nothing to report leaves a field at zero.
struct SinkStats {
    uint64_t frames_written = 0;    // frames passed to write()
    uint64_t frames_accepted = 0;   // frames the sink queued for playback
    uint64_t frames_dropped = 0;    // frames write() could not queue (device queue full)
    uint64_t underruns = 0;         // times the device ran out of queued audio after playback had started
    uint32_t queued_peak = 0;       // most buffers the device held queued at one time
    uint32_t queued_peak_frames = 0;   // most frames queued at one time (sinks that can measure it)
    uint32_t queued_low_frames = 0;    // fewest frames queued after playback had started (the margin against an underrun)
    uint32_t sample_rate = 0;          // the rate of the last open() (0: never opened); turns frame counts into milliseconds
};

class AudioSink {
public:
    virtual ~AudioSink() = default;
    // Opens the device for `channels` (2 or 6) interleaved int16 channels at `sample_rate` Hz. False on failure.
    virtual bool open(uint32_t sample_rate, uint32_t channels) = 0;
    virtual void close() = 0;
    // Queues `frames` frames (frames * channels int16 values). Returns the number of frames accepted (the rest is dropped when the
    // device queue is full). Never blocks for long.
    virtual uint32_t write(const int16_t* pcm, uint32_t frames) = 0;
    // Frames queued but not yet played.
    virtual uint32_t queued_frames() = 0;
    // True when queued_frames() follows a real (or simulated) device clock that drains at the sample rate. The audio renderer then
    // paces itself by the queue level (it keeps about 40 ms queued) instead of by the host clock, so a device that runs a little fast
    // or slow never overflows or starves the queue. The null sink has no clock.
    virtual bool has_device_clock() const { return false; }
    virtual const char* name() const = 0;
    // The counters above. The base class has no write path of its own, so by default it reports zeros.
    virtual SinkStats stats() { return {}; }
};

// The null sink: accepts everything, plays nothing, reports an empty queue. Counts frames for tests.
class NullSink : public AudioSink {
public:
    bool open(uint32_t sr, uint32_t ch) override { rate = sr; channels = ch; return true; }
    void close() override;   // logs the stats line (src/services/audio_sink.cpp)
    uint32_t write(const int16_t*, uint32_t frames) override { total_frames += frames; return frames; }
    uint32_t queued_frames() override { return 0; }
    const char* name() const override { return "null"; }
    SinkStats stats() override {
        SinkStats s;
        s.frames_written = total_frames;
        s.frames_accepted = total_frames;
        s.sample_rate = rate;
        return s;
    }
    uint32_t rate = 0, channels = 0;
    uint64_t total_frames = 0;
};

void set_sink(std::shared_ptr<AudioSink> s);          // replaces the active sink (closing the old one)
// Never null. [audio] sink: auto (default: XAudio2 when it can open the default output device, else the null sink), xaudio2 (the same,
// but a failure is a warning), paced (a simulated device), null (discard). With [audio] wav_dump set, the chosen sink is wrapped in
// a WAV tee (make_wav_tee).
std::shared_ptr<AudioSink> sink();
std::shared_ptr<AudioSink> make_xaudio2_sink();       // src/services/audio_xaudio2.cpp; nullptr when XAudio2 is unavailable
// A simulated device: consumes queued frames at the sample rate (times 1 + skew_ppm / 1e6) on the host clock, counts the times it
// ran dry and the lowest and highest queue level. No output. [audio] sink = paced ([audio] paced_ppm sets the skew); what the tests and
// the underrun checks run on when no audio device is present. src/services/audio_paced.cpp.
std::shared_ptr<AudioSink> make_paced_sink(double skew_ppm = 0.0);

// A sink that forwards everything to `inner` and also writes every frame passed to write() into a 16-bit PCM WAV file at `path`.
// The file is opened at the rate and channel count of each open(); a later open() with different parameters starts a new file
// named <stem>.1<ext>, <stem>.2<ext>, and so on. The WAV header is patched about once per second of audio and at close().
// src/services/audio_wav.cpp.
std::shared_ptr<AudioSink> make_wav_tee(std::shared_ptr<AudioSink> inner, const std::string& path);

// Writes 16-bit PCM WAV files. Stereo is WAVE_FORMAT_PCM; 6 channels is WAVE_FORMAT_EXTENSIBLE with channel mask 0x3F.
// The header sizes are kept valid while the file is open, so a process that dies without close() still leaves a readable file.
class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter();
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    // Creates (truncates) the file and writes the header. Closes any file already open. False on failure.
    bool open(const std::string& path, uint32_t sample_rate, uint32_t channels);
    // Appends `frames` frames (frames * channels int16 values). No-op when the file is not open.
    void write(const int16_t* pcm, uint32_t frames);
    // Patches the header and closes the file. Safe to call when nothing is open.
    void close();

    bool is_open() const { return file_ != nullptr; }
    uint32_t sample_rate() const { return sample_rate_; }
    uint32_t channels() const { return channels_; }
    uint64_t frames_written() const { return frames_; }

private:
    void patch_header();

    std::FILE* file_ = nullptr;
    uint32_t sample_rate_ = 0;
    uint32_t channels_ = 0;
    uint64_t frames_ = 0;
    uint64_t data_bytes_ = 0;          // sample bytes written so far
    uint64_t bytes_at_patch_ = 0;      // data_bytes_ when the header was last patched
    uint64_t bytes_per_second_ = 0;
    size_t header_size_ = 0;           // 44 for stereo, 68 for extensible
};

// Master volume, process wide: [audio] volume (0.0 to 2.0, default 1.0), clamped. XAudio2Sink applies up to 1.0 on its source voice;
// above 1.0 (the boost) the rest is a software gain with a peak limiter (apply_boost), so the boosted sound never clips.
void set_master_volume(float volume);   // clamped to [0, 2]; NaN is ignored
float master_volume();
// Mute switch on top of the master volume (play mode sets it while the window has no focus and the player asked for that). Not part of
// master_volume(): the volume the player chose is kept. effective_volume() is what a sink applies: 0 while muted, else master_volume().
void set_muted(bool muted);
bool muted();
float effective_volume();
constexpr float kMaxVolume = 2.0f;   // 200%

// The boost above 100%: multiplies interleaved int16 PCM in place by boost (> 1) under a peak limiter (no lookahead: the gain drops at
// once to keep every sample under about -0.2 dBFS, then climbs back to boost over about 100 ms). boost <= 1 leaves the samples as
// they are and resets the limiter. One BoostLimiter per stream.
struct BoostLimiter {
    float gain = 1.0f;
};
void apply_boost(int16_t* pcm, uint32_t frames, uint32_t channels, float boost, uint32_t sample_rate, BoostLimiter& limiter);

// Periodic stats: while a sink is active, a low-priority thread samples its queue every 10 ms and every [audio] stats_interval seconds
// (default 10, 0 = off) logs one line: queue depth (average, min..max in ms = the output latency), underruns, drops and the rate the
// guest delivered frames at over the interval (should be the sample rate; less means the audio is starved or stretched).
// Started by sink() when it picks the sink; stopped at exit.
void start_periodic_stats();

// One line with the counters of the active sink, e.g. "sink xaudio2: written 4800, accepted 4800, ...".
std::string stats_report();
// The same line for one sink, given its name and counters. Each sink's close() logs this.
std::string format_stats(const char* sink_name, const SinkStats& s);

// An active render endpoint. `id` is the endpoint id (what [audio] device accepts as an exact value); `name` is the friendly name.
struct OutputDevice {
    std::wstring id;
    std::string name;   // UTF-8
    bool is_default = false;
};
// The active render endpoints, in the system's order. Empty when there are none or enumeration fails. src/services/audio_devices.cpp.
std::vector<OutputDevice> list_output_devices();
// The id of the default render endpoint now; empty when there is none. src/services/audio_devices.cpp.
std::wstring default_output_id();

}  // namespace nxrt::audio
