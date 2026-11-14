// SPDX-License-Identifier: GPL-3.0-or-later
// hid service (IHidServer, S3c): the applet resource, the shared memory the SDK reads controller state from, the sampling that
// fills it from nxrt::hid::input_source(), and the commands of nn::hid::IHidServer.
// Adapted from Ryujinx Ryujinx.HLE/HOS/Services/Hid (IHidServer.cs: command ids, argument order and results; HidDevices/NpadDevices.cs:
// pad setup and lifo writes; Hid.cs UpdateStickButtons: the stick threshold; HidServer/HidUtils.cs: NpadIdType mapping; Types/SharedMemory:
// layouts; MIT License) and from ScarletNX src/nx/hle/services/hid.swipc (the command table; GPL-3.0-or-later).
// Command semantics: Switchbrew "HID services" (https://switchbrew.org/wiki/HID_services).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "hid_shm.h"
#include "nxrt/game.h"
#include "nxrt/gauge.h"
#include "nxrt/config.h"
#include "nxrt/input_source.h"
#include "nxrt/jc2_mouse.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "services_internal.h"

namespace nxrt::services {

namespace {

using namespace hidshm;
using nxrt::hid::InputSnapshot;
using nxrt::hid::NpadInput;
using nxrt::hid::NpadStyle;
using nxrt::hid::StickValue;

constexpr uint32_t kNpadIdOther = 0x10;       // NpadIdType (Switchbrew): players 0..7, other 0x10, handheld 0x20
constexpr uint32_t kNpadIdHandheld = 0x20;
constexpr uint32_t kDefaultStyles = kTagFullKey | kTagHandheld | kTagJoyDual | kTagJoyLeft | kTagJoyRight;
constexpr uint32_t kDefaultIdMask = 0x1FF;    // shared-memory entries 0..8: players 1..8 and handheld (Ryujinx MaxControllers 9)
constexpr int32_t kStickButtonThreshold = 32767 / 2;    // Ryujinx Hid.cs: short.MaxValue / 2
constexpr uint64_t kSampleNs = 5000000;       // 5 ms between samples (200 Hz), in ns (ScarletNX hid_motion.h dt_ns)
constexpr uint32_t kResultInvalidNpadId = res(202, 123);       // Ryujinx ResultCode.InvalidNpadIdType
constexpr uint32_t kResultInvalidDeviceIndex = res(202, 124);  // Ryujinx ResultCode.InvalidDeviceIndex
constexpr uint32_t kResultNoHandle = kernel::kOutOfHandles;    // the handle table is full

struct VibValue {
    float amp_low = 0, freq_low = 0, amp_high = 0, freq_high = 0;   // Switchbrew VibrationValue field order
};

// ---- Joy-Con 2 mouse (Nmouse / Jcms) ------------------------------------------------------------------------------------------------
// Layout from the SDK 20.5 client (sdk nn::hid::detail::NmouseTransferMemoryAccessor, GetJcmsStates); docs/hid_mouse_sw2.md has the
// addresses. Both blocks are the game's own buffers, lent as transfer memory (InitializeNmouse 3139, InitializeJcms 3110); the server
// owns their contents and the game reads them directly.
constexpr uint32_t kNmSlots = 17;              // handle table entries, info entries and rings
constexpr uint64_t kNmHandles = 0x0;           // 17 x {u64 handle, u8 valid, pad} (16 bytes)
constexpr uint64_t kNmRings = 0x220;           // 17 rings of 0x818 (0x20 header + 17 x (u64 seq, 0x70 state))
constexpr uint64_t kNmRingSize = 0x818;
constexpr uint64_t kNmInfo = 0x8C40;           // 17 x 0x1C: u32 NmouseType set, u8 valid, u32 NpadIdType, u8 valid, u8 pointer enabled
constexpr uint64_t kNmInfoSize = 0x1C;
constexpr uint64_t kNmShowCursor = 0x8E68;     // u8 IsEnabledShowingCursor
constexpr uint64_t kNmMinSize = 0x8E70;
constexpr uint64_t kJcmsRingSize = 0x2C8;      // per JcmsHandle index (npad * 2 + type): 0x20 header + 17 x (u64 seq, 0x20 state)
constexpr uint64_t kJcmsMinSize = 16 * kJcmsRingSize;
// NmouseType bits (from the client: GetNmouseHandles asks for exactly 2 then exactly 4, the game asks 4 for its "right" side and 2 for
// "left", GetMergedNmouseHandle takes the first entry with bit 0).
[[maybe_unused]] constexpr uint32_t kNmTypeMerged = 1u << 0, kNmTypeJoyLeftJcms = 1u << 1, kNmTypeJoyRightJcms = 1u << 2;
constexpr uint64_t kNmHandleMerged = 0x4E4D0001, kNmHandleRight = 0x4E4D0002;   // opaque, only compared
constexpr uint32_t kNmAttrConnected = 1u << 1;  // the game counts frames with this bit to switch to mouse controls

// The server-side Nmouse state (0x70). The public NmouseState (0x30) is bytes 0..0x24, then 0x30 (at 0x24), 0x28, 0x2C
// (sdk NmouseTransferMemoryAccessor::GetNmouseStates 0x7500658980). The SDK's DevNmouseState conversion (sdk 0x7500657dc0) reads
// public +0x18/+0x1C as the pointer range and maps it to 720p/1080p/1440p/4K, so there is no movement delta in this state.
struct NmouseInternal {
    uint64_t unknown0;      // copied out, never read by the game or the SDK
    uint64_t sampling;      // +0x08
    int32_t x, y;           // +0x10 pointer in the pointer range (1920x1080 for this game); the game's cursor reads these
    int32_t range_w, range_h;   // +0x18 the pointer range (InitializeNmouse / SetNmousePointerRanges)
    int32_t wheel_x;        // +0x20 public +0x20; the game sums public +0x20 and +0x24 per sample (clamped to +-600)
    int32_t dev24;          // +0x24 not in the public state
    uint32_t buttons;       // +0x28 MouseButton bits (inferred: no reader in the game or the SDK interprets them)
    uint32_t attributes;    // +0x2C bit 1: the mouse is on its surface (the SDK's dev state turns it into a bool)
    int32_t wheel_y;        // +0x30 public +0x24 (GetWheelDeltaDev returns +0x20 and +0x30)
    uint8_t dev[0x3C];
};
static_assert(sizeof(NmouseInternal) == 0x70, "NmouseInternal");
static_assert(sizeof(Ring<NmouseInternal>) == kNmRingSize, "nmouse lifo");
struct JcmsState {
    uint64_t unknown0;
    uint64_t sampling;      // +0x08
    uint64_t unknown10;     // read by the placement code (non-zero turns the player toward the cursor); left 0
    uint64_t unknown18;
};
static_assert(sizeof(Ring<JcmsState>) == kJcmsRingSize, "jcms lifo");

struct Jc2Feed {
    std::mutex mu;
    nxrt::hid::HostMouse m;
    uint64_t last_ns = 0;
    float wheel_x = 0, wheel_y = 0;     // notches not yet sampled
    float dx = 0, dy = 0;               // motion not yet sampled (pointer-range units)
    float wake = 0;                     // raw counts not yet sampled (the wake test)
    uint32_t presses = 0;               // buttons pressed and not yet sampled
    uint64_t other_seq = 0;             // jc2_mouse_other_input calls
};
Jc2Feed& feed() {
    static Jc2Feed* f = new Jc2Feed();
    return *f;
}
std::atomic<int> g_jc2_mode{-1};          // -1: not read from the config yet
std::atomic<bool> g_jc2_request{false};
std::atomic<bool> g_jc2_listed{false};    // the game set up Nmouse and the device is in its handle table
std::atomic<bool> g_jc2_active{false};    // last sampled "on its surface"
std::atomic<float> g_jc2_wake{8.0f};      // [mouse] jc2_wake: raw counts within kWakeWindowNs that put the mouse on its surface
std::atomic<uint64_t> g_jc2_engages{0}, g_jc2_lifts{0}, g_jc2_recentres{0};
std::atomic<int32_t> g_jc2_x{0}, g_jc2_y{0};
constexpr uint64_t kWakeWindowNs = 250000000ull;

uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The feed is live when play mode sent one in the last 250 ms with the window focused.
bool jc2_feed_live() {
    Jc2Feed& f = feed();
    std::lock_guard<std::mutex> lk(f.mu);
    return f.last_ns && now_ns() - f.last_ns < 250000000ull && f.m.focused;
}

struct HidState {
    std::mutex mu;
    std::optional<ServiceSharedMemory> shm;
    ServiceEvent style_event[kNpadCount];
    uint32_t supported_styles = kDefaultStyles;
    uint32_t supported_index_mask = kDefaultIdMask;
    bool npad_active = false;
    bool debug_pad_active = false, touch_active = false, mouse_active = false, keyboard_active = false;
    bool sixaxis_fusion = false;
    bool vibration_permitted = true;
    bool unintended_home_protection = false;
    uint64_t joy_hold = 0;                  // NpadJoyHoldType: 0 Vertical, 1 Horizontal
    int64_t handheld_activation = 0;
    int64_t npad_communication = 0;
    uint32_t last_tag[kNpadCount] = {};     // style tag each entry had at the last sample
    bool connected[kNpadCount] = {};
    std::map<uint32_t, VibValue> vibration; // last value sent per VibrationDeviceHandle
    // Joy-Con 2 mouse
    uint64_t nm_addr = 0, nm_size = 0;      // Nmouse transfer memory (guest address = host pointer), 0 when not set up
    int32_t nm_w = 1920, nm_h = 1080;       // pointer range from InitializeNmouse
    uint64_t jcms_addr = 0, jcms_size = 0;  // Jcms transfer memory
    uint32_t nm_merged_types = 0;
    uint64_t nm_btn_left = 0, nm_btn_right = 0;   // NmouseButtonConfigJoy{Left,Right}Jcms (8 bytes each), stored only
    int32_t nm_x = -1, nm_y = -1;           // last sampled pointer (-1: none yet)
    float nm_px = 960, nm_py = 540;         // the virtual pointer (pointer range): moved by the mouse, placed by SetNmousePointerPosition
    bool nm_in_use = false;                 // the mouse is "on its surface": set by real mouse use, cleared by any other input (sample_nmouse)
    float nm_wake = 0;                      // raw counts moved since nm_wake_ns (the wake test)
    uint64_t nm_wake_ns = 0;
    uint64_t nm_other_seen = 0;             // the Jc2Feed::other_seq already acted on
    uint32_t nm_logged = 0, jcms_starts = 0, active_logs = 0;
    bool vib214_logged = false;
    std::atomic<int> resources{0};          // live IAppletResource objects
    std::atomic<bool> background{true};     // the thread samples only when this is set
    std::atomic<bool> stop{false};
    std::thread sampler;
};

// Never destroyed: the sampler thread is joined by hid_reset(), so no static destructor has to deal with it.
HidState& st() {
    static HidState* s = new HidState();
    return *s;
}

// ---- ring lifos -----------------------------------------------------------------------------------------------------------------------
template <class T>
Ring<T>* ring_at(uint8_t* base, uint64_t off) {
    return reinterpret_cast<Ring<T>*>(base + off);
}

void set_sampling(NpadCommonState& v, uint64_t n) { v.sampling = n; }
void set_sampling(SixAxisState& v, uint64_t n) { v.sampling = n; }
void set_sampling(TouchScreenState& v, uint64_t n) { v.sampling = n; }
void set_sampling(MouseState& v, uint64_t n) { v.sampling = n; }
void set_sampling(KeyboardState& v, uint64_t n) { v.sampling = n; }
void set_sampling(DebugPadState& v, uint64_t n) { v.sampling = n; }
void set_sampling(NmouseInternal& v, uint64_t n) { v.sampling = n; }
void set_sampling(JcmsState& v, uint64_t n) { v.sampling = n; }

// Appends one entry: the state's sampling number = the previous one + 1 (the first entry is 1), the header index moves to it, and
// count grows up to kRingMaxCount (Ryujinx RingLifo.Write). The entry's own number (AtomicStorage) is a sequence lock: odd while the
// state is being written, then twice the state's number. The SDK 20 reader (nn::hid::detail::GetNpadStates, sdk+0x5087a0) retries
// while it is odd and while it changed across the copy, and checks that neighbouring states' numbers differ by 1; older readers only
// compare it before and after the copy (libnx _hidGetStates), so the doubled value suits both.
template <class T>
void ring_push(Ring<T>* r, T value) {
    const uint64_t n = r->entries[r->index].sampling / 2 + 1;
    set_sampling(value, n);
    const uint64_t next = (r->index + 1) % kRingEntries;
    std::atomic_ref<uint64_t> seq(r->entries[next].sampling);
    seq.store(2 * n - 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    r->entries[next].state = value;
    seq.store(2 * n, std::memory_order_release);
    std::atomic_ref<uint64_t>(r->index).store(next, std::memory_order_release);
    if (r->count < kRingMaxCount) r->count++;
}

template <class T>
void ring_init(uint8_t* base, uint64_t off) {
    Ring<T>* r = ring_at<T>(base, off);
    r->buffer_count = kRingEntries;   // RingLifo.Create(): the rest is zero
}

// ---- shared memory --------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kCommonLifos[] = {kFullKeyLifo, kHandheldLifo, kJoyDualLifo, kJoyLeftLifo, kJoyRightLifo, kPalmaLifo, kSystemExtLifo};
constexpr uint64_t kSixAxisLifos[] = {kFullKeySixAxisLifo, kHandheldSixAxisLifo, kJoyDualLeftSixAxisLifo, kJoyDualRightSixAxisLifo,
                                      kJoyLeftSixAxisLifo, kJoyRightSixAxisLifo};

// Ryujinx SharedMemory.Create() and NpadState.Create(): every ring gets its buffer count, the npad condition its start values.
void init_shm(uint8_t* host) {
    std::memset(host, 0, kSize);
    ring_init<DebugPadState>(host, kDebugPad);
    ring_init<TouchScreenState>(host, kTouchScreen);
    ring_init<MouseState>(host, kMouse);
    ring_init<KeyboardState>(host, kKeyboard);
    for (uint32_t i = 0; i < kNpadCount; ++i) {
        uint8_t* e = host + kNpads + i * kNpadEntrySize;
        for (uint64_t off : kCommonLifos) ring_init<NpadCommonState>(e, off);
        for (uint64_t off : kSixAxisLifos) ring_init<SixAxisState>(e, off);
    }
    const uint32_t cond[4] = {0, 1, 1, 1};   // NpadCondition: _00 = 0, _04 = 1, hold type Horizontal (1), _0C = 1
    std::memcpy(host + kNpadCondition, cond, sizeof cond);
}

// Creates the shared memory on first use, and again when the kernel region it lived in is gone (tests re-initialise the runtime).
// Caller holds st().mu.
bool shm_ready(HidState& s) {
    if (s.shm && s.shm->ok() && mem::is_mapped(s.shm->host(), kSize)) return true;
    s.shm.emplace(kSize, "hid");
    if (!s.shm->ok()) {
        s.shm.reset();
        return false;
    }
    init_shm(reinterpret_cast<uint8_t*>(s.shm->host()));
    return true;
}

// Always through host(): it moves to the guest's view once the guest maps the object. Caller holds st().mu.
uint8_t* host_of(HidState& s) {
    if (!shm_ready(s)) return nullptr;
    return reinterpret_cast<uint8_t*>(s.shm->host());
}

void w32(uint8_t* p, uint64_t off, uint32_t v) { std::memcpy(p + off, &v, 4); }
void w64(uint8_t* p, uint64_t off, uint64_t v) { std::memcpy(p + off, &v, 8); }

// ---- mapping of ids and styles ---------------------------------------------------------------------------------------------------------
// Shared-memory entry index of a NpadIdType (-1 when invalid).
int index_of_npad_id(uint32_t id) {
    if (id < 8) return static_cast<int>(id);
    if (id == kNpadIdHandheld) return 8;
    if (id == kNpadIdOther) return 9;
    return -1;
}
// The input slot (input_source.h) that feeds a shared-memory entry.
uint32_t slot_of_index(uint32_t idx) {
    if (idx < 8) return idx;
    return idx == 8 ? nxrt::hid::kNpadSlotHandheld : nxrt::hid::kNpadSlotOther;
}
uint32_t tag_of_style(NpadStyle s) {
    switch (s) {
        case NpadStyle::FullKey: return kTagFullKey;
        case NpadStyle::Handheld: return kTagHandheld;
        case NpadStyle::JoyDual: return kTagJoyDual;
        case NpadStyle::JoyLeft: return kTagJoyLeft;
        case NpadStyle::JoyRight: return kTagJoyRight;
        default: return 0;
    }
}
uint64_t common_lifo_of_tag(uint32_t tag) {
    switch (tag) {
        case kTagFullKey: return kFullKeyLifo;
        case kTagHandheld: return kHandheldLifo;
        case kTagJoyDual: return kJoyDualLifo;
        case kTagJoyLeft: return kJoyLeftLifo;
        case kTagJoyRight: return kJoyRightLifo;
        default: return 0;
    }
}
bool six_lifo_of_tag(uint32_t tag, uint64_t off) {
    switch (tag) {
        case kTagFullKey: return off == kFullKeySixAxisLifo;
        case kTagHandheld: return off == kHandheldSixAxisLifo;
        case kTagJoyDual: return off == kJoyDualLeftSixAxisLifo || off == kJoyDualRightSixAxisLifo;
        case kTagJoyLeft: return off == kJoyLeftSixAxisLifo;
        case kTagJoyRight: return off == kJoyRightSixAxisLifo;
        default: return false;
    }
}
uint32_t attributes_of_tag(uint32_t tag) {
    uint32_t a = kAttrConnected;
    switch (tag) {
        case kTagFullKey:
        case kTagHandheld: a |= kAttrWired; break;
        case kTagJoyDual: a |= kAttrLeftConnected | kAttrRightConnected; break;
        case kTagJoyLeft: a |= kAttrLeftConnected; break;
        case kTagJoyRight: a |= kAttrRightConnected; break;
        default: break;
    }
    return a;
}

// Ryujinx Hid.cs UpdateStickButtons: a stick past half its range sets the direction bit (Dy > threshold is Up).
uint64_t stick_buttons(const StickValue& l, const StickValue& r) {
    using namespace nxrt::hid;
    uint64_t b = 0;
    if (l.x < -kStickButtonThreshold) b |= btn(kBtnStickLLeft);
    if (l.x > kStickButtonThreshold) b |= btn(kBtnStickLRight);
    if (l.y < -kStickButtonThreshold) b |= btn(kBtnStickLDown);
    if (l.y > kStickButtonThreshold) b |= btn(kBtnStickLUp);
    if (r.x < -kStickButtonThreshold) b |= btn(kBtnStickRLeft);
    if (r.x > kStickButtonThreshold) b |= btn(kBtnStickRRight);
    if (r.y < -kStickButtonThreshold) b |= btn(kBtnStickRDown);
    if (r.y > kStickButtonThreshold) b |= btn(kBtnStickRUp);
    return b;
}

// Ryujinx NpadDevices.SetupNpad: what a pad of this style looks like the moment it appears (or leaves). tag 0 clears the pad.
void apply_style(uint8_t* e, uint32_t tag, const NpadInput& in) {
    w32(e, kStyleSet, tag);
    if (tag == 0) {
        w32(e, kDeviceType, 0);
        w64(e, kSystemProperties, 0);
        return;
    }
    w32(e, kJoyAssignmentMode, (tag == kTagJoyLeft || tag == kTagJoyRight) ? 1 : 0);   // Single for the half pads, else Dual
    w32(e, kFullKeyColor + 0, 0);
    w32(e, kFullKeyColor + 4, in.color_body);
    w32(e, kFullKeyColor + 8, in.color_buttons);
    w32(e, kJoyColor + 0, 0);
    w32(e, kJoyColor + 4, in.color_body);
    w32(e, kJoyColor + 8, in.color_buttons);
    w32(e, kJoyColor + 12, in.color_body);
    w32(e, kJoyColor + 16, in.color_buttons);
    // NpadSystemProperties: powered Joy-Con bits 3..5 always; ABXY/Plus/Minus for the full pads, SL/SR and one of Plus/Minus for halves.
    uint64_t props = (1ull << 3) | (1ull << 4) | (1ull << 5);
    constexpr uint64_t kAbxy = 1ull << 11, kSlSr = 1ull << 12, kPlus = 1ull << 13, kMinus = 1ull << 14;
    uint32_t device = 0;
    switch (tag) {
        case kTagFullKey: props |= kAbxy | kPlus | kMinus; device = 1u << 0; break;
        case kTagHandheld: props |= kAbxy | kPlus | kMinus; device = (1u << 2) | (1u << 3); break;
        case kTagJoyDual: props |= kAbxy | kPlus | kMinus; device = (1u << 4) | (1u << 5); break;
        case kTagJoyLeft: props |= kSlSr | kMinus; device = 1u << 4; break;
        case kTagJoyRight: props |= kSlSr | kPlus; device = 1u << 5; break;
        default: break;
    }
    w32(e, kDeviceType, device);
    w64(e, kSystemProperties, props);
    w32(e, kBatteryJoyDual, 4);   // NpadBatteryLevel.Percent100
    w32(e, kBatteryJoyLeft, 4);
    w32(e, kBatteryJoyRight, 4);
}

// The stick values are the source's; the direction bits are added where the source left them clear.
void sample_npad(HidState& s, uint8_t* host, uint32_t idx, const InputSnapshot& snap, bool docked) {
    uint8_t* e = host + kNpads + idx * kNpadEntrySize;
    const NpadInput& in = snap.npad[slot_of_index(idx)];
    const uint32_t wanted = tag_of_style(in.style);
    // The handheld entry is only live in handheld mode (Ryujinx Remap); a style must be in the supported set and the id supported.
    const bool on = in.connected && wanted != 0 && (s.supported_styles & wanted) && ((s.supported_index_mask >> idx) & 1) &&
                    !(idx == 8 && docked);
    const uint32_t tag = on ? wanted : 0;
    if (tag != s.last_tag[idx]) {
        apply_style(e, tag, in);
        s.last_tag[idx] = tag;
        s.style_event[idx].signal();
    }
    s.connected[idx] = on;

    NpadCommonState real{};
    SixAxisState six{};
    if (on) {
        real.buttons = in.buttons | stick_buttons(in.left, in.right);
        real.lx = in.left.x;
        real.ly = in.left.y;
        real.rx = in.right.x;
        real.ry = in.right.y;
        real.attributes = attributes_of_tag(tag);
        six.delta_time = kSampleNs;
        six.attributes = 1;   // connected
        for (int k = 0; k < 3; ++k) {
            six.accel[k] = in.six_axis.accel[k];
            six.gyro[k] = in.six_axis.gyro[k];
            six.angle[k] = in.six_axis.angle[k];
        }
        for (int k = 0; k < 9; ++k) six.direction[k] = in.six_axis.direction[k];
    }
    // The pad's own style lifo gets the state and SystemExt mirrors it; every other common lifo gets an empty entry, so all of them keep
    // counting (Ryujinx NpadDevices.UpdateInput). A disconnected pad writes empty entries everywhere (attributes cleared).
    for (uint64_t off : kCommonLifos) {
        const bool pick = on && (off == common_lifo_of_tag(tag) || off == kSystemExtLifo);
        ring_push(ring_at<NpadCommonState>(e, off), pick ? real : NpadCommonState{});
    }
    for (uint64_t off : kSixAxisLifos) {
        const bool pick = on && six_lifo_of_tag(tag, off);
        ring_push(ring_at<SixAxisState>(e, off), pick ? six : SixAxisState{});
    }
}

void sample_touch(uint8_t* host, const nxrt::hid::TouchInput& t) {
    TouchScreenState ts{};
    const uint32_t n = std::min<uint32_t>(t.count, 16);
    ts.count = static_cast<int32_t>(n);
    for (uint32_t i = 0; i < n; ++i) {
        const nxrt::hid::TouchFinger& f = t.fingers[i];
        TouchState& d = ts.touches[i];
        d.finger = f.id;
        d.x = f.x;
        d.y = f.y;
        d.diameter_x = f.diameter_x;
        d.diameter_y = f.diameter_y;
        d.rotation_angle = static_cast<uint32_t>(f.angle);
    }
    ring_push(ring_at<TouchScreenState>(host, kTouchScreen), ts);
}

void sample_mouse(uint8_t* host, const nxrt::hid::MouseInput& m) {
    MouseState ms{};
    ms.x = m.x;
    ms.y = m.y;
    ms.dx = m.dx;
    ms.dy = m.dy;
    ms.wheel_dx = m.wheel_dx;
    ms.wheel_dy = m.wheel_dy;
    ms.buttons = m.buttons;
    ms.attributes = m.connected ? 2u : 0u;   // bit 1 connected
    ring_push(ring_at<MouseState>(host, kMouse), ms);
}

void sample_keyboard(uint8_t* host, const nxrt::hid::KeyboardInput& k) {
    KeyboardState ks{};
    ks.modifiers = k.modifiers;
    for (int i = 0; i < 4; ++i) ks.keys[i] = k.keys[i];
    ring_push(ring_at<KeyboardState>(host, kKeyboard), ks);
}

// ---- Joy-Con 2 mouse ----------------------------------------------------------------------------------------------------------------
// Lists the virtual devices in the Nmouse block: a merged mouse (slot 0) and a Joy-Con 2 (R) mouse of player 1 (slot 1). Info first, the
// handle's valid byte last, so a reader never sees a valid handle without its info. Caller holds st().mu.
void nm_list_devices(HidState& s) {
    uint8_t* b = reinterpret_cast<uint8_t*>(s.nm_addr);
    struct Dev { uint64_t handle; uint32_t types; bool has_npad; uint32_t npad; };
    const Dev devs[2] = {{kNmHandleMerged, kNmTypeMerged, false, 0}, {kNmHandleRight, kNmTypeJoyRightJcms, true, 0}};
    for (uint32_t i = 0; i < 2; ++i) {
        uint8_t* info = b + kNmInfo + i * kNmInfoSize;
        w32(info, 0x0, devs[i].types);
        info[0x4] = 1;
        w32(info, 0x8, devs[i].npad);
        info[0xC] = devs[i].has_npad ? 1 : 0;
        info[0x10] = 1;   // pointer enabled
        w64(b, kNmHandles + i * 16, devs[i].handle);
        std::atomic_thread_fence(std::memory_order_release);
        b[kNmHandles + i * 16 + 8] = 1;
    }
    g_jc2_listed.store(true);
    NXLOG_I("hid", "Joy-Con 2 mouse: listed a merged mouse and a Joy-Con 2 (R) mouse for player 1 (mode %s, pointer range %dx%d)",
            nxrt::hid::jc2_mouse_mode_name(nxrt::hid::jc2_mouse_mode()), s.nm_w, s.nm_h);
}

// The server's view of the Nmouse block after InitializeNmouse: zeroed, every ring's buffer count set, devices listed unless the mode is
// off. Caller holds st().mu.
void nm_setup(HidState& s) {
    uint8_t* b = reinterpret_cast<uint8_t*>(s.nm_addr);
    std::memset(b, 0, kNmMinSize);
    for (uint32_t i = 0; i < kNmSlots; ++i) ring_init<NmouseInternal>(b, kNmRings + i * kNmRingSize);
    s.nm_x = s.nm_y = -1;
    s.nm_px = static_cast<float>(s.nm_w / 2);
    s.nm_py = static_cast<float>(s.nm_h / 2);
    if (nxrt::hid::jc2_mouse_mode() != nxrt::hid::Jc2MouseMode::Off) nm_list_devices(s);
}

void jcms_setup(HidState& s) {
    uint8_t* b = reinterpret_cast<uint8_t*>(s.jcms_addr);
    std::memset(b, 0, kJcmsMinSize);
    for (uint32_t i = 0; i < 16; ++i) ring_init<JcmsState>(b, i * kJcmsRingSize);
}

// One Nmouse sample from the play layer's feed, as a real Joy-Con 2 (R) mouse would send it.
// - Pointer: the virtual pointer moves by the motion fed since the last sample (pointer-range units, kept within the range) and the game
//   places it with SetNmousePointerPosition. Fortune's Weave puts it back at the centre every frame and reads x - 960, y - 540 as that
//   frame's movement (main 0x19c70a0), so where the PC pointer is on the window does not matter, only how it moves.
// - "On its surface" (attribute bit 1): lifted while the window has no focus (or the F1 menu is up); kept as it is while the feed is
//   late (a load or a stall is no reason to change hands). Auto mode sets it when the mouse really moves (jc2_wake raw counts within
//   0.25 s), clicks or scrolls, and clears it at once on any key or pad input (Jc2Feed::other_seq): the last device used wins and a
//   still or jittery mouse never takes over. The game leaves its mouse controls after its own timer (main 0x19c7024; Fortune's Weave's
//   module cuts it short, fefw_mouse.cpp). On mode keeps the bit set while the window has focus. Motion while the mouse is lifted only
//   counts toward the wake test.
// Caller holds st().mu.
void sample_nmouse(HidState& s, const InputSnapshot&) {
    if (!s.nm_addr || !g_jc2_listed.load()) return;
    const bool mode_off = nxrt::hid::jc2_mouse_mode() == nxrt::hid::Jc2MouseMode::Off;
    const bool on_mode = nxrt::hid::jc2_mouse_mode() == nxrt::hid::Jc2MouseMode::On;
    float dx = 0, dy = 0, wake = 0, wx = 0, wy = 0;
    uint32_t buttons = 0, presses = 0;
    uint64_t other = 0;
    bool focused = false, fresh = false;
    {
        Jc2Feed& f = feed();
        std::lock_guard<std::mutex> lk(f.mu);
        focused = f.last_ns != 0 && f.m.focused;
        fresh = f.last_ns != 0 && now_ns() - f.last_ns < 250000000ull;
        dx = f.dx;
        dy = f.dy;
        wake = f.wake;
        wx = f.wheel_x;
        wy = f.wheel_y;
        presses = f.presses;
        buttons = f.m.buttons;
        other = f.other_seq;
        f.dx = f.dy = f.wake = f.wheel_x = f.wheel_y = 0;
        f.presses = 0;
    }
    const uint64_t t = now_ns();
    const bool other_used = other != s.nm_other_seen;
    s.nm_other_seen = other;
    bool in_use = s.nm_in_use;
    if (mode_off || !focused) {
        in_use = false;          // off, or the window lost focus (or a runtime menu is up)
        s.nm_wake = 0;
    } else if (!fresh) {
        // no feed for a while (the game is loading or stalled): nothing changes hands
    } else if (on_mode) {
        in_use = true;
    } else if (other_used) {
        in_use = false;          // a key or a pad: the mouse is lifted at once
        s.nm_wake = 0;
    } else if (!in_use) {
        if (wake > 0) {
            if (t - s.nm_wake_ns > kWakeWindowNs) s.nm_wake = 0;
            s.nm_wake += wake;
            s.nm_wake_ns = t;
        }
        if (presses || wx != 0 || wy != 0 || s.nm_wake >= g_jc2_wake.load(std::memory_order_relaxed)) {
            in_use = true;
            s.nm_wake = 0;
        }
    }
    if (in_use != s.nm_in_use) (in_use ? g_jc2_engages : g_jc2_lifts).fetch_add(1, std::memory_order_relaxed);
    s.nm_in_use = in_use;
    const bool active = in_use;
    NmouseInternal ns{};
    const int32_t prev_x = s.nm_x, prev_y = s.nm_y;
    if (active) {
        s.nm_px = std::clamp(s.nm_px + dx, 0.f, static_cast<float>(s.nm_w - 1));
        s.nm_py = std::clamp(s.nm_py + dy, 0.f, static_cast<float>(s.nm_h - 1));
        ns.wheel_x = static_cast<int32_t>(std::lround(wx * 120.f));
        ns.wheel_y = static_cast<int32_t>(std::lround(wy * 120.f));
        ns.buttons = buttons;
        ns.attributes = kNmAttrConnected;
    }
    // the pointer keeps its fraction here; the state carries whole units
    ns.x = s.nm_x = static_cast<int32_t>(std::lround(s.nm_px));
    ns.y = s.nm_y = static_cast<int32_t>(std::lround(s.nm_py));
    g_jc2_x.store(ns.x, std::memory_order_relaxed);
    g_jc2_y.store(ns.y, std::memory_order_relaxed);
    ns.range_w = s.nm_w;
    ns.range_h = s.nm_h;
    if (active != g_jc2_active.exchange(active) && s.active_logs < 200) {
        s.active_logs++;
        NXLOG_I("hid", "Joy-Con 2 mouse %s (mode %s)", active ? "on its surface: the game sees the PC mouse" : "lifted",
                nxrt::hid::jc2_mouse_mode_name(nxrt::hid::jc2_mouse_mode()));
    }
    uint8_t* b = reinterpret_cast<uint8_t*>(s.nm_addr);
    for (uint32_t i = 0; i < 2; ++i) ring_push(ring_at<NmouseInternal>(b, kNmRings + i * kNmRingSize), ns);
    const bool moved = active && (ns.x != prev_x || ns.y != prev_y);
    if (active && s.nm_logged < 3 && (moved || ns.buttons || ns.wheel_x || ns.wheel_y)) {
        s.nm_logged++;
        NXLOG_I("hid", "Joy-Con 2 mouse sample %u: pos %d,%d range %dx%d buttons %x wheel %d,%d attr %x (block %llx)", s.nm_logged, ns.x, ns.y,
                ns.range_w, ns.range_h, ns.buttons, ns.wheel_x, ns.wheel_y, ns.attributes, static_cast<unsigned long long>(s.nm_addr));
    }
    if (s.jcms_addr) {
        // Player 1's Jcms states (index npad * 2 + type: 0 left, 1 right) keep counting so GetJcmsState has a current entry.
        for (uint32_t i = 0; i < 2; ++i) ring_push(ring_at<JcmsState>(reinterpret_cast<uint8_t*>(s.jcms_addr), i * kJcmsRingSize), JcmsState{});
    }
}

// The guest buffer behind a transfer memory handle, or 0 when it is unusable for `need` bytes.
uint64_t tm_addr(const ipc::Request& req, uint64_t size, uint64_t need, const char* what) {
    if (req.in_copy.empty()) {
        NXLOG_W("hid", "%s: no transfer memory handle", what);
        return 0;
    }
    auto tm = kernel::handle_as<kernel::KTransferMemory>(req.in_copy[0]);
    if (!tm || !tm->addr) {
        NXLOG_W("hid", "%s: handle %x is not a transfer memory", what, req.in_copy[0]);
        return 0;
    }
    const uint64_t n = std::min(size, tm->size);
    if (n < need || !mem::is_mapped(tm->addr, need)) {
        NXLOG_W("hid", "%s: transfer memory %llx size %llx (request %llx) is smaller than %llx or unmapped", what,
                static_cast<unsigned long long>(tm->addr), static_cast<unsigned long long>(tm->size), static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(need));
        return 0;
    }
    return tm->addr;
}

// InitializeNmouse (3139) and its NmouseStateRange form (3130) share one layout: u8 pointer type @0, s32 width @4, s32 height @8,
// aruid @0x10, size @0x18 and the transfer memory handle (3139 adds a u32, always 1, at @0xC).
uint32_t nm_initialize(ipc::Request& req, const char* what) {
    const uint32_t type = req.arg<uint8_t>(0);
    const int32_t w = req.arg<int32_t>(4), h = req.arg<int32_t>(8);
    const uint64_t size = req.arg<uint64_t>(0x18);
    std::lock_guard<std::mutex> lk(st().mu);
    HidState& s = st();
    s.nm_w = w > 0 ? w : 1920;
    s.nm_h = h > 0 ? h : 1080;
    s.nm_addr = tm_addr(req, size, kNmMinSize, what);
    s.nm_size = s.nm_addr ? size : 0;
    NXLOG_I("hid", "%s(pointer type %u, range %dx%d, size %llx) -> block %llx", what, type, w, h, static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(s.nm_addr));
    if (s.nm_addr) nm_setup(s);
    return 0;
}

// The input for one sample: the source's, or the default pads (player 1 idle FullKey, handheld in handheld mode).
InputSnapshot read_input() {
    InputSnapshot snap;
    if (auto src = nxrt::hid::input_source()) {
        src->poll(snap);
        return snap;
    }
    snap.npad[0].connected = true;
    snap.npad[0].style = NpadStyle::FullKey;
    if (!profile().docked) {
        snap.npad[nxrt::hid::kNpadSlotHandheld].connected = true;
        snap.npad[nxrt::hid::kNpadSlotHandheld].style = NpadStyle::Handheld;
    }
    return snap;
}

void sampler_main(HidState* s) {
    while (!s->stop.load()) {
        if (s->background.load() && s->resources.load() > 0) hid_sample_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Caller holds st().mu.
void ensure_sampler(HidState& s) {
    if (!s.sampler.joinable()) s.sampler = std::thread(sampler_main, &s);
}

// Joy assignment of one entry, written straight into the shared memory (the sampler keeps it until the next style change).
void set_joy_assignment(HidState& s, int idx, uint32_t mode) {
    if (idx < 0) return;
    uint8_t* host = host_of(s);
    if (host) w32(host + kNpads + static_cast<uint64_t>(idx) * kNpadEntrySize, kJoyAssignmentMode, mode);
}

// Decodes a VibrationDeviceHandle / SixAxisSensorHandle: low byte device type, byte 1 NpadIdType, bit 16 position (ScarletNX hid.c,
// Ryujinx VibrationDeviceHandle).
struct DeviceHandle {
    uint32_t device_type, npad_id, position;
};
DeviceHandle decode_handle(uint32_t h) {
    return {h & 0xFF, (h >> 8) & 0xFF, (h >> 16) & 0xFF};
}

// Input slot of a handle's npad, or -1.
int slot_of_handle(uint32_t h) {
    const int idx = index_of_npad_id(decode_handle(h).npad_id);
    return idx < 0 ? -1 : static_cast<int>(slot_of_index(static_cast<uint32_t>(idx)));
}

// Forwards a stored value to the vibration sink: the handle's npad slot and its position (0 left or the only device, 1 right). Handles
// whose npad is invalid or whose position is past 1 are not forwarded. Called without st().mu held.
void forward_vibration(uint32_t h, const VibValue& v) {
    const int slot = slot_of_handle(h);
    const uint32_t hand = decode_handle(h).position;
    if (slot < 0 || hand > 1) return;
    nxrt::hid::VibrationValue out;
    out.amp_low = v.amp_low;
    out.freq_low = v.freq_low;
    out.amp_high = v.amp_high;
    out.freq_high = v.freq_high;
    vibration_dispatch(static_cast<uint32_t>(slot), static_cast<int>(hand), out);
}

// ---- commands ---------------------------------------------------------------------------------------------------------------------------
// A command that is accepted and does nothing (Switchbrew: stubbed, or nothing to do here). Writes `out_bytes` zero bytes as its reply.
void ignore_cmd(ipc::Service& svc, uint32_t id, const char* name, size_t out_bytes = 0) {
    svc.add_cmd(id, name, [name, out_bytes](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        NXLOG_D("hid", "%s: accepted and ignored", name);
        rsp.out.resize(rsp.out.size() + out_bytes, 0);
        return 0;
    });
}

// A command whose handle output is 0 on hardware (Switchbrew: "[10.0.0+] Stubbed, just returns 0").
void zero_handle_cmd(ipc::Service& svc, uint32_t id, const char* name) {
    svc.add_cmd(id, name, [name](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        NXLOG_D("hid", "%s: stubbed, returns handle 0", name);
        rsp.out_copy.push_back(0);
        return 0;
    });
}

// Gauge (include/nxrt/gauge.h): IAppletResource objects alive (the sampler runs while any is).
GaugeCounter g_hid_resources("hid.applet_resources");

// IAppletResource (nn::hid::IAppletResource): one per CreateAppletResource. While one is alive the sampler runs.
class AppletResource : public ipc::Service {
public:
    AppletResource() : ipc::Service("nn::hid::IAppletResource") {
        st().resources++;
        g_hid_resources.add(1);
        add_cmd(0, "GetSharedMemoryHandle", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            HidState& s = st();
            std::lock_guard<std::mutex> lk(s.mu);
            if (!shm_ready(s)) return kernel::kOutOfMemory;
            const uint32_t h = s.shm->new_handle();
            if (!h) return kResultNoHandle;
            rsp.out_copy.push_back(h);
            return 0;
        });
    }
    ~AppletResource() override {
        st().resources--;
        g_hid_resources.sub(1);
    }
};

std::shared_ptr<ipc::Service> make_vibration_list() {
    return make_iface("nn::hid::IActiveVibrationDeviceList", [](ipc::Service& svc) {
        ignore_cmd(svc, 0, "ActivateVibrationDevice");   // the devices are activated and never vibrate (ScarletNX hid.swipc)
    });
}

void setup_hid(ipc::Service& svc) {
    // ---- resource and the device activations
    svc.add_cmd(0, "CreateAppletResource", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        {
            std::lock_guard<std::mutex> lk(st().mu);
            ensure_sampler(st());
        }
        rsp.push_object(std::make_shared<AppletResource>());
        NXLOG_I("hid", "CreateAppletResource");
        return 0;
    });
    svc.add_cmd(1, "ActivateDebugPad", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().debug_pad_active = true;
        return 0;
    });
    svc.add_cmd(11, "ActivateTouchScreen", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().touch_active = true;
        return 0;
    });
    svc.add_cmd(21, "ActivateMouse", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().mouse_active = true;
        return 0;
    });
    svc.add_cmd(31, "ActivateKeyboard", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().keyboard_active = true;
        return 0;
    });
    ignore_cmd(svc, 32, "SendKeyboardLockKeyEvent");
    zero_handle_cmd(svc, 40, "AcquireXpadIdEventHandle");
    ignore_cmd(svc, 51, "ActivateXpad");
    ignore_cmd(svc, 55, "GetXpadIds", 8);
    ignore_cmd(svc, 56, "ActivateJoyXpad");
    zero_handle_cmd(svc, 58, "GetJoyXpadLifoHandle");
    ignore_cmd(svc, 59, "GetJoyXpadIds", 8);

    // ---- six-axis sensors (the samples are in the six-axis lifos; start and stop change nothing, as in Ryujinx)
    ignore_cmd(svc, 60, "ActivateSixAxisSensor");
    ignore_cmd(svc, 61, "DeactivateSixAxisSensor");
    zero_handle_cmd(svc, 62, "GetSixAxisSensorLifoHandle");
    ignore_cmd(svc, 63, "ActivateJoySixAxisSensor");
    ignore_cmd(svc, 64, "DeactivateJoySixAxisSensor");
    zero_handle_cmd(svc, 65, "GetJoySixAxisSensorLifoHandle");
    ignore_cmd(svc, 66, "StartSixAxisSensor");
    ignore_cmd(svc, 67, "StopSixAxisSensor");
    svc.add_cmd(68, "IsSixAxisSensorFusionEnabled", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<uint8_t>(st().sixaxis_fusion ? 1 : 0);
        return 0;
    });
    svc.add_cmd(69, "EnableSixAxisSensorFusion", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().sixaxis_fusion = req.arg<uint8_t>(0) != 0;
        return 0;
    });
    ignore_cmd(svc, 70, "SetSixAxisSensorFusionParameters");
    ignore_cmd(svc, 71, "GetSixAxisSensorFusionParameters", 8);
    ignore_cmd(svc, 72, "ResetSixAxisSensorFusionParameters");
    ignore_cmd(svc, 73, "SetAccelerometerParameters");
    ignore_cmd(svc, 74, "GetAccelerometerParameters", 8);
    ignore_cmd(svc, 75, "ResetAccelerometerParameters");
    ignore_cmd(svc, 76, "SetAccelerometerPlayMode");
    ignore_cmd(svc, 77, "GetAccelerometerPlayMode", 4);
    ignore_cmd(svc, 78, "ResetAccelerometerPlayMode");
    ignore_cmd(svc, 79, "SetGyroscopeZeroDriftMode");
    ignore_cmd(svc, 80, "GetGyroscopeZeroDriftMode", 4);
    ignore_cmd(svc, 81, "ResetGyroscopeZeroDriftMode");
    svc.add_cmd(82, "IsSixAxisSensorAtRest", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        // The handle names the pad; the answer is that pad's at_rest in the current input (true when no such pad).
        bool at_rest = true;
        const int slot = slot_of_handle(req.arg<uint32_t>(0));
        if (slot >= 0) {
            InputSnapshot snap = read_input();
            at_rest = snap.npad[slot].six_axis.at_rest;
        }
        rsp.push<uint8_t>(at_rest ? 1 : 0);
        return 0;
    });
    ignore_cmd(svc, 83, "IsFirmwareUpdateAvailableForSixAxisSensor", 1);
    ignore_cmd(svc, 91, "ActivateGesture");

    // ---- npads
    svc.add_cmd(100, "SetSupportedNpadStyleSet", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().supported_styles = req.arg<uint32_t>(0);
        return 0;
    });
    svc.add_cmd(101, "GetSupportedNpadStyleSet", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<uint32_t>(st().supported_styles);
        return 0;
    });
    svc.add_cmd(102, "SetSupportedNpadIdType", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const ipc::Buffer b = req.in_buf(0);
        const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(b.size / 4), 10);
        std::vector<uint32_t> ids(n);
        if (n) buf_read(b, ids.data(), n * 4ull);
        uint32_t mask = 0;
        for (uint32_t id : ids) {
            const int idx = index_of_npad_id(id);
            if (idx >= 0) mask |= 1u << idx;
        }
        std::lock_guard<std::mutex> lk(st().mu);
        st().supported_index_mask = mask;
        return 0;
    });
    svc.add_cmd(103, "ActivateNpad", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().npad_active = true;
        return 0;
    });
    svc.add_cmd(104, "DeactivateNpad", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().npad_active = false;
        return 0;
    });
    svc.add_cmd(106, "AcquireNpadStyleSetUpdateEventHandle", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const int idx = index_of_npad_id(req.arg<uint32_t>(0));
        if (idx < 0) return kResultInvalidNpadId;
        std::lock_guard<std::mutex> lk(st().mu);
        const uint32_t h = st().style_event[idx].new_handle();
        if (!h) return kResultNoHandle;
        st().style_event[idx].signal();   // games expect it signalled after this call (Ryujinx)
        rsp.out_copy.push_back(h);
        return 0;
    });
    ignore_cmd(svc, 107, "DisconnectNpad");
    svc.add_cmd(108, "GetPlayerLedPattern", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        // Player LED patterns as Ryujinx's table (players 1..8); handheld and other have none.
        static constexpr uint64_t kLed[8] = {0b0001, 0b0011, 0b0111, 0b1111, 0b1001, 0b0101, 0b1101, 0b0110};
        const uint32_t id = req.arg<uint32_t>(0);
        if (id >= 8 && id != kNpadIdHandheld && id != kNpadIdOther) return kResultInvalidNpadId;
        rsp.push<uint64_t>(id < 8 ? kLed[id] : 0);
        return 0;
    });
    svc.add_cmd(109, "ActivateNpadWithRevision", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().npad_active = true;
        return 0;
    });
    svc.add_cmd(120, "SetNpadJoyHoldType", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().joy_hold = req.arg<uint64_t>(8);
        return 0;
    });
    svc.add_cmd(121, "GetNpadJoyHoldType", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<uint64_t>(st().joy_hold);
        return 0;
    });
    svc.add_cmd(122, "SetNpadJoyAssignmentModeSingleByDefault", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        set_joy_assignment(st(), index_of_npad_id(req.arg<uint32_t>(0)), 1);
        return 0;
    });
    svc.add_cmd(123, "SetNpadJoyAssignmentModeSingle", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        set_joy_assignment(st(), index_of_npad_id(req.arg<uint32_t>(0)), 1);
        return 0;
    });
    svc.add_cmd(124, "SetNpadJoyAssignmentModeDual", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        set_joy_assignment(st(), index_of_npad_id(req.arg<uint32_t>(0)), 0);
        return 0;
    });
    ignore_cmd(svc, 125, "MergeSingleJoyAsDualJoy");
    ignore_cmd(svc, 126, "StartLrAssignmentMode");
    ignore_cmd(svc, 127, "StopLrAssignmentMode");
    svc.add_cmd(128, "SetNpadHandheldActivationMode", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().handheld_activation = req.arg<int64_t>(8);
        return 0;
    });
    svc.add_cmd(129, "GetNpadHandheldActivationMode", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<int64_t>(st().handheld_activation);
        return 0;
    });
    ignore_cmd(svc, 130, "SwapNpadAssignment");
    svc.add_cmd(131, "IsUnintendedHomeButtonInputProtectionEnabled", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<uint8_t>(st().unintended_home_protection ? 1 : 0);
        return 0;
    });
    svc.add_cmd(132, "EnableUnintendedHomeButtonInputProtection", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().unintended_home_protection = req.arg<uint8_t>(0) != 0;
        return 0;
    });
    svc.add_cmd(133, "SetNpadJoyAssignmentModeSingleWithDestination", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        // Output: bool "is reassigned" and the new id. Nothing is ever merged here, so it is always false and 0.
        std::lock_guard<std::mutex> lk(st().mu);
        set_joy_assignment(st(), index_of_npad_id(req.arg<uint32_t>(0)), 1);
        rsp.push<uint8_t>(0);
        rsp.push<uint8_t>(0);
        rsp.push<uint8_t>(0);
        rsp.push<uint8_t>(0);
        rsp.push<uint32_t>(0);
        return 0;
    });
    ignore_cmd(svc, 134, "SetNpadAnalogStickUseCenterClamp");

    // ---- vibration
    svc.add_cmd(200, "GetVibrationDeviceInfo", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const DeviceHandle d = decode_handle(req.arg<uint32_t>(0));
        if (index_of_npad_id(d.npad_id) < 0) return kResultInvalidNpadId;
        if (d.position > 1) return kResultInvalidDeviceIndex;
        // Ryujinx: device types 3..7 and 32, 33 (NpadStyleIndex) are linear resonant actuators, 8 is a GC ERM; the position of an LRA is
        // 0 = left, 1 = right. VibrationDeviceType: None 0, LinearResonantActuator 1, GcErm 2. VibrationDevicePosition: None 0, Left 1, Right 2.
        const bool lra = d.device_type == 3 || d.device_type == 4 || d.device_type == 5 || d.device_type == 6 || d.device_type == 7 ||
                         d.device_type == 32 || d.device_type == 33;
        const uint32_t type = lra ? 1 : (d.device_type == 8 ? 2 : 0);
        const uint32_t pos = (lra && d.position <= 1) ? d.position + 1 : 0;
        rsp.push<uint32_t>(type);
        rsp.push<uint32_t>(pos);
        return 0;
    });
    svc.add_cmd(201, "SendVibrationValue", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        // Handle (u32), then the VibrationValue (4 floats) right after it. The aruid that follows is not needed here.
        const uint32_t h = req.arg<uint32_t>(0);
        VibValue v;
        v.amp_low = req.arg<float>(4);
        v.freq_low = req.arg<float>(8);
        v.amp_high = req.arg<float>(12);
        v.freq_high = req.arg<float>(16);
        {
            std::lock_guard<std::mutex> lk(st().mu);
            st().vibration[h] = v;
        }
        forward_vibration(h, v);
        return 0;
    });
    svc.add_cmd(202, "GetActualVibrationValue", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const uint32_t h = req.arg<uint32_t>(0);
        std::lock_guard<std::mutex> lk(st().mu);
        auto it = st().vibration.find(h);
        const VibValue v = it == st().vibration.end() ? VibValue{} : it->second;
        rsp.push<float>(v.amp_low);
        rsp.push<float>(v.freq_low);
        rsp.push<float>(v.amp_high);
        rsp.push<float>(v.freq_high);
        return 0;
    });
    svc.add_cmd(203, "CreateActiveVibrationDeviceList", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_vibration_list());
        return 0;
    });
    svc.add_cmd(204, "PermitVibration", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().vibration_permitted = req.arg<uint8_t>(0) != 0;
        return 0;
    });
    svc.add_cmd(205, "IsVibrationPermitted", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<uint8_t>(st().vibration_permitted ? 1 : 0);
        return 0;
    });
    svc.add_cmd(206, "SendVibrationValues", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        // Two buffers: an array of handles (u32) and an array of VibrationValue (16 bytes each), same entry count.
        const ipc::Buffer hb = req.in_buf(0), vb = req.in_buf(1);
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(std::min<uint64_t>(hb.size / 4, vb.size / 16), 16));
        std::vector<uint32_t> handles(n);
        std::vector<float> vals(n * 4ull);
        if (n) {
            buf_read(hb, handles.data(), n * 4ull);
            buf_read(vb, vals.data(), n * 16ull);
        }
        std::vector<VibValue> values(n);
        for (uint32_t i = 0; i < n; ++i) {
            values[i].amp_low = vals[i * 4 + 0];
            values[i].freq_low = vals[i * 4 + 1];
            values[i].amp_high = vals[i * 4 + 2];
            values[i].freq_high = vals[i * 4 + 3];
        }
        {
            std::lock_guard<std::mutex> lk(st().mu);
            for (uint32_t i = 0; i < n; ++i) st().vibration[handles[i]] = values[i];
        }
        for (uint32_t i = 0; i < n; ++i) forward_vibration(handles[i], values[i]);
        return 0;
    });
    ignore_cmd(svc, 207, "SendVibrationGcErmCommand");
    ignore_cmd(svc, 208, "GetActualVibrationGcErmCommand", 8);
    ignore_cmd(svc, 209, "BeginPermitVibrationSession");
    ignore_cmd(svc, 210, "EndPermitVibrationSession");
    svc.add_cmd(211, "IsVibrationDeviceMounted", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const int idx = index_of_npad_id(decode_handle(req.arg<uint32_t>(0)).npad_id);
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<uint8_t>(idx >= 0 && st().connected[idx] ? 1 : 0);
        return 0;
    });
    ignore_cmd(svc, 212, "SendVibrationValueInBool");
    svc.add_cmd(214, "SendVibrationValues2", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        // Switch 2: nn::hid::detail::SendVibrationValues (sdk 0x750064d360) sends this instead of 206 unless the Switch 1 compatible
        // mode is set, with the same two buffers (handles, VibrationValues) and an extra u16 (2). Forwarded like 206.
        const ipc::Buffer hb = req.in_buf(0), vb = req.in_buf(1);
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(std::min<uint64_t>(hb.size / 4, vb.size / 16), 16));
        std::vector<uint32_t> handles(n);
        std::vector<VibValue> values(n);
        if (n) {
            buf_read(hb, handles.data(), n * 4ull);
            buf_read(vb, values.data(), n * 16ull);
        }
        {
            std::lock_guard<std::mutex> lk(st().mu);
            for (uint32_t i = 0; i < n; ++i) st().vibration[handles[i]] = values[i];
            if (!st().vib214_logged && n) {
                st().vib214_logged = true;
                NXLOG_I("hid", "SendVibrationValues2 (214): %u values, first handle %x, routed to the pad rumble", n, handles[0]);
            }
        }
        for (uint32_t i = 0; i < n; ++i) forward_vibration(handles[i], values[i]);
        return 0;
    });

    // ---- console and seven-axis sensors (not used by the title; accepted)
    ignore_cmd(svc, 300, "ActivateConsoleSixAxisSensor");
    ignore_cmd(svc, 301, "StartConsoleSixAxisSensor");
    ignore_cmd(svc, 302, "StopConsoleSixAxisSensor");
    ignore_cmd(svc, 303, "ActivateSevenSixAxisSensor");
    ignore_cmd(svc, 304, "StartSevenSixAxisSensor");
    ignore_cmd(svc, 305, "StopSevenSixAxisSensor");
    ignore_cmd(svc, 306, "InitializeSevenSixAxisSensor");
    ignore_cmd(svc, 307, "FinalizeSevenSixAxisSensor");
    ignore_cmd(svc, 308, "SetSevenSixAxisSensorFusionStrength");
    ignore_cmd(svc, 309, "GetSevenSixAxisSensorFusionStrength", 4);
    ignore_cmd(svc, 310, "ResetSevenSixAxisSensorTimestamp");

    // ---- system and mode
    svc.add_cmd(400, "IsUsbFullKeyControllerEnabled", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(0);
        return 0;
    });
    svc.add_cmd(1000, "SetNpadCommunicationMode", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        // The mode comes first, then the aruid (Ryujinx). ScarletNX's table lists the aruid first; see the report.
        std::lock_guard<std::mutex> lk(st().mu);
        st().npad_communication = req.arg<int64_t>(0);
        return 0;
    });
    svc.add_cmd(1001, "GetNpadCommunicationMode", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<int64_t>(st().npad_communication);
        return 0;
    });
    ignore_cmd(svc, 1002, "SetTouchScreenConfiguration");
    ignore_cmd(svc, 1003, "IsFirmwareUpdateNeededForNotification", 1);
    ignore_cmd(svc, 1004, "SetTouchScreenResolution");
    // ---- Switch 2 Joy-Con 2 mouse (Nmouse / Jcms). Ids from the sdk proxy thunks (0x75006701b4..0x75006702ec, IHidServer vtable
    // slots 0x538..0x5B8) matched to the nn::hid::detail callers; raw-data layouts from the thunks' CMIF writers. docs/hid_mouse_sw2.md.
    // Pokopia imports only the wrappers behind 3110, 3111, 3112, 3131 and 3136-3139; the others get the answer a server with the two
    // listed devices gives. There is no 3114-3129 or 3134 in the SDK's table.
    svc.add_cmd(3110, "InitializeJcms", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const uint64_t size = req.arg<uint64_t>(8);   // aruid at 0
        std::lock_guard<std::mutex> lk(st().mu);
        HidState& s = st();
        s.jcms_addr = tm_addr(req, size, kJcmsMinSize, "InitializeJcms");
        s.jcms_size = s.jcms_addr ? size : 0;
        if (s.jcms_addr) jcms_setup(s);
        NXLOG_I("hid", "InitializeJcms(size %llx) -> block %llx", static_cast<unsigned long long>(size), static_cast<unsigned long long>(s.jcms_addr));
        return 0;
    });
    svc.add_cmd(3111, "FinalizeJcms", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().jcms_addr = st().jcms_size = 0;
        NXLOG_I("hid", "FinalizeJcms");
        return 0;
    });
    svc.add_cmd(3112, "StartJcms", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        // u32 JcmsHandle @0, aruid @8. The game calls it every frame for each Joy-Con 2 mouse it found, so a call proves it read the
        // Nmouse handle table. The Jcms rings are sampled whether started or not.
        std::lock_guard<std::mutex> lk(st().mu);
        if (st().jcms_starts < 3) NXLOG_I("hid", "StartJcms(handle %x): the game found the Joy-Con 2 mouse", req.arg<uint32_t>(0));
        st().jcms_starts++;
        return 0;
    });
    // u32 JcmsHandle @0, aruid @8. Not imported by Pokopia; the rings keep sampling (nothing to stop).
    ignore_cmd(svc, 3113, "StopJcms");
    // InitializeNmouse with an NmouseStateRange (sdk 0x75006576c0): the width and height come from the SDK's range table (720p, 1080p,
    // 1440p, 4K), the pointer type is 0. The SDK sends SetMergedNmouseSourceTypes(6) right after it.
    svc.add_cmd(3130, "InitializeNmouseWithStateRange", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        return nm_initialize(req, "InitializeNmouseWithStateRange");
    });
    svc.add_cmd(3131, "FinalizeNmouse", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().nm_addr = st().nm_size = 0;
        g_jc2_listed.store(false);
        g_jc2_active.store(false);
        NXLOG_I("hid", "FinalizeNmouse");
        return 0;
    });
    // SetNmousePointerRanges / SetNmouseStateRanges (sdk 0x7500656cc0, 0x75006578c0): s32 width @0, s32 height @4, aruid @8. The new
    // range applies from the next sample (pointer position and the state's range fields).
    svc.add_cmd(3132, "SetNmousePointerRanges", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const int32_t w = req.arg<int32_t>(0), h = req.arg<int32_t>(4);
        std::lock_guard<std::mutex> lk(st().mu);
        if (w > 0) st().nm_w = w;
        if (h > 0) st().nm_h = h;
        NXLOG_I("hid", "SetNmousePointerRanges(%d, %d)", w, h);
        return 0;
    });
    // GetNmousePointerRanges / GetNmouseStateRanges (sdk 0x7500656d40, 0x7500657960): aruid in; out s32 width @0, s32 height @4.
    svc.add_cmd(3133, "GetNmousePointerRanges", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<int32_t>(st().nm_w);
        rsp.push<int32_t>(st().nm_h);
        return 0;
    });
    // EnableShowingCursor (sdk 0x750065719c): bool @0, aruid @8. Stored where the client reads it back
    // (NmouseTransferMemoryAccessor::IsEnabledShowingCursor, block +0x8E68).
    svc.add_cmd(3135, "EnableShowingCursor", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        if (st().nm_addr) reinterpret_cast<uint8_t*>(st().nm_addr)[kNmShowCursor] = req.arg<uint8_t>(0) ? 1 : 0;
        return 0;
    });
    svc.add_cmd(3136, "SetNmouseButtonConfigJoyLeftJcms", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().nm_btn_left = req.arg<uint64_t>(8);   // aruid at 0
        return 0;
    });
    svc.add_cmd(3137, "SetNmouseButtonConfigJoyRightJcms", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().nm_btn_right = req.arg<uint64_t>(8);
        return 0;
    });
    svc.add_cmd(3138, "SetMergedNmouseSourceTypes", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().nm_merged_types = req.arg<uint32_t>(0);   // aruid at 8
        return 0;
    });
    // InitializeNmouse (sdk 0x75006569e0, the form Pokopia uses: 0xC000 bytes, 1920x1080).
    svc.add_cmd(3139, "InitializeNmouse", [](ipc::Request& req, ipc::Response&) -> uint32_t { return nm_initialize(req, "InitializeNmouse"); });
    // SetNmousePointerPosition (sdk 0x7500657410): s32 x @0, s32 y @4, aruid @8, NmouseHandle @0x10. Places the virtual pointer, which
    // the PC mouse then moves (sample_nmouse). Fortune's Weave puts it at the centre every frame after reading the state and takes the
    // offset from the centre as the frame's movement.
    svc.add_cmd(3140, "SetNmousePointerPosition", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const int32_t x = req.arg<int32_t>(0), y = req.arg<int32_t>(4);
        const uint64_t n = g_jc2_recentres.fetch_add(1, std::memory_order_relaxed);
        const LogLevel level = n < 3 ? LogLevel::Info : LogLevel::Debug;
        if (log_enabled(level))
            logf(level, "hid", "SetNmousePointerPosition(%d, %d, handle %llx)", x, y, static_cast<unsigned long long>(req.arg<uint64_t>(0x10)));
        std::lock_guard<std::mutex> lk(st().mu);
        HidState& s = st();
        s.nm_px = static_cast<float>(std::clamp(x, 0, s.nm_w - 1));
        s.nm_py = static_cast<float>(std::clamp(y, 0, s.nm_h - 1));
        return 0;
    });
    // EnableNmousePointer (sdk 0x75006572b0): bool @0, aruid @8, NmouseHandle @0x10. Stored in that handle's info byte, which the client
    // reads back (IsEnableNmousePointer, info +0x10).
    svc.add_cmd(3141, "EnableNmousePointer", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const bool on = req.arg<uint8_t>(0) != 0;
        const uint64_t handle = req.arg<uint64_t>(0x10);
        std::lock_guard<std::mutex> lk(st().mu);
        HidState& s = st();
        if (!s.nm_addr) return 0;
        uint8_t* b = reinterpret_cast<uint8_t*>(s.nm_addr);
        for (uint32_t i = 0; i < kNmSlots; ++i)
            if (b[kNmHandles + i * 16 + 8] && std::memcmp(b + kNmHandles + i * 16, &handle, 8) == 0) b[kNmInfo + i * kNmInfoSize + 0x10] = on ? 1 : 0;
        return 0;
    });
    // RestrictNmousePointer (sdk 0x7500657630): four floats @0..0xC, aruid @0x10. The PC pointer is not restricted: accepted.
    ignore_cmd(svc, 3142, "RestrictNmousePointer");
    // 3143 (vtable slot 0x5B8): aruid @0, u64 @8; no nn::hid::detail caller in the SDK, name unknown. Accepted.
    ignore_cmd(svc, 3143, "Cmd3143");
}

void hid_stop_sampler(HidState& s) {
    s.stop.store(true);
    if (s.sampler.joinable()) s.sampler.join();
    s.stop.store(false);
}

}  // namespace

void register_hid() {
    add("hid", [] { return make_iface("nn::hid::IHidServer", setup_hid); });
}

void hid_reset() {
    HidState& s = st();
    hid_stop_sampler(s);
    std::lock_guard<std::mutex> lk(s.mu);
    s.supported_styles = kDefaultStyles;
    s.supported_index_mask = kDefaultIdMask;
    s.npad_active = s.debug_pad_active = s.touch_active = s.mouse_active = s.keyboard_active = false;
    s.sixaxis_fusion = false;
    s.vibration_permitted = true;
    s.unintended_home_protection = false;
    s.joy_hold = 0;
    s.handheld_activation = 0;
    s.npad_communication = 0;
    std::memset(s.last_tag, 0, sizeof s.last_tag);
    std::memset(s.connected, 0, sizeof s.connected);
    s.vibration.clear();
    s.nm_addr = s.nm_size = s.jcms_addr = s.jcms_size = 0;
    s.nm_w = 1920;
    s.nm_h = 1080;
    s.nm_merged_types = 0;
    s.nm_btn_left = s.nm_btn_right = 0;
    s.nm_x = s.nm_y = -1;
    s.nm_px = 960;
    s.nm_py = 540;
    s.nm_logged = s.jcms_starts = s.active_logs = 0;
    s.nm_in_use = false;
    s.nm_wake = 0;
    s.nm_wake_ns = 0;
    s.vib214_logged = false;
    g_jc2_listed.store(false);
    g_jc2_active.store(false);
    s.background.store(true);
    for (auto& e : s.style_event) e = ServiceEvent();
    if (s.shm && s.shm->ok() && mem::is_mapped(s.shm->host(), kSize)) {
        init_shm(reinterpret_cast<uint8_t*>(s.shm->host()));
    } else {
        s.shm.reset();   // created again on first use
    }
}

void hid_sample_once() {
    HidState& s = st();
    const InputSnapshot snap = read_input();
    const bool docked = profile().docked;
    std::lock_guard<std::mutex> lk(s.mu);
    uint8_t* host = host_of(s);
    if (!host) return;
    if (s.npad_active) {
        for (uint32_t i = 0; i < kNpadCount; ++i) sample_npad(s, host, i, snap, docked);
    }
    if (s.debug_pad_active) ring_push(ring_at<DebugPadState>(host, kDebugPad), DebugPadState{});
    if (s.touch_active) sample_touch(host, snap.touch);
    if (s.mouse_active) sample_mouse(host, snap.mouse);
    if (s.keyboard_active) sample_keyboard(host, snap.keyboard);
    sample_nmouse(s, snap);
}

void hid_set_background_sampling(bool enabled) {
    st().background.store(enabled);
}

}  // namespace nxrt::services

// ---- Joy-Con 2 mouse: the play layer's side (include/nxrt/jc2_mouse.h) -------------------------------------------------------------------
namespace nxrt::hid {

namespace {
struct PointerHandover {
    std::mutex mu;
    float host_u = 0, host_v = 0;
    bool host = false;
    float ret_u = 0, ret_v = 0;
    bool ret = false;
};
PointerHandover& handover() {
    static PointerHandover* h = new PointerHandover();
    return *h;
}
}  // namespace

Jc2MouseMode jc2_mouse_mode() {
    int m = services::g_jc2_mode.load();
    if (m < 0) {
        const game::Info* g = game::current();
        std::string v = config_get_str("input", "jc2_mouse", g && g->jc2_mouse_default && *g->jc2_mouse_default ? g->jc2_mouse_default : "auto");
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        m = v == "off" ? 0 : (v == "on" ? 2 : 1);
        if (v != "off" && v != "on" && v != "auto") NXLOG_W("hid", "[input] jc2_mouse = %s: not auto, on or off; using auto", v.c_str());
        services::g_jc2_mode.store(m);
    }
    return static_cast<Jc2MouseMode>(m);
}

void jc2_mouse_set_mode(Jc2MouseMode m) {
    services::g_jc2_mode.store(static_cast<int>(m));
}

const char* jc2_mouse_mode_name(Jc2MouseMode m) {
    return m == Jc2MouseMode::Off ? "off" : (m == Jc2MouseMode::On ? "on" : "auto");
}

void jc2_mouse_feed(const HostMouse& m) {
    services::Jc2Feed& f = services::feed();
    std::lock_guard<std::mutex> lk(f.mu);
    f.m = m;
    f.wheel_x += m.wheel_x;
    f.wheel_y += m.wheel_y;
    f.dx += m.dx;
    f.dy += m.dy;
    f.wake += m.wake;
    f.presses |= m.presses;
    f.last_ns = services::now_ns();
    if (m.pointer) {
        PointerHandover& h = handover();
        std::lock_guard<std::mutex> lk2(h.mu);
        h.host_u = m.u;
        h.host_v = m.v;
        h.host = true;
    }
}


bool jc2_mouse_host_pointer(float* u, float* v) {
    PointerHandover& h = handover();
    std::lock_guard<std::mutex> lk(h.mu);
    if (h.host) {
        *u = h.host_u;
        *v = h.host_v;
    }
    return h.host;
}

void jc2_mouse_set_return_point(float u, float v) {
    PointerHandover& h = handover();
    std::lock_guard<std::mutex> lk(h.mu);
    h.ret_u = u;
    h.ret_v = v;
    h.ret = true;
}

bool jc2_mouse_take_return_point(float* u, float* v) {
    PointerHandover& h = handover();
    std::lock_guard<std::mutex> lk(h.mu);
    if (!h.ret) return false;
    *u = h.ret_u;
    *v = h.ret_v;
    h.ret = false;
    return true;
}

void jc2_mouse_other_input() {
    services::Jc2Feed& f = services::feed();
    std::lock_guard<std::mutex> lk(f.mu);
    f.other_seq++;
    f.dx = f.dy = f.wake = 0;      // motion that came with it (a hand brushing the mouse) does not wake it again
}

void jc2_mouse_set_wake(float counts) {
    services::g_jc2_wake.store(std::max(0.f, counts), std::memory_order_relaxed);
}

bool jc2_mouse_engaged() {
    return services::g_jc2_active.load(std::memory_order_relaxed);
}

Jc2Stats jc2_mouse_stats() {
    Jc2Stats r;
    r.engages = services::g_jc2_engages.load(std::memory_order_relaxed);
    r.lifts = services::g_jc2_lifts.load(std::memory_order_relaxed);
    r.recentres = services::g_jc2_recentres.load(std::memory_order_relaxed);
    r.x = services::g_jc2_x.load(std::memory_order_relaxed);
    r.y = services::g_jc2_y.load(std::memory_order_relaxed);
    return r;
}

void jc2_mouse_request(bool want) {
    services::g_jc2_request.store(want);
}

bool jc2_mouse_wants_pointer() {
    const Jc2MouseMode m = jc2_mouse_mode();
    if (m == Jc2MouseMode::Off) return false;
    if (m == Jc2MouseMode::On) return services::g_jc2_listed.load();
    return services::g_jc2_listed.load() && services::g_jc2_request.load();
}

bool jc2_mouse_active() {
    return jc2_mouse_mode() != Jc2MouseMode::Off && services::g_jc2_listed.load() && services::jc2_feed_live();
}

}  // namespace nxrt::hid
