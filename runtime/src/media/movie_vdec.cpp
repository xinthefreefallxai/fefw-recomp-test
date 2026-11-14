// SPDX-License-Identifier: GPL-3.0-or-later
// movie::VideoDecoder (the Switch 2 movie library; Fortune's Weave's subsdk2) on the host, decoding AV1 with dav1d. include/nxrt/movie_vdec.h.
//
// Ported from nxnative-penis6 dm/host/hle_nx2_movie.c (MIT; commits 9d1a455e17 and 3f9113b10e). The library's own decoder drives nn::codec,
// the NVDEC of the skipped GPU boot. Its nn::movie::MediaExtractor runs as it is, so the game demuxes and this decodes. The player (main
// 0x1900000-0x1907xxx) registers VideoFrames with its own buffers, hands packets to DecodeAsync on the input event, takes pictures with
// AcquireVideoFrame on the output event and gives them back with ReleaseVideoFrame. The frames are the game's: this writes the fields their
// accessors read. Pictures are NV12 at the picture's width: the player's CPU-frame path. The console's player uses native images, which the
// host cannot fill, so the game module selects the other path (games/ObeliskChamber/ObeliskChamber_play.cpp).
//
// Unlike the fork, a host thread per decoder decodes ahead (dav1d is C only here: a 1440p picture takes tens of milliseconds, which stalled
// the game's main thread when it was decoded inside ReleaseVideoFrame). Up to kAhead pictures wait in host memory; a guest call or the worker
// copies them into free frames. The input and output events are host kernel events attached to the guest's SystemEventTypes (as vi's vsync),
// so the worker can wake the player.
//
// Layouts (SDK 22.2): VideoFrame +0x08 buffer, +0x28 / +0x30 bytes of luma / chroma written, +0x48 buffer size, +0x78 time, +0x88 end of
// stream (u8); nn::movie::Buffer +0x00 data, +0x10 bytes, +0x18 time, +0x20 flags (8: end of stream). The host state hangs off the guest
// object's first word (where the library keeps its own), with a marker in the second.
//   [media] movies = host | skip          host (default for a title with game::Info::movie_video_decoder); skip: every movie fails to open
//                                         and the game moves on as after a skip (core/switch2.cpp)
//   [media] video_decoder_threads = 4     dav1d worker threads per decoder
//   [media] video_decoder_log = 1         one log line per decoder call (verbose)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/movie_vdec.h"
#include "nxrt/services.h"

#if NXRT_HAVE_DAV1D
#include <dav1d/dav1d.h>
#endif

namespace nxrt::nvn::be::gpu {   // gfx/nvn/nvn_gpu.h (its header needs the NVN include path)
uint64_t last_serial();
bool serial_done(uint64_t serial);
}  // namespace nxrt::nvn::be::gpu

namespace nxrt::media {

namespace {
std::atomic<bool> g_enabled{false};
}

#if NXRT_HAVE_DAV1D
namespace {

#define A(i) (c->x[i])
#define RET(v) (c->x[0] = static_cast<uint64_t>(v))

constexpr const char* kTag = "movie";
constexpr uint64_t kMarker = 0x4e5852434d564443ull;   // "NXRCMVDC" at +8 of the guest object
constexpr uint64_t kVfBuf = 0x08, kVfFilled = 0x28, kVfSize = 0x48, kVfTime = 0x78, kVfEos = 0x88;
constexpr uint64_t kBufData = 0x00, kBufFilled = 0x10, kBufTime = 0x18, kBufFlags = 0x20;
constexpr uint32_t kBufFlagEos = 8;
constexpr int kMimeAv1 = 3;                // nn::movie::MediaMimeType of an AV1 track
constexpr uint32_t kResultNoFrame = 0x7b;  // AcquireVideoFrame with no picture ready, as the library answers
constexpr uint32_t kAutoClear = 1;         // nn::os::EventClearMode_AutoClear
constexpr size_t kMaxFrames = 64;
constexpr size_t kAhead = 4;               // decoded pictures held in host memory while no frame is free

bool g_log = false;
int g_threads = 4;
std::atomic<uint64_t> s_packets{0}, s_pictures{0}, s_acquired{0}, s_released{0};
// Decode time of the packets since the last stats line (worker threads): total and the slowest, in microseconds.
std::atomic<uint64_t> s_dec_us{0}, s_dec_n{0}, s_dec_max_us{0};

// A stats line at most every 5 s while a movie plays: packets in, pictures decoded, frames acquired and released by the player.
void stats_tick() {
    static std::atomic<int64_t> last_ns{0};
    const int64_t now = static_cast<int64_t>(GetTickCount64()) * 1000000;
    int64_t prev = last_ns.load(std::memory_order_relaxed);
    if (prev && now - prev < 5'000'000'000ll) return;
    if (!last_ns.compare_exchange_strong(prev, now)) return;
    const uint64_t n = s_dec_n.exchange(0), us = s_dec_us.exchange(0), mx = s_dec_max_us.exchange(0);
    NXLOG_I(kTag, "stats: packets %llu, pictures %llu, frames acquired %llu, released %llu; decode %.2f ms per packet (slowest %.1f) over %llu",
            static_cast<unsigned long long>(s_packets.load()), static_cast<unsigned long long>(s_pictures.load()),
            static_cast<unsigned long long>(s_acquired.load()), static_cast<unsigned long long>(s_released.load()),
            n ? us / 1000.0 / n : 0.0, mx / 1000.0, static_cast<unsigned long long>(n));
}

inline uint64_t rd64(uint64_t a) { uint64_t v; std::memcpy(&v, reinterpret_cast<const void*>(a), 8); return v; }
inline uint32_t rd32(uint64_t a) { uint32_t v; std::memcpy(&v, reinterpret_cast<const void*>(a), 4); return v; }
inline void wr64(uint64_t a, uint64_t v) { std::memcpy(reinterpret_cast<void*>(a), &v, 8); }
inline void wr8(uint64_t a, uint8_t v) { std::memcpy(reinterpret_cast<void*>(a), &v, 1); }

// A guest function called from a handler, on the guest thread that called the import; returns x0.
uint64_t gcall(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0) {
    if (!fn) return 0;
    uint64_t save[9];
    std::memcpy(save, c->x, sizeof save);
    const uint64_t lr = c->x[30], f = c->fn;
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3;
    dispatch_call(c, fn);
    const uint64_t r = c->x[0];
    std::memcpy(c->x, save, sizeof save);
    c->x[30] = lr;
    c->fn = f;
    return r;
}

struct Fns {
    uint64_t create_event, attach_event, get_int32, set_int32, set_size;
};
const Fns& fns() {
    static const Fns f = [] {
        Fns o{};
        o.create_event = module_find_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
        o.attach_event = module_find_symbol("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE");
        o.get_int32 = module_find_symbol("_ZNK2nn5movie13MediaProperty8GetInt32EPiPKc");
        o.set_int32 = module_find_symbol("_ZN2nn5movie13MediaProperty8SetInt32EPKci");
        o.set_size = module_find_symbol("_ZN2nn5movie13MediaProperty7SetSizeEPKcm");
        if (!o.create_event || !o.attach_event || !o.get_int32 || !o.set_int32 || !o.set_size)
            NXLOG_E(kTag, "guest nn::os / nn::movie::MediaProperty functions missing: movies will not play");
        return o;
    }();
    return f;
}

struct Packet {
    std::vector<uint8_t> data;
    int64_t time = 0;
    bool eos = false;
};

struct Picture {   // a decoded picture in host memory (NV12 at its width), or the end of the stream
    std::vector<uint8_t> nv12;
    uint32_t w = 0, h = 0;
    int64_t time = 0;
    bool eos = false;
};

struct Decoder {
    std::mutex mu;
    std::condition_variable cv;              // the worker waits here for packets and for room in `decoded`
    std::thread worker;
    bool stop = false;
    uint64_t generation = 0;                 // Flush bumps it: what the worker decoded before is dropped and dav1d starts again
    int mime = 0;
    std::vector<uint8_t> config;             // the track's av1C record (Configure): its OBUs go to dav1d ahead of the first packet
    int w = 1920, h = 1080, color[3] = {0, 0, 0};
    std::vector<uint64_t> frames, free_;     // registered VideoFrames; free_ is used as a stack
    std::deque<uint64_t> ready;              // frames with a picture, not acquired yet
    std::deque<Packet> queue;                // packets not decoded yet
    std::deque<Picture> decoded;             // pictures waiting for a free frame
    // Frames the player gave back, with the GPU serial at that moment: free once the GPU passed it. Fortune's Weave's CPU-frame path
    // polls one fence (sync[0], main 0x1902870) for every buffer, so it released buffers whose NV12 copy to the textures had not run
    // yet; the next picture written into them showed 3 of 4 pictures early or torn (the choppy cutscenes).
    std::deque<std::pair<uint64_t, uint64_t>> pending_free;
    uint64_t packets = 0, pictures = 0;
    // the movie's clock (skip_late): picture timestamps against the host clock; unit = seconds per timestamp tick, learned from the spacing
    bool clk_on = false;
    int64_t clk_ts0 = 0, clk_last = 0, last_time = 0;
    double clk_t0 = 0, unit = 0;
    uint64_t skipped = 0;
    bool warned_size = false, warned_format = false, warned_mime = false, warned_error = false;
    services::ServiceEvent in_ready, out_ready;   // attached to in_ev / out_ev: any thread can signal them
    alignas(16) uint8_t in_ev[0x100]{}, out_ev[0x100]{}, fmt_ev[0x100]{};
    alignas(16) int32_t scratch[4]{};        // guest-visible out value for MediaProperty::GetInt32
};

Decoder* dec_of(uint64_t self) {
    if (!self || rd64(self + 8) != kMarker) return nullptr;
    return reinterpret_cast<Decoder*>(rd64(self));
}

// I420 to NV12, rows packed at the picture's width, chroma interleaved.
void to_nv12(uint8_t* o, uint32_t w, uint32_t h, const uint8_t* py, ptrdiff_t sy, const uint8_t* pu, const uint8_t* pv, ptrdiff_t suv) {
    const uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    for (uint32_t r = 0; r < h; ++r) std::memcpy(o + static_cast<uint64_t>(r) * w, py + static_cast<ptrdiff_t>(r) * sy, w);
    uint8_t* q = o + static_cast<uint64_t>(w) * h;
    for (uint32_t r = 0; r < ch; ++r) {
        const uint8_t* u = pu + static_cast<ptrdiff_t>(r) * suv;
        const uint8_t* v = pv + static_cast<ptrdiff_t>(r) * suv;
        for (uint32_t x = 0; x < cw; ++x) { *q++ = u[x]; *q++ = v[x]; }
    }
}

uint64_t nv12_size(uint32_t w, uint32_t h) { return static_cast<uint64_t>(w) * h + static_cast<uint64_t>((w + 1) / 2) * 2 * ((h + 1) / 2); }

// A player that shows one picture a frame plays a 60 fps movie at the game's frame rate: Ocarina of Time's AV1 movies ran far below real time
// with their sound ahead while the game drew 10 to 30 frames a second. So pictures go by the clock, as a video player does (nxnative oot2
// 3b588ca6): the timestamps from the first picture against the host clock, and of the waiting pictures the newest one that is due, the late
// ones before it dropped. A picture is late once the next is 50 ms overdue (no slack would make a game one frame behind skip every other
// picture). A stall over a second (a load, a pause) or a timestamp going back (a loop, a new stream) starts the clock again; Flush too.
// A game that keeps up drops nothing. [media] movie_clock = 0 turns it off. The timestamp unit (us or ns) is learned from the spacing.
double host_seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void learn_unit(Decoder& d, int64_t t) {
    if (d.unit == 0 && d.last_time && t > d.last_time) {
        const int64_t dt = t - d.last_time;   // one picture: ~16,667 at 60 fps in us, ~16,666,667 in ns, ~17 in ms
        if (dt >= 2000 && dt <= 200000) d.unit = 1e-6;
        else if (dt >= 2000000 && dt <= 200000000) d.unit = 1e-9;
        else if (dt >= 2 && dt <= 200) d.unit = 1e-3;
        if (d.unit != 0) NXLOG_I(kTag, "movie clock: timestamps in %s (picture spacing %lld)", d.unit == 1e-6 ? "us" : d.unit == 1e-9 ? "ns" : "ms",
                                 static_cast<long long>(dt));
    }
    d.last_time = t;
}

void skip_late(Decoder& d) {   // lock held, at least one decoded picture
    static const bool on = config_get_bool("media", "movie_clock", true);
    if (!on || d.unit == 0 || d.decoded.front().eos) return;
    const double now = host_seconds();
    const int64_t ts = d.decoded.front().time;
    if (!d.clk_on || ts < d.clk_last || now - (d.clk_t0 + static_cast<double>(ts - d.clk_ts0) * d.unit) > 1.0) {
        d.clk_on = true;
        d.clk_ts0 = ts;
        d.clk_t0 = now;
        return;
    }
    while (d.decoded.size() > 1 && !d.decoded[1].eos && d.clk_t0 + static_cast<double>(d.decoded[1].time - d.clk_ts0) * d.unit <= now - 0.05) {
        d.decoded.pop_front();
        if (!d.skipped++) NXLOG_I(kTag, "movie clock: late pictures skipped to keep the movie in time (the game shows fewer pictures a second than the movie has)");
    }
}

// Moves decoded pictures into free frames (caller holds the lock) and wakes the player when one is ready.
void deliver(Decoder& d) {
    bool any = false;
    while (!d.pending_free.empty() && nvn::be::gpu::serial_done(d.pending_free.front().second)) {
        d.free_.push_back(d.pending_free.front().first);
        d.pending_free.pop_front();
    }
    while (!d.decoded.empty() && !d.free_.empty()) {
        skip_late(d);
        Picture& p = d.decoded.front();
        if (!p.eos) {
            learn_unit(d, p.time);
            d.clk_last = p.time;
        }
        const uint64_t f = d.free_.back();
        d.free_.pop_back();
        uint64_t y = 0, uv = 0;
        if (!p.eos) {
            y = static_cast<uint64_t>(p.w) * p.h;
            uv = nv12_size(p.w, p.h) - y;
            const uint64_t buf = rd64(f + kVfBuf), cap = rd64(f + kVfSize);
            if (buf && y + uv <= cap) std::memcpy(reinterpret_cast<void*>(buf), p.nv12.data(), y + uv);
            else {
                if (!d.warned_size) NXLOG_W(kTag, "a %ux%u picture (%llu bytes) does not fit its frame's %llu-byte buffer: frames left empty", p.w, p.h,
                                            static_cast<unsigned long long>(y + uv), static_cast<unsigned long long>(cap));
                d.warned_size = true;
                y = uv = 0;
            }
        }
        wr64(f + kVfFilled, y);
        wr64(f + kVfFilled + 8, uv);
        wr64(f + kVfTime, static_cast<uint64_t>(p.time));
        wr8(f + kVfEos, p.eos ? 1 : 0);
        d.ready.push_back(f);
        d.decoded.pop_front();
        any = true;
    }
    if (any) {
        d.out_ready.signal();
        d.cv.notify_all();   // room in `decoded` again
    }
}

// Decodes one packet into pictures (worker thread, without the lock). `av1` is the worker's own context.
void decode_packet(Decoder& d, Dav1dContext* av1, const Packet& k, std::vector<Picture>& out) {
    Dav1dData in{};
    uint8_t* b = dav1d_data_create(&in, k.data.size());
    if (!b) {
        NXLOG_E(kTag, "out of memory for a %zu-byte packet", k.data.size());
        return;
    }
    std::memcpy(b, k.data.data(), k.data.size());
    in.m.timestamp = k.time;
    for (int guard = 0; in.sz && guard < 64; ++guard) {
        const int r = dav1d_send_data(av1, &in);
        if (r < 0 && r != DAV1D_ERR(EAGAIN)) {
            if (!d.warned_error) NXLOG_E(kTag, "AV1 decode error %d (packet at %lld): its data is dropped", r, static_cast<long long>(k.time));
            d.warned_error = true;
            break;
        }
        for (;;) {
            Dav1dPicture p{};
            if (dav1d_get_picture(av1, &p) < 0) break;
            if (p.p.bpc == 8 && p.p.layout == DAV1D_PIXEL_LAYOUT_I420) {
                Picture pic;
                pic.w = static_cast<uint32_t>(p.p.w);
                pic.h = static_cast<uint32_t>(p.p.h);
                pic.time = p.m.timestamp;
                pic.nv12.resize(nv12_size(pic.w, pic.h));
                to_nv12(pic.nv12.data(), pic.w, pic.h, static_cast<const uint8_t*>(p.data[0]), p.stride[0], static_cast<const uint8_t*>(p.data[1]),
                        static_cast<const uint8_t*>(p.data[2]), p.stride[1]);
                out.push_back(std::move(pic));
            } else {
                if (!d.warned_format) NXLOG_W(kTag, "a picture dropped: %d bits, layout %d (only 4:2:0 8-bit is converted)", p.p.bpc, static_cast<int>(p.p.layout));
                d.warned_format = true;
            }
            dav1d_picture_unref(&p);
        }
    }
    if (in.sz) dav1d_data_unref(&in);
}

Dav1dContext* open_av1(Decoder& d, const std::vector<uint8_t>& config) {
    Dav1dSettings s;
    dav1d_default_settings(&s);
    s.n_threads = g_threads;
    s.max_frame_delay = 1;   // a picture per packet: the player's output event wants no decoder delay
    Dav1dContext* av1 = nullptr;
    if (dav1d_open(&av1, &s) < 0) {
        NXLOG_E(kTag, "dav1d failed to start");
        return nullptr;
    }
    if (config.size() > 4) {   // av1C: 4 bytes, then the sequence header OBU
        Packet k;
        k.data.assign(config.begin() + 4, config.end());
        std::vector<Picture> none;
        decode_packet(d, av1, k, none);
    }
    return av1;
}

void worker_main(Decoder* d) {
    Dav1dContext* av1 = nullptr;
    uint64_t gen = d->generation;
    std::unique_lock<std::mutex> lk(d->mu);
    for (;;) {
        const auto can_work = [&] { return d->stop || (!d->queue.empty() && d->decoded.size() < kAhead); };
        while (!can_work()) {
            if (d->pending_free.empty()) d->cv.wait(lk);
            else {   // frames wait for the GPU: look again shortly (deliver frees them and wakes the player)
                d->cv.wait_for(lk, std::chrono::milliseconds(1));
                if (!d->decoded.empty()) deliver(*d);
            }
        }
        if (d->stop) break;
        if (gen != d->generation) {
            if (av1) dav1d_close(&av1);
            av1 = nullptr;
            gen = d->generation;
        }
        Packet k = std::move(d->queue.front());
        d->queue.pop_front();
        const bool reopen = !av1 && !k.eos && d->mime == kMimeAv1;
        const std::vector<uint8_t> config = reopen ? d->config : std::vector<uint8_t>{};
        lk.unlock();
        std::vector<Picture> out;
        if (reopen) av1 = open_av1(*d, config);
        if (k.eos) {
            out.emplace_back();
            out.back().eos = true;
        } else if (av1) {
            LARGE_INTEGER t0, t1, hz;
            QueryPerformanceCounter(&t0);
            decode_packet(*d, av1, k, out);
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&hz);
            const uint64_t us = static_cast<uint64_t>((t1.QuadPart - t0.QuadPart) * 1000000 / hz.QuadPart);
            s_dec_us += us;
            s_dec_n++;
            for (uint64_t m = s_dec_max_us.load(); us > m && !s_dec_max_us.compare_exchange_weak(m, us);) {}
        } else if (d->mime != kMimeAv1 && !d->warned_mime) {
            NXLOG_W(kTag, "video of MIME type %d is not decoded on the host (AV1 only): pictures left out", d->mime);
            d->warned_mime = true;
        }
        lk.lock();
        if (gen == d->generation) {
            for (Picture& p : out) {
                if (!p.eos) {
                    ++d->pictures;
                    s_pictures++;
                }
                d->decoded.push_back(std::move(p));
            }
            deliver(*d);
            if (d->queue.empty()) d->in_ready.signal();
        }
    }
    if (av1) dav1d_close(&av1);
}

void stop_worker(Decoder& d) {
    {
        std::lock_guard<std::mutex> lk(d.mu);
        d.stop = true;
    }
    d.cv.notify_all();
    if (d.worker.joinable()) d.worker.join();
}

// Attaches a host event to the guest's SystemEventType (managed handle, auto clear: each wait takes one signal), as vi's vsync event.
void attach_event(::Ctx* c, uint8_t* guest_ev, services::ServiceEvent& host) {
    const uint32_t h = host.new_handle();
    if (!h) {
        NXLOG_E(kTag, "no handle for a decoder event");
        return;
    }
    gcall(c, fns().attach_event, reinterpret_cast<uint64_t>(guest_ev), h, 1, kAutoClear);
}

int prop_int(::Ctx* c, Decoder& d, uint64_t prop, const char* name, int fallback) {
    if (!prop) return fallback;
    d.scratch[0] = 0;
    const uint64_t ok = gcall(c, fns().get_int32, prop, reinterpret_cast<uint64_t>(&d.scratch[0]), reinterpret_cast<uint64_t>(name));
    return (ok & 1) && d.scratch[0] > 0 ? d.scratch[0] : fallback;
}

// What the library's own decoder writes (subsdk2 +0x3400, the names checked in its strings), so the player sizes its frames: NV12.
void write_props(::Ctx* c, const Decoder& d, uint64_t prop) {
    if (!prop) return;
    const Fns& f = fns();
    gcall(c, f.set_int32, prop, reinterpret_cast<uint64_t>("width"), static_cast<uint64_t>(d.w));
    gcall(c, f.set_int32, prop, reinterpret_cast<uint64_t>("height"), static_cast<uint64_t>(d.h));
    gcall(c, f.set_size, prop, reinterpret_cast<uint64_t>("output-buf-size"), static_cast<uint64_t>(d.w) * d.h * 3 / 2);
    gcall(c, f.set_size, prop, reinterpret_cast<uint64_t>("input-buf-size"), 0x400000);
    gcall(c, f.set_int32, prop, reinterpret_cast<uint64_t>("color-standard"), static_cast<uint64_t>(d.color[0]));
    gcall(c, f.set_int32, prop, reinterpret_cast<uint64_t>("color-range"), static_cast<uint64_t>(d.color[1]));
    gcall(c, f.set_int32, prop, reinterpret_cast<uint64_t>("color-transfer"), static_cast<uint64_t>(d.color[2]));
}

// ---- movie::VideoDecoder ---------------------------------------------------------------------------------------------------------------

void vd_ctor(::Ctx* c) {   // (self)
    if (A(0)) { wr64(A(0), 0); wr64(A(0) + 8, 0); }
}

// Finalize and the destructor stop the worker but leave the Decoder (and the guest events in it) allocated: the player may still hold the
// events, and a game makes few decoders.
void retire(uint64_t self, Decoder* d) {
    if (d) {
        stop_worker(*d);
        std::lock_guard<std::mutex> lk(d->mu);
        d->queue.clear();
        d->decoded.clear();
        d->ready.clear();
        d->pending_free.clear();
        d->free_.clear();
        d->frames.clear();
    }
    if (self) { wr64(self, 0); wr64(self + 8, 0); }
}

void vd_dtor(::Ctx* c) {   // (self)
    retire(A(0), dec_of(A(0)));
}

void vd_initialize(::Ctx* c) {   // (self, void* mem, size_t size, MediaMimeType, VideoDecoderOutputFormat, VideoCodecMode)
    const uint64_t self = A(0);
    const int mime = static_cast<int>(A(3)), fmt = static_cast<int>(A(4)), mode = static_cast<int>(A(5));
    if (!self) { RET(0); return; }
    auto* d = new Decoder();
    d->mime = mime;
    wr64(self, reinterpret_cast<uint64_t>(d));
    wr64(self + 8, kMarker);
    attach_event(c, d->in_ev, d->in_ready);
    attach_event(c, d->out_ev, d->out_ready);
    gcall(c, fns().create_event, reinterpret_cast<uint64_t>(d->fmt_ev), kAutoClear, 0);   // the format never changes
    d->worker = std::thread(worker_main, d);
    NXLOG_I(kTag, "VideoDecoder: MIME type %d%s, output format %d, mode %d", mime, mime == kMimeAv1 ? " (AV1)" : "", fmt, mode);
    RET(0);
}

void vd_finalize(::Ctx* c) {   // (self)
    retire(A(0), dec_of(A(0)));
    RET(0);
}

void vd_configure(::Ctx* c) {   // (self, const MediaProperty*, const void* config, size_t)
    Decoder* d = dec_of(A(0));
    if (!d) { RET(0); return; }
    const uint8_t* cfg = reinterpret_cast<const uint8_t*>(A(2));
    const size_t n = A(3);
    const int w = prop_int(c, *d, A(1), "width", 1920), h = prop_int(c, *d, A(1), "height", 1080);
    const int cs = prop_int(c, *d, A(1), "color-standard", 0), cr = prop_int(c, *d, A(1), "color-range", 0), ct = prop_int(c, *d, A(1), "color-transfer", 0);
    {
        std::lock_guard<std::mutex> lk(d->mu);
        d->config.assign(cfg && n ? cfg : nullptr, cfg && n ? cfg + n : nullptr);
        d->w = w;
        d->h = h;
        d->color[0] = cs;
        d->color[1] = cr;
        d->color[2] = ct;
    }
    NXLOG_I(kTag, "VideoDecoder: %dx%d, colour standard %d range %d transfer %d, %zu bytes of codec configuration", w, h, cs, cr, ct, n);
    RET(0);
}

void vd_frame_requirements(::Ctx* c) {   // (self, MediaProperty*): also GetOutputFrameProperties
    if (Decoder* d = dec_of(A(0))) write_props(c, *d, A(1));
    RET(0);
}

void vd_register_frames(::Ctx* c) {   // (self, VideoFrame** frames, int n)
    Decoder* d = dec_of(A(0));
    const uint64_t list = A(1);
    int n = static_cast<int>(A(2));
    if (!d || !list || n <= 0) { RET(0); return; }
    if (static_cast<size_t>(n) > kMaxFrames) {
        NXLOG_W(kTag, "%d frames registered, %zu used", n, kMaxFrames);
        n = static_cast<int>(kMaxFrames);
    }
    std::lock_guard<std::mutex> lk(d->mu);
    d->frames.clear();
    d->free_.clear();
    d->pending_free.clear();
    d->ready.clear();
    for (int i = 0; i < n; ++i) d->frames.push_back(rd64(list + 8ull * i));
    for (int i = n - 1; i >= 0; --i) d->free_.push_back(d->frames[i]);
    deliver(*d);
    if (g_log) NXLOG_I(kTag, "VideoDecoder: %d frames registered", n);
    RET(0);
}

void vd_input_event(::Ctx* c) {   // (self, SystemEventType** out)
    Decoder* d = dec_of(A(0));
    if (A(1)) wr64(A(1), d ? reinterpret_cast<uint64_t>(d->in_ev) : 0);
    RET(0);
}
void vd_output_event(::Ctx* c) {
    Decoder* d = dec_of(A(0));
    if (A(1)) wr64(A(1), d ? reinterpret_cast<uint64_t>(d->out_ev) : 0);
    RET(0);
}
void vd_format_event(::Ctx* c) {   // the format never changes
    Decoder* d = dec_of(A(0));
    if (A(1)) wr64(A(1), d ? reinterpret_cast<uint64_t>(d->fmt_ev) : 0);
    RET(0);
}

void vd_start(::Ctx* c) {   // (self)
    if (Decoder* d = dec_of(A(0))) d->in_ready.signal();
    RET(0);
}

void vd_stop(::Ctx* c) { RET(0); }

void vd_decode_async(::Ctx* c) {   // (self, const nn::movie::Buffer*)
    Decoder* d = dec_of(A(0));
    const uint64_t buf = A(1);
    if (!d || !buf) { RET(0); return; }
    const uint64_t data = rd64(buf + kBufData), n = rd64(buf + kBufFilled);
    const int64_t time = static_cast<int64_t>(rd64(buf + kBufTime));
    const uint32_t flags = rd32(buf + kBufFlags);
    {
        std::lock_guard<std::mutex> lk(d->mu);
        if (n && data) {
            Packet k;
            k.data.assign(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + n);
            k.time = time;
            d->queue.push_back(std::move(k));
            ++d->packets;
            s_packets++;
        }
        if (flags & kBufFlagEos) {
            Packet k;
            k.eos = true;
            d->queue.push_back(std::move(k));
        }
        if (g_log) NXLOG_I(kTag, "DecodeAsync: %llu bytes at %lld%s; queued %zu, decoded %zu, ready %zu, free %zu", static_cast<unsigned long long>(n),
                           static_cast<long long>(time), (flags & kBufFlagEos) ? " (end of stream)" : "", d->queue.size(), d->decoded.size(),
                           d->ready.size(), d->free_.size());
    }
    d->cv.notify_all();
    RET(0);
}

uint32_t acquire(Decoder* d, uint64_t* out, int* idx) {
    std::lock_guard<std::mutex> lk(d->mu);
    if (d->ready.empty()) {
        *out = 0;
        return kResultNoFrame;
    }
    *out = d->ready.front();
    d->ready.pop_front();
    if (!d->ready.empty()) d->out_ready.signal();   // one signal per wait: the next ready frame needs its own
    if (idx) {
        *idx = -1;
        for (size_t i = 0; i < d->frames.size(); ++i)
            if (d->frames[i] == *out) *idx = static_cast<int>(i);
    }
    return 0;
}

void vd_acquire(::Ctx* c) {   // (self, VideoFrame** out)
    Decoder* d = dec_of(A(0));
    uint64_t f = 0;
    const uint32_t r = d ? acquire(d, &f, nullptr) : kResultNoFrame;
    if (A(1)) wr64(A(1), f);
    if (f) {
        s_acquired++;
        if (g_log) NXLOG_I(kTag, "AcquireVideoFrame: %llx at %lld (luma %llu, chroma %llu bytes%s)", static_cast<unsigned long long>(f),
                           static_cast<long long>(rd64(f + kVfTime)), static_cast<unsigned long long>(rd64(f + kVfFilled)),
                           static_cast<unsigned long long>(rd64(f + kVfFilled + 8)), (rd32(f + kVfEos) & 0xff) ? ", end of stream" : "");
    }
    stats_tick();
    RET(r);
}

void vd_acquire_index(::Ctx* c) {   // (self, VideoFrame** out, int* index)
    Decoder* d = dec_of(A(0));
    uint64_t f = 0;
    int idx = -1;
    const uint32_t r = d ? acquire(d, &f, &idx) : kResultNoFrame;
    if (A(1)) wr64(A(1), f);
    if (A(2)) std::memcpy(reinterpret_cast<void*>(A(2)), &idx, 4);
    if (f) s_acquired++;
    RET(r);
}

void vd_release(::Ctx* c) {   // (self, VideoFrame*)
    Decoder* d = dec_of(A(0));
    const uint64_t f = A(1);
    if (!d) { RET(0); return; }
    {
        std::lock_guard<std::mutex> lk(d->mu);
        const bool known = std::find(d->frames.begin(), d->frames.end(), f) != d->frames.end();
        const bool already = std::find(d->free_.begin(), d->free_.end(), f) != d->free_.end();
        const bool pending = std::any_of(d->pending_free.begin(), d->pending_free.end(), [f](const auto& p) { return p.first == f; });
        if (f && known && !already && !pending && d->free_.size() + d->pending_free.size() < kMaxFrames)
            d->pending_free.emplace_back(f, nvn::be::gpu::last_serial());
        s_released++;
        if (g_log) NXLOG_I(kTag, "ReleaseVideoFrame: %llx%s; queued %zu, decoded %zu, ready %zu, free %zu", static_cast<unsigned long long>(f),
                           known ? (already ? " (already free)" : "") : " (not registered)", d->queue.size(), d->decoded.size(), d->ready.size(),
                           d->free_.size());
        deliver(*d);
    }
    RET(0);
}

void vd_flush(::Ctx* c) {   // (self)
    Decoder* d = dec_of(A(0));
    if (!d) { RET(0); return; }
    {
        std::lock_guard<std::mutex> lk(d->mu);
        ++d->generation;
        d->clk_on = false;
        d->last_time = 0;
        d->queue.clear();
        d->decoded.clear();
        d->ready.clear();
        d->pending_free.clear();
        d->free_.clear();
        for (auto it = d->frames.rbegin(); it != d->frames.rend(); ++it) d->free_.push_back(*it);
    }
    d->cv.notify_all();
    if (g_log) NXLOG_I(kTag, "VideoDecoder flushed");
    d->in_ready.signal();
    RET(0);
}

void vd_set_core_mask(::Ctx* c) { (void)c; }
void vd_get_core_mask(::Ctx* c) { RET(1); }
void vd_set_priority(::Ctx* c) { (void)c; }
void vd_get_priority(::Ctx* c) { RET(16); }

struct Import {
    const char* name;
    HleFn fn;
};
const Import kImports[] = {
    {"_ZN5movie12VideoDecoderC1Ev", vd_ctor},
    {"_ZN5movie12VideoDecoderC2Ev", vd_ctor},
    {"_ZN5movie12VideoDecoderD1Ev", vd_dtor},
    {"_ZN5movie12VideoDecoderD2Ev", vd_dtor},
    {"_ZN5movie12VideoDecoder10InitializeEPvmN2nn5movie13MediaMimeTypeENS_24VideoDecoderOutputFormatENS_14VideoCodecModeE", vd_initialize},
    {"_ZN5movie12VideoDecoder8FinalizeEv", vd_finalize},
    {"_ZN5movie12VideoDecoder9ConfigureEPKN2nn5movie13MediaPropertyEPKvm", vd_configure},
    {"_ZN5movie12VideoDecoder25GetVideoFrameRequirementsEPN2nn5movie13MediaPropertyE", vd_frame_requirements},
    {"_ZN5movie12VideoDecoder24GetOutputFramePropertiesEPN2nn5movie13MediaPropertyE", vd_frame_requirements},
    {"_ZN5movie12VideoDecoder18RegisterVideoFrameEPPNS_10VideoFrameEi", vd_register_frames},
    {"_ZN5movie12VideoDecoder18GetInputReadyEventEPPN2nn2os15SystemEventTypeE", vd_input_event},
    {"_ZN5movie12VideoDecoder19GetOutputReadyEventEPPN2nn2os15SystemEventTypeE", vd_output_event},
    {"_ZN5movie12VideoDecoder21GetFormatChangedEventEPPN2nn2os15SystemEventTypeE", vd_format_event},
    {"_ZN5movie12VideoDecoder5StartEv", vd_start},
    {"_ZN5movie12VideoDecoder4StopEv", vd_stop},
    {"_ZN5movie12VideoDecoder11DecodeAsyncEPKN2nn5movie6BufferE", vd_decode_async},
    {"_ZN5movie12VideoDecoder17AcquireVideoFrameEPPNS_10VideoFrameE", vd_acquire},
    {"_ZN5movie12VideoDecoder17AcquireVideoFrameEPPNS_10VideoFrameEPi", vd_acquire_index},
    {"_ZN5movie12VideoDecoder17ReleaseVideoFrameEPNS_10VideoFrameE", vd_release},
    {"_ZN5movie12VideoDecoder5FlushEv", vd_flush},
    {"_ZN5movie12VideoDecoder17SetThreadCoreMaskEm", vd_set_core_mask},
    {"_ZN5movie12VideoDecoder17GetThreadCoreMaskEv", vd_get_core_mask},
    {"_ZN5movie12VideoDecoder17SetThreadPriorityEi", vd_set_priority},
    {"_ZN5movie12VideoDecoder17GetThreadPriorityEv", vd_get_priority},
};

}  // namespace

bool install_movie_decoder_hle() {
    static std::once_flag once;
    std::call_once(once, [] {
        const game::Info& gi = game::info();
        if (gi.platform != game::Platform::Switch2 || !gi.movie_video_decoder) return;
        if (config_get_str("media", "movies", "host") == "skip") {
            NXLOG_I(kTag, "[media] movies = skip: every movie fails to open and is skipped");
            return;
        }
        g_log = config_get_bool("media", "video_decoder_log", false);
        g_threads = static_cast<int>(std::clamp<int64_t>(config_get_int("media", "video_decoder_threads", 4), 1, 16));
        for (const Import& im : kImports) {
            hle_register(im.name, im.fn);
            module_override_symbol(im.name);
        }
        g_enabled = true;
        NXLOG_I(kTag, "movie::VideoDecoder on the host: AV1 through dav1d %s (%d threads), %zu imports routed", dav1d_version(), g_threads,
                sizeof kImports / sizeof kImports[0]);
    });
    return g_enabled;
}

#else   // built without dav1d

bool install_movie_decoder_hle() {
    static bool logged = false;
    if (!logged && game::info().movie_video_decoder) {
        logged = true;
        NXLOG_W("movie", "built without dav1d (NXRT_HAVE_DAV1D): movies are skipped");
    }
    return false;
}

#endif

bool movie_decoder_enabled() { return g_enabled; }

}  // namespace nxrt::media
