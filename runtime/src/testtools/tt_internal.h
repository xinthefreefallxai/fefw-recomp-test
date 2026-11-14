// SPDX-License-Identifier: GPL-3.0-or-later
// Private header of the testtools implementation (tt_core.cpp, tt_game.cpp, tt_runner.cpp, tt_hid.cpp).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nxrt/tt.h"

namespace nxrt::tt::detail {

constexpr uint64_t kDefaultGameBase = 0x7100000000ull;

// ---- tt_game.cpp (memory helpers; the rest forwards to the game's tt::GameProbes) ----
uint64_t game_base();
void set_game_base(uint64_t b);
// The warp sequence of the practice mod (docs/mpr/hooks.md section 8), executed with the live context. false: not possible now (no state manager yet).
bool warp(::Ctx* c, const uint8_t world[16], const uint8_t area[16]);
bool have_state_manager();
// The game's resource loader thread, read from memory by the game's probe (games/<id>; S42). Unknown: the loader is not readable
// (no game globals yet, or a test rig without them). *pending (optional) = the sum of its queue counts when readable.
using LoaderState = ::nxrt::tt::LoaderState;     // nxrt/tt.h
LoaderState loader_state(uint32_t* pending = nullptr);
// @grant: write {amount, capacity} of one EItemType into the power-up array of the player state (CPlayerStateMP1 + 0x2c, 8 bytes per item: amount at +0, capacity at +4); item -1 = every upgrade.
bool grant(int64_t item, int32_t amount, int32_t capacity);
// @call: the guest function at main+offset with arguments (numbers, $gs $ps $mgr $player); false and *err when an argument is bad or the game is not ready.
bool call_guest(::Ctx* c, uint64_t offset, const std::string& args, uint64_t* result, std::string* err);
bool read_mem(uint64_t addr, void* out, size_t n);      // ReadProcessMemory on ourselves: false instead of a fault
bool write_mem(uint64_t addr, const void* in, size_t n);

// ---- tt_core.cpp ----
bool take_shot(const std::string& name, std::string* why_not);
void add_checkpoint(const std::string& label);
const RegionTable* regions();
const std::string& script_path();
bool require_shots();
uint64_t current_frame_hash();
void set_pad(const PadFrame& p);                        // the runner's pad for the frame that just began
uint64_t main_frame_count();
// Scripted key steps: calls the registered key sink (tt::set_key_sink) and tracks which names are down, so the run's end can release them.
// false when there is no sink or the sink refused the name.
bool key_event(const std::string& name, bool down);

// ---- tt_runner.cpp ----
class Runner {
public:
    bool load(const std::string& script_file, const RegionTable* regions, std::string* err);
    // Executes steps for the frame that just began (frame number k, state s, valid when s_valid). Calls end_run on pass/fail.
    void frame(uint64_t k, const GameState& s, bool s_valid, ::Ctx* c);
    PadFrame pad() const;
    bool finished() const { return done_; }
    size_t steps_total() const { return script_.steps.size(); }
    size_t steps_done() const { return idx_; }
    int fail_line() const { return fail_line_; }
    bool has_key_steps() const { return has_keys_ || !live_path_.empty(); }    // the loaded script has at least one key / keydown / keyup step

private:
    Script script_;
    size_t idx_ = 0;
    bool started_ = false, done_ = false, warped_ = false, has_keys_ = false;
    uint64_t shot_frame_ = 0;   // frame of the last @shot: @pass / @fail wait a few frames so the capture is taken before the run ends
    int64_t remaining_ = 0;
    uint64_t start_frame_ = 0;
    uint64_t start_ms_ = 0;
    [[maybe_unused]] double start_igt_ = -1;
    uint64_t next_mash_ms_ = 0, mash_release_ms_ = 0;
    int64_t default_timeout_ = 3600;
    uint64_t held_ = 0, pressing_ = 0;
    int16_t stick_[2][2] = {{0, 0}, {0, 0}};            // persistent [which][x,y]
    bool stick_override_ = false;
    int16_t stick_tmp_[2] = {0, 0};
    int stick_tmp_which_ = 0;
    float mouse_x_ = 0, mouse_y_ = 0;
    double goto_cam_ = 0;                                // @goto: estimated offset between the stick angle and the world heading
    bool goto_have_cam_ = false;
    double goto_theta_ = 0;                              // @goto: the stick angle of the last frame
    int fail_line_ = 0;
    double igt_base_ = -1;                               // @play: the play time at the first readable frame of the step
    // @warp gate (S42): the warp's QuitGame tears the game instance down while the resource loader thread may still be building assets for it,
    // which crashed CParticleStaticRenderState on a material token with no object. The warp is issued only after the loader has been idle for
    // kWarpIdleFrames frames in a row ([test] warp_wait_loader, default on), or after warp_loader_max_ms of host time with a warning.
    static constexpr int kWarpIdleFrames = 10;
    bool warp_wait_loader_ = true, gate_open_ = false, gate_logged_ = false;
    int idle_frames_ = 0;
    uint64_t warp_loader_max_ms_ = 30000, gate_start_ms_ = 0;
    // live mode ([test] live = file, env NXRT_TEST_LIVE): when the script runs out of steps the runner waits for more lines appended to that
    // file (so a person or a driver can play the game step by step), writing <file>.status (JSON, one line) each time it goes idle.
    std::string live_path_;
    const RegionTable* regions_ = nullptr;
    size_t live_off_ = 0;
    int live_lines_ = 0;
    bool poll_live(uint64_t k, const GameState& s, bool valid);       // true when new steps were appended
    bool write_live_status(uint64_t k, const GameState& s, bool valid, const char* note);
    void advance() { idx_++; started_ = false; remaining_ = 0; }
    void fail(const Step& st, const std::string& msg);
    bool eval_assert(const Step& st, const GameState& s, bool valid, std::string* detail);
};

}  // namespace nxrt::tt::detail

namespace nxrt::tt::detail {
bool dump_objects(const std::string& classes, const std::string& out_path, std::string* err);   // the game's probes: @objs
bool probe_positions(::Ctx* c, const std::string& in_path, const std::string& out_path, std::string* err);   // the game's probes: @probe
bool poke_memory(const std::string& in_path, std::string* err);   // tt_game.cpp: @poke
bool peek_memory(const std::string& in_path, const std::string& out_path, std::string* err);   // tt_game.cpp: @peek
bool cast_rays(::Ctx* c, const std::string& in_path, const std::string& out_path, std::string* err);   // the game's probes: @rays
bool watch_start(::Ctx* c, int what);      // the game's probes: arm the debug-register write watch (1 vel.y, 2 pos.y, 3 both); false when the player is not readable yet
void watch_stop();                          // disarm and log the hits
void watch_flush(uint64_t frame);           // log the hits recorded since the last call (once per frame)
bool give_item(int item, int amount);      // the game's probes: writes amount and capacity of one item of CPlayerStateMP1
// ---- tt_hid.cpp: the hid adapter (a no-op when S3c's input_source.h is not in the tree) ----
void install_input();                      // (re)installs the wrapping source when the hid service's source changed; called every frame
bool sample_live(PadFrame* out);           // the pad of the live source (the one installed before ours); false when there is none
// pad for the sampler thread (override_live: replay/script replaces the live source; merge_live: with override_live, the live pad's buttons are
// ORed into player 1's and a stick whose scripted value is (0, 0) takes the live stick. Used when a script has key steps.)
void publish_pad(const PadFrame& p, bool override_live, bool merge_live = false);
}  // namespace nxrt::tt::detail
