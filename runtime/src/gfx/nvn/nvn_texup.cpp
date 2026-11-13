// SPDX-License-Identifier: GPL-3.0-or-later
// Background upgrade pool of the BC7 disk cache. See nvn_texup.h.
// Settings: [gfx] astc_upgrade_threads (default max(1, min(2, cpus / 8)); 0 turns the upgrade off), [gfx] astc_upgrade_mb (memory of waiting and running
// jobs, default 192), [gfx] texture_eco (efficiency mode for texture transcodes, default true).
#include "nvn_texup.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include <nxrt/config.h>
#include <nxrt/log.h>

namespace nxrt::nvn::be::texup {
namespace {

constexpr const char* kTag = "nvn.texup";

struct Job {
    uint64_t key = 0, bytes = 0;
    Work work;
};

// Never destroyed: workers can still be inside a job while the process exits, and a destroyed mutex or thread object under them would crash the exit.
struct State {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Job> q;
    std::unordered_set<uint64_t> seen;
    std::vector<std::thread> threads;
    std::atomic<bool> stop{false};
    uint64_t held_bytes = 0, budget = 0;
    int want_threads = -1;   // -1: not read yet
    Stats st;
    std::chrono::steady_clock::time_point last_log = std::chrono::steady_clock::now();
    bool warned_budget = false;
};

State& S() {
    static State* s = new State();
    return *s;
}

std::atomic<int> g_first_pass{0};   // texprep transcodes in flight (first_pass_begin / _end)

void log_totals_locked(State& s, const char* what) {
    s.last_log = std::chrono::steady_clock::now();
    NXLOG_I(kTag, "%s: %llu high-quality BC7 textures stored, %llu failed or stopped, %zu waiting (%.1f MB held), %llu left for a later run (queue full)", what,
            static_cast<unsigned long long>(s.st.done), static_cast<unsigned long long>(s.st.failed), s.q.size(), static_cast<double>(s.held_bytes) / 1048576.0,
            static_cast<unsigned long long>(s.st.dropped_budget));
}

void worker_main(int index) {
    log_set_thread_name("nvn-texup");
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    set_thread_eco(true);
    (void)index;
    State& s = S();
    std::unique_lock<std::mutex> lk(s.mu);
    for (;;) {
        s.cv.wait(lk, [&] { return s.stop.load() || !s.q.empty(); });
        if (s.stop.load()) return;
        // The first pass has the cores while it runs (a new area: hundreds of textures still drawn as stand-ins); look again every 100 ms
        if (g_first_pass.load(std::memory_order_relaxed) > 0) {
            s.cv.wait_for(lk, std::chrono::milliseconds(100), [&] { return s.stop.load(); });
            continue;
        }
        Job j = std::move(s.q.front());
        s.q.pop_front();
        lk.unlock();
        bool ok = false;
        try {
            ok = j.work(s.stop);
        } catch (...) {
            ok = false;
        }
        j.work = nullptr;   // frees the snapshot the closure holds
        lk.lock();
        s.held_bytes -= std::min(s.held_bytes, j.bytes);
        if (ok) s.st.done++;
        else s.st.failed++;
        if (std::chrono::steady_clock::now() - s.last_log > std::chrono::seconds(60)) log_totals_locked(s, "texture cache upgrade");
    }
}

}  // namespace

namespace {
// Caller holds mu.
void read_settings_locked(State& s) {
    if (s.want_threads >= 0) return;
    const int cpus = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    // Polish nothing waits for: one thread up to 15 hardware threads, two from 16 (it was up to four, which with the first pass kept a laptop's
    // cores busy for minutes after each new area, #122)
    s.want_threads = static_cast<int>(std::clamp<int64_t>(config_get_int("gfx", "astc_upgrade_threads", std::max(1, std::min(2, cpus / 8))), 0, 16));
    s.budget = static_cast<uint64_t>(std::max<int64_t>(8, config_get_int("gfx", "astc_upgrade_mb", 192))) << 20;
    if (s.want_threads > 0)
        NXLOG_I(kTag, "texture cache upgrade: %d low-priority thread(s), up to %llu MB of waiting work", s.want_threads, static_cast<unsigned long long>(s.budget >> 20));
}
}  // namespace

void first_pass_begin() { g_first_pass.fetch_add(1, std::memory_order_relaxed); }
void first_pass_end() { g_first_pass.fetch_sub(1, std::memory_order_relaxed); }

void set_thread_eco(bool on) {
#ifdef _WIN32
    static const bool eco = config_get_bool("gfx", "texture_eco", true);
    if (!eco) return;
    THREAD_POWER_THROTTLING_STATE st = {};
    st.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = on ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &st, sizeof st);
#else
    (void)on;
#endif
}

bool wants(uint64_t key) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    read_settings_locked(s);
    return s.want_threads > 0 && !s.stop.load() && !s.seen.count(key);
}

bool submit(uint64_t key, uint64_t bytes, Work work) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    read_settings_locked(s);
    if (s.want_threads == 0 || s.stop.load() || !work) return false;
    if (s.seen.count(key)) return false;
    if (s.held_bytes + bytes > s.budget) {
        s.st.dropped_budget++;
        if (!s.warned_budget) {
            s.warned_budget = true;
            NXLOG_I(kTag, "texture cache upgrade: the queue is full (%.0f MB); textures that do not fit are upgraded in a later run", static_cast<double>(s.budget) / 1048576.0);
        }
        return false;
    }
    s.seen.insert(key);
    s.held_bytes += bytes;
    s.st.submitted++;
    s.q.push_back(Job{key, bytes, std::move(work)});
    if (static_cast<int>(s.threads.size()) < s.want_threads)
        s.threads.emplace_back(worker_main, static_cast<int>(s.threads.size()));
    s.cv.notify_one();
    return true;
}

void shutdown() {
    State& s = S();
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.threads.empty() && s.q.empty() && s.st.submitted == 0) return;
        s.stop.store(true);
        threads.swap(s.threads);
        s.q.clear();
        s.held_bytes = 0;
    }
    s.cv.notify_all();
    for (std::thread& t : threads)
        if (t.joinable()) t.join();
    std::lock_guard<std::mutex> lk(s.mu);
    log_totals_locked(s, "texture cache upgrade stopped");
    s.stop.store(false);   // a later submit starts the pool again (tests)
    s.seen.clear();
    s.st = Stats();
    s.warned_budget = false;
}

Stats stats() {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    Stats r = s.st;
    r.waiting = s.q.size();
    return r;
}

}  // namespace nxrt::nvn::be::texup
