// SPDX-License-Identifier: GPL-3.0-or-later
// The audio sink options: the WAV writer and the tee sink behind [audio] wav_dump (src/services/audio_wav.cpp), the master volume
// and the output device listing (src/services/audio_devices.cpp). No real output device is opened here.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "nxrt/audio_sink.h"
#include "test_framework.h"

NXTEST_GROUP(svc_audio_sink_opts)

namespace {

constexpr uint32_t kRate = 48000;

// Interleaved ramp of `frames` frames, `channels` values per frame, kept inside int16 range.
std::vector<int16_t> make_ramp(uint32_t frames, uint32_t channels) {
    std::vector<int16_t> out(static_cast<size_t>(frames) * channels);
    for (size_t i = 0; i < out.size(); i++) {
        out[i] = static_cast<int16_t>(static_cast<int>((i * 37u) % 65000u) - 32500);
    }
    return out;
}

std::string temp_wav(const char* tag) {
    return (std::filesystem::temp_directory_path() / (std::string("nxrt_sinkopts_") + tag + ".wav")).string();
}

std::vector<uint8_t> read_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool tag_at(const std::vector<uint8_t>& b, size_t at, const char* t) {
    return at + 4 <= b.size() && std::memcmp(b.data() + at, t, 4) == 0;
}

uint32_t rd16(const std::vector<uint8_t>& b, size_t at) {
    if (at + 2 > b.size()) return 0;
    return static_cast<uint32_t>(b[at] | (b[at + 1] << 8));
}

uint32_t rd32(const std::vector<uint8_t>& b, size_t at) {
    if (at + 4 > b.size()) return 0;
    return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) | (static_cast<uint32_t>(b[at + 2]) << 16) |
           (static_cast<uint32_t>(b[at + 3]) << 24);
}

// KSDATAFORMAT_SUBTYPE_PCM as stored in a WAVE_FORMAT_EXTENSIBLE fmt chunk.
const uint8_t kSubtypePcm[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};

struct WavFields {
    bool riff = false, wave = false, fmt = false, data = false;
    uint32_t riff_size = 0;
    uint32_t fmt_size = 0, format_tag = 0, channels = 0, rate = 0, byte_rate = 0, block_align = 0, bits = 0;
    uint32_t cb_size = 0, valid_bits = 0, channel_mask = 0;
    bool subformat_pcm = false;
    uint32_t data_size = 0;
    size_t data_offset = 0;
};

// Walks the RIFF chunks and records what the tests check.
WavFields parse_wav(const std::vector<uint8_t>& b) {
    WavFields f;
    if (b.size() < 12) return f;
    f.riff = tag_at(b, 0, "RIFF");
    f.riff_size = rd32(b, 4);
    f.wave = tag_at(b, 8, "WAVE");
    size_t at = 12;
    while (at + 8 <= b.size()) {
        const uint32_t size = rd32(b, at + 4);
        const size_t body = at + 8;
        if (tag_at(b, at, "fmt ")) {
            f.fmt = true;
            f.fmt_size = size;
            f.format_tag = rd16(b, body);
            f.channels = rd16(b, body + 2);
            f.rate = rd32(b, body + 4);
            f.byte_rate = rd32(b, body + 8);
            f.block_align = rd16(b, body + 12);
            f.bits = rd16(b, body + 14);
            if (size >= 40 && body + 40 <= b.size()) {
                f.cb_size = rd16(b, body + 16);
                f.valid_bits = rd16(b, body + 18);
                f.channel_mask = rd32(b, body + 20);
                f.subformat_pcm = std::memcmp(b.data() + body + 24, kSubtypePcm, 16) == 0;
            }
        } else if (tag_at(b, at, "data")) {
            f.data = true;
            f.data_size = size;
            f.data_offset = body;
            break;
        }
        at = body + size + (size & 1u);
    }
    return f;
}

// True when the file holds `expect` as little-endian int16 samples starting at `offset`.
bool samples_equal(const std::vector<uint8_t>& b, size_t offset, const std::vector<int16_t>& expect) {
    if (offset + expect.size() * 2 > b.size()) return false;
    for (size_t i = 0; i < expect.size(); i++) {
        const int16_t v = static_cast<int16_t>(b[offset + 2 * i] | (b[offset + 2 * i + 1] << 8));
        if (v != expect[i]) return false;
    }
    return true;
}

}  // namespace

NXTEST(wav_stereo_header_and_samples) {
    const std::string path = temp_wav("stereo");
    const uint32_t frames = 1000;
    const std::vector<int16_t> ramp = make_ramp(frames, 2);
    nxrt::audio::WavWriter w;
    REQUIRE(w.open(path, kRate, 2));
    w.write(ramp.data(), 300);
    w.write(ramp.data() + 300 * 2, 700);
    w.close();

    const std::vector<uint8_t> b = read_bytes(path);
    const WavFields f = parse_wav(b);
    CHECK(f.riff);
    CHECK(f.wave);
    CHECK(f.fmt);
    CHECK(f.data);
    CHECK_EQ(f.riff_size, static_cast<uint32_t>(b.size() - 8));
    CHECK_EQ(f.fmt_size, 16u);
    CHECK_EQ(f.format_tag, 1u);
    CHECK_EQ(f.channels, 2u);
    CHECK_EQ(f.rate, kRate);
    CHECK_EQ(f.byte_rate, 192000u);
    CHECK_EQ(f.block_align, 4u);
    CHECK_EQ(f.bits, 16u);
    CHECK_EQ(f.data_offset, static_cast<size_t>(44));
    CHECK_EQ(f.data_size, frames * 4u);
    CHECK(samples_equal(b, f.data_offset, ramp));
    std::filesystem::remove(path);
}

NXTEST(wav_six_channel_extensible_header_and_samples) {
    const std::string path = temp_wav("surround");
    const uint32_t frames = 500;
    const std::vector<int16_t> ramp = make_ramp(frames, 6);
    nxrt::audio::WavWriter w;
    REQUIRE(w.open(path, kRate, 6));
    w.write(ramp.data(), frames);
    w.close();

    const std::vector<uint8_t> b = read_bytes(path);
    const WavFields f = parse_wav(b);
    CHECK(f.riff);
    CHECK(f.wave);
    CHECK(f.fmt);
    CHECK(f.data);
    CHECK_EQ(f.riff_size, static_cast<uint32_t>(b.size() - 8));
    CHECK_EQ(f.fmt_size, 40u);
    CHECK_EQ(f.format_tag, 0xFFFEu);
    CHECK_EQ(f.channels, 6u);
    CHECK_EQ(f.rate, kRate);
    CHECK_EQ(f.byte_rate, 576000u);
    CHECK_EQ(f.block_align, 12u);
    CHECK_EQ(f.bits, 16u);
    CHECK_EQ(f.cb_size, 22u);
    CHECK_EQ(f.valid_bits, 16u);
    CHECK_EQ(f.channel_mask, 0x3Fu);
    CHECK(f.subformat_pcm);
    CHECK_EQ(f.data_offset, static_cast<size_t>(68));
    CHECK_EQ(f.data_size, frames * 12u);
    CHECK(samples_equal(b, f.data_offset, ramp));
    std::filesystem::remove(path);
}

NXTEST(wav_tee_forwards_and_writes_file) {
    const std::string path = temp_wav("tee");
    auto inner = std::make_shared<nxrt::audio::NullSink>();
    std::shared_ptr<nxrt::audio::AudioSink> tee = nxrt::audio::make_wav_tee(inner, path);
    REQUIRE(tee->open(kRate, 2));
    const uint32_t frames = 480;
    const std::vector<int16_t> ramp = make_ramp(frames, 2);
    CHECK_EQ(tee->write(ramp.data(), frames), frames);
    CHECK_EQ(inner->total_frames, static_cast<uint64_t>(frames));
    const nxrt::audio::SinkStats s = tee->stats();
    CHECK_EQ(s.frames_written, static_cast<uint64_t>(frames));
    CHECK_EQ(s.frames_accepted, static_cast<uint64_t>(frames));
    tee->close();

    const std::vector<uint8_t> b = read_bytes(path);
    const WavFields f = parse_wav(b);
    CHECK(f.riff);
    CHECK_EQ(f.format_tag, 1u);
    CHECK_EQ(f.channels, 2u);
    CHECK_EQ(f.rate, kRate);
    CHECK_EQ(f.data_size, frames * 4u);
    CHECK(samples_equal(b, f.data_offset, ramp));
    std::filesystem::remove(path);
}

NXTEST(wav_header_valid_while_open) {
    const std::string path = temp_wav("live");
    nxrt::audio::WavWriter w;
    REQUIRE(w.open(path, kRate, 2));
    const std::vector<int16_t> second = make_ramp(kRate, 2);
    // One full second in one call: the header is patched at once, while the file is still open.
    w.write(second.data(), kRate);
    {
        const std::vector<uint8_t> b = read_bytes(path);
        const WavFields f = parse_wav(b);
        CHECK(f.riff);
        CHECK(f.data);
        CHECK_EQ(f.data_size, 192000u);
        CHECK_EQ(f.riff_size, static_cast<uint32_t>(36 + 192000));
        CHECK(b.size() >= 44 + 192000);
        CHECK(samples_equal(b, f.data_offset, second));
    }
    // More audio, still open: the header keeps the last patched size, so the file reads as valid even if it stops here.
    const std::vector<int16_t> more = make_ramp(12000, 2);
    w.write(more.data(), 12000);
    {
        const std::vector<uint8_t> b = read_bytes(path);
        const WavFields f = parse_wav(b);
        CHECK(f.riff);
        CHECK_EQ(f.data_size, 192000u);
        CHECK_EQ(f.riff_size, static_cast<uint32_t>(36 + 192000));
    }
    w.close();
    const std::vector<uint8_t> b = read_bytes(path);
    const WavFields f = parse_wav(b);
    CHECK_EQ(f.data_size, 240000u);
    CHECK_EQ(f.riff_size, static_cast<uint32_t>(b.size() - 8));
    std::filesystem::remove(path);
}

NXTEST(master_volume_clamps_and_round_trips) {
    const float saved = nxrt::audio::master_volume();
    nxrt::audio::set_master_volume(0.25f);
    CHECK_EQ(nxrt::audio::master_volume(), 0.25f);
    nxrt::audio::set_master_volume(1.5f);   // up to kMaxVolume (200%): the boost
    CHECK_EQ(nxrt::audio::master_volume(), 1.5f);
    nxrt::audio::set_master_volume(3.0f);
    CHECK_EQ(nxrt::audio::master_volume(), nxrt::audio::kMaxVolume);
    nxrt::audio::set_master_volume(-3.0f);
    CHECK_EQ(nxrt::audio::master_volume(), 0.0f);
    nxrt::audio::set_master_volume(0.5f);
    CHECK_EQ(nxrt::audio::master_volume(), 0.5f);
    nxrt::audio::set_master_volume(std::nanf(""));   // NaN is ignored
    CHECK_EQ(nxrt::audio::master_volume(), 0.5f);
    nxrt::audio::set_master_volume(saved);
}

NXTEST(list_output_devices_does_not_crash) {
    // The content depends on the machine (a CI runner may have no endpoints), so only the calls are checked.
    const auto first = nxrt::audio::list_output_devices();
    const auto second = nxrt::audio::list_output_devices();
    CHECK(first.size() < 4096);
    CHECK(second.size() < 4096);
}
