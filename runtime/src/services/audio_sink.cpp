// SPDX-License-Identifier: GPL-3.0-or-later
// The active host audio sink (include/nxrt/audio_sink.h): a NullSink until replaced. Also the process-wide master volume and the
// stats line that each sink logs when it closes.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>

#include "nxrt/audio_sink.h"
#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"

namespace nxrt::audio {

namespace {
std::mutex g_mu;
std::shared_ptr<AudioSink> g_sink;

std::atomic<float> g_volume{1.0f};
std::atomic<bool> g_muted{false};
std::once_flag g_volume_once;

float clamp_volume(float v) { return v < 0.0f ? 0.0f : (v > kMaxVolume ? kMaxVolume : v); }

// Reads [audio] volume the first time the volume is used. set_master_volume and master_volume both call this first.
void load_volume_config() {
    std::call_once(g_volume_once, [] {
        double v = config_get_double("audio", "volume", 1.0);
        if (!(v == v)) v = 1.0;   // NaN
        g_volume.store(clamp_volume(static_cast<float>(v)));
    });
}

// The current sink's counters (a copy of the pointer keeps the sink alive while they are read), or zeros with no sink.
SinkStats current_stats() {
    std::shared_ptr<AudioSink> s;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        s = g_sink;
    }
    return s ? s->stats() : SinkStats{};
}

// ---- gauges (include/nxrt/gauge.h): the sink's own counters, same numbers as stats_report() ----
// Removes its gauge when this file's statics are torn down (declared after g_sink, so it goes first).
struct GaugeHook {
    const char* name;
    GaugeHook(const char* n, std::function<int64_t()> f) : name(n) { nxrt::gauge_register(n, std::move(f)); }
    ~GaugeHook() { nxrt::gauge_unregister(name); }
};
GaugeHook gauge_frames_written("audio.sink_frames_written", [] { return static_cast<int64_t>(current_stats().frames_written); });
GaugeHook gauge_frames_accepted("audio.sink_frames_accepted", [] { return static_cast<int64_t>(current_stats().frames_accepted); });
GaugeHook gauge_frames_dropped("audio.sink_frames_dropped", [] { return static_cast<int64_t>(current_stats().frames_dropped); });   // overruns: device queue full
GaugeHook gauge_underruns("audio.underruns", [] { return static_cast<int64_t>(current_stats().underruns); });
GaugeHook gauge_queued_buffers_peak("audio.queued_buffers_peak", [] { return static_cast<int64_t>(current_stats().queued_peak); });
GaugeHook gauge_queued_frames_peak("audio.queued_frames_peak", [] { return static_cast<int64_t>(current_stats().queued_peak_frames); });
GaugeHook gauge_queued_frames_low("audio.queued_frames_low", [] { return static_cast<int64_t>(current_stats().queued_low_frames); });
}  // namespace

void NullSink::close() { NXLOG_I("audio", "%s", format_stats(name(), stats()).c_str()); }

void set_sink(std::shared_ptr<AudioSink> s) {
    std::shared_ptr<AudioSink> old;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        old = std::move(g_sink);
        g_sink = std::move(s);
    }
    if (old) old->close();
}

std::shared_ptr<AudioSink> sink() {
    std::unique_lock<std::mutex> lk(g_mu);
    if (!g_sink) {
        std::shared_ptr<AudioSink> chosen;
        const std::string want = config_get_str("audio", "sink", "auto");
        if (want == "paced") {
            chosen = make_paced_sink(config_get_double("audio", "paced_ppm", 0.0));
        } else if (want == "xaudio2" || want == "auto") {
            chosen = make_xaudio2_sink();
            if (!chosen) {
                NXLOG_W("audio", "XAudio2 is not available: using the null sink (no sound)");
            } else if (!chosen->open(48000, 2)) {   // the real open comes with the first frames; this only finds out whether a device is there
                NXLOG_W("audio", "XAudio2 cannot open an output device: using the null sink (no sound)");
                chosen.reset();
            } else {
                chosen->close();
            }
        } else if (want != "null") {
            NXLOG_W("audio", "[audio] sink = %s is not a sink name (auto, xaudio2, paced, null): using the null sink", want.c_str());
        }
        if (!chosen) chosen = std::make_shared<NullSink>();
        const std::string wav = config_get_str("audio", "wav_dump", "");
        if (!wav.empty()) chosen = make_wav_tee(chosen, wav);
        g_sink = chosen;
        NXLOG_I("audio", "host audio sink: %s", g_sink->name());
    }
    std::shared_ptr<AudioSink> out = g_sink;
    lk.unlock();
    start_periodic_stats();
    return out;
}

void set_master_volume(float volume) {
    load_volume_config();
    if (!(volume == volume)) return;   // NaN: keep the current volume
    g_volume.store(clamp_volume(volume));
}

float master_volume() {
    load_volume_config();
    return g_volume.load();
}

void set_muted(bool m) { g_muted.store(m); }
bool muted() { return g_muted.load(); }
float effective_volume() { return g_muted.load() ? 0.0f : master_volume(); }

void apply_boost(int16_t* pcm, uint32_t frames, uint32_t channels, float boost, uint32_t sample_rate, BoostLimiter& limiter) {
    if (!(boost > 1.0f)) {
        limiter.gain = 1.0f;
        return;
    }
    if (!pcm || channels == 0) return;
    if (boost > kMaxVolume) boost = kMaxVolume;
    constexpr float kCeiling = 32000.0f;   // about -0.2 dBFS
    const float release = 1.0f - std::exp(-1.0f / (0.1f * static_cast<float>(sample_rate ? sample_rate : 48000)));
    float g = limiter.gain;
    for (uint32_t f = 0; f < frames; f++) {
        int16_t* s = pcm + static_cast<size_t>(f) * channels;
        int peak = 0;
        for (uint32_t c = 0; c < channels; c++) peak = std::max(peak, std::abs(static_cast<int>(s[c])));
        const float want = peak > 0 ? std::min(boost, kCeiling / static_cast<float>(peak)) : boost;
        if (want < g) g = want;              // attack at once: this frame already fits under the ceiling
        else g += (want - g) * release;      // release: back toward the boost
        for (uint32_t c = 0; c < channels; c++) {
            const float v = std::clamp(static_cast<float>(s[c]) * g, -32768.0f, 32767.0f);
            s[c] = static_cast<int16_t>(std::lrint(v));
        }
    }
    limiter.gain = g;
}

std::string format_stats(const char* sink_name, const SinkStats& s) {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "sink %s: written %llu, accepted %llu, dropped %llu, underruns %llu, queued peak %u buffers, queue %u..%u frames",
                  sink_name ? sink_name : "?", static_cast<unsigned long long>(s.frames_written),
                  static_cast<unsigned long long>(s.frames_accepted), static_cast<unsigned long long>(s.frames_dropped),
                  static_cast<unsigned long long>(s.underruns), static_cast<unsigned>(s.queued_peak),
                  static_cast<unsigned>(s.queued_low_frames), static_cast<unsigned>(s.queued_peak_frames));
    return buf;
}

namespace {
// The periodic stats thread (start_periodic_stats). It never holds g_mu while it calls into a sink.
class StatsLogger {
public:
    ~StatsLogger() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }
    void start() {
        std::lock_guard<std::mutex> lk(mu_);
        if (started_) return;
        started_ = true;
        const double secs = config_get_double("audio", "stats_interval", 10.0);
        if (!(secs > 0.0)) return;
        interval_ = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(secs));
        th_ = std::thread([this] { run(); });
    }

private:
    void run() {
        using Clock = std::chrono::steady_clock;
        constexpr auto kSample = std::chrono::milliseconds(10);
        SinkStats last{};
        auto window_start = Clock::now();
        uint64_t sum = 0, samples = 0;
        uint32_t qmin = UINT32_MAX, qmax = 0;
        std::unique_lock<std::mutex> lk(mu_);
        while (!stop_) {
            cv_.wait_for(lk, kSample, [this] { return stop_; });
            if (stop_) break;
            lk.unlock();
            std::shared_ptr<AudioSink> s;
            {
                std::lock_guard<std::mutex> g(g_mu);
                s = g_sink;
            }
            if (s) {
                const uint32_t q = s->queued_frames();
                sum += q;
                samples++;
                qmin = std::min(qmin, q);
                qmax = std::max(qmax, q);
            }
            const auto now = Clock::now();
            if (s && now - window_start >= interval_) {
                const SinkStats st = s->stats();
                const double secs = std::chrono::duration<double>(now - window_start).count();
                if (st.frames_written >= last.frames_written && st.sample_rate) {
                    const double ms = 1000.0 / st.sample_rate;
                    const double avg = samples ? static_cast<double>(sum) / samples : 0.0;
                    NXLOG_I("audio",
                            "sink %s: queue avg %.1f ms (%.1f..%.1f ms), underruns +%llu (%llu), dropped +%llu, written %.0f frames/s "
                            "(rate %u)",
                            s->name(), avg * ms, (samples ? qmin : 0) * ms, qmax * ms,
                            static_cast<unsigned long long>(st.underruns - last.underruns),
                            static_cast<unsigned long long>(st.underruns),
                            static_cast<unsigned long long>(st.frames_dropped - last.frames_dropped),
                            (st.frames_written - last.frames_written) / secs, st.sample_rate);
                }
                last = st;
                window_start = now;
                sum = samples = 0;
                qmin = UINT32_MAX;
                qmax = 0;
            }
            lk.lock();
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::thread th_;
    bool started_ = false;
    bool stop_ = false;
    std::chrono::steady_clock::duration interval_{};
};
StatsLogger g_stats_logger;
}  // namespace

void start_periodic_stats() { g_stats_logger.start(); }

std::string stats_report() {
    std::shared_ptr<AudioSink> s;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        s = g_sink;
    }
    if (!s) return "no audio sink";
    return format_stats(s->name(), s->stats());
}

}  // namespace nxrt::audio
