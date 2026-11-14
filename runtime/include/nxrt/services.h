// SPDX-License-Identifier: GPL-3.0-or-later
// Horizon system services at IPC level (S3c): what the recompiled SDK's own service client code (nn::fs, nn::oe, nn::account,
// nn::time, nn::hid, nn::audio, nn::vi, ...) gets back from sm:GetServiceHandle(<name>) and the sessions behind it.
// Each service is a nxrt::ipc::Service registered by name in the ipc registry (ipc.h); register_all() registers every one of them.
// Command ids, layouts and result codes follow https://switchbrew.org/wiki/Services_API and the per-service pages (SDK 12.x era).
//
// Conventions for every service file in src/services/:
//   * state that belongs to the process (the applet message queue, the open save data, the clock offsets) lives in a function-local
//     singleton in the service's .cpp; the service objects handed out per session are thin and share it. A `<name>_reset()` hook
//     (declared in src/services/services_internal.h) puts that state back to its start-up value and is called by reset_all().
//   * every command registered with add_cmd() has a Switchbrew name; unknown commands fall through to Service::handle, which logs
//     "not implemented" and answers kStubService (0x0BCBD5): the end-of-run ipc summary then lists them.
//   * a command that is deliberately accepted and ignored is registered with a handler that logs at Debug and returns 0.
//   * buffers: ipc::Request::in_buf(n) / out_buf(n) already pick A/X and B/C; pointers are host pointers.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nxrt/ipc.h"
#include "nxrt/kernel.h"

namespace nxrt::services {

// ---- registration ---------------------------------------------------------------------------------------------------------
// Registers every service in this library with ipc::register_service. Safe to call again after ipc::registry_reset().
void register_all();
// Puts the process-wide state of every service back to its start-up value (tests, a restart of the guest).
void reset_all();
// Names of the services register_all() registers (sorted).
std::vector<std::string> registered_names();

// ---- result codes ---------------------------------------------------------------------------------------------------------
// result = module | (description << 9). Module numbers and descriptions: https://switchbrew.org/wiki/Error_codes (look each one up).
constexpr uint32_t res(uint32_t module, uint32_t desc) { return kernel::make_result(module, desc); }

// ---- guest buffers --------------------------------------------------------------------------------------------------------
// Copies up to min(size, buf.size) bytes into a guest buffer; returns the number written (0 when the buffer is absent).
inline uint64_t buf_write(const ipc::Buffer& b, const void* src, uint64_t size) {
    if (!b.addr || !src) return 0;
    const uint64_t n = size < b.size ? size : b.size;
    __builtin_memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(b.addr)), src, static_cast<size_t>(n));
    return n;
}
inline uint64_t buf_read(const ipc::Buffer& b, void* dst, uint64_t size) {
    if (!b.addr || !dst) return 0;
    const uint64_t n = size < b.size ? size : b.size;
    __builtin_memcpy(dst, reinterpret_cast<const void*>(static_cast<uintptr_t>(b.addr)), static_cast<size_t>(n));
    return n;
}
inline const uint8_t* buf_ptr(const ipc::Buffer& b) { return reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(b.addr)); }
inline uint8_t* buf_mut(const ipc::Buffer& b) { return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(b.addr)); }

// ---- events ---------------------------------------------------------------------------------------------------------------
// A readable kernel event owned by a service. new_handle() makes a fresh handle onto the shared state each time it is called (the
// guest owns the handle and closes it); signal()/clear() change the state seen through every handle and wake threads blocked in
// WaitSynchronization. Manual-reset like a Horizon readable event: the guest clears it with ResetSignal/ClearEvent.
class ServiceEvent {
public:
    explicit ServiceEvent(bool signaled = false);
    uint32_t new_handle() const;            // 0 when the handle table is full
    void signal();
    void clear();
    bool signaled() const;
    std::shared_ptr<kernel::EventState> state() const { return st_; }

private:
    std::shared_ptr<kernel::EventState> st_;
};

// Shared memory handed to the guest (hid, pl:u, the audio renderer's work buffer ...). `host()` is where the service writes: it is the
// kernel-region buffer until the guest maps the object (MapSharedMemory), and the guest's view afterwards (the kernel tracks that in
// KSharedMemory::host), so always write through host(), never through a pointer cached at creation.
class ServiceSharedMemory {
public:
    explicit ServiceSharedMemory(uint64_t size, const char* name = "");
    uint64_t host() const { return obj_ ? obj_->host : 0; }
    uint64_t size() const { return obj_ ? obj_->size : 0; }
    uint32_t new_handle() const;            // a new handle onto the same object (each handle is closed by the guest)
    bool ok() const { return obj_ != nullptr; }

private:
    std::shared_ptr<kernel::KSharedMemory> obj_;
};

// ---- title profile --------------------------------------------------------------------------------------------------------
// Facts about the program and the console that several services answer from. The defaults below are neutral; the registered game fills
// its own (nxrt/game.h, Info::title_defaults: its main.npdm and control.nacp values) and every field can be overridden by an ini key
// (docs/CONFIG.md, sections [title] and [system]).
enum class Language : uint32_t {   // nn::settings::Language / set LanguageCode order (Switchbrew "Settings services")
    Japanese = 0, AmericanEnglish, French, German, Italian, Spanish, Chinese, Korean, Dutch, Portuguese, Russian, Taiwanese,
    BritishEnglish, CanadianFrench, LatinAmericanSpanish, SimplifiedChinese, TraditionalChinese, BrazilianPortuguese, Count
};
struct TitleProfile {
    uint64_t program_id = 0;                               // [title] program_id
    std::string name;                                      // [title] name
    std::string display_version;                           // [title] display_version
    uint32_t supported_languages = 0;                      // bit n = Language n; [title] supported_languages (comma list of names, or a hex mask)
    uint64_t save_data_owner_id = 0;                       // [title] save_owner_id (default: the program id)
    uint64_t account_save_size = 0;                        // [title] account_save_size   (control.nacp UserAccountSaveDataSize)
    uint64_t account_save_journal = 0;                     // [title] account_save_journal (UserAccountSaveDataJournalSize)
    uint64_t device_save_size = 0, device_save_journal = 0;   // none
    uint64_t cache_save_size = 0, cache_save_journal = 0;     // none
    uint64_t main_thread_stack = 0x100000;                 // main.npdm default stack size
    int main_thread_priority = 44, main_thread_core = 0;   // main.npdm
    uint64_t system_resource_size = 0;                     // main.npdm META SystemResourceSize ([title] system_resource_size): GetInfo 16.
                                                           // Non-zero turns on the SDK's virtual address memory manager (nn::os VAMM).
    // console
    Language language = Language::AmericanEnglish;         // [system] language (name, e.g. AmericanEnglish)
    int region = 1;                                        // [system] region: Japan 0, USA 1, Europe 2, Australia 3, HongKongTaiwanKorea 4, China 5 (name or number; Switchbrew RegionCode)
    std::string firmware = "12.1.0";                       // [system] firmware  (set:sys GetFirmwareVersion)
    std::string device_nickname = "nxrt";                  // [system] nickname
    bool docked = true;                                    // [am] operation_mode = docked|handheld
    std::string user_nickname = "Player";                  // [account] nickname
    uint64_t user_uid_hi = 0x0000000000000001ull, user_uid_lo = 0x6e78727475736572ull;   // [account] uid = 32 hex digits ("nxrtuser")
};
uint64_t program_id_now();               // [process] program_id if set, else [title] program_id, else the game's title id; read from the config on every call
const TitleProfile& profile();           // loaded from the config on first use
void profile_reload();                   // tests: re-read the config
const char* language_code(Language l);   // "en-US", "ja", ... (the 8-byte LanguageCode string)
uint64_t language_code_u64(Language l);  // the same string as the little-endian u64 the settings services return

// The host folder game data is served from ([fs] romfs, default "game/romfs" relative to the working directory, resolved at first
// use) and the folder save data lives in ([fs] save_dir, default %APPDATA%\<game save folder>\save, nxrt/game.h).
std::string romfs_dir();
std::string save_dir();

// A game module's say over the paths of the account save data. The hook gets the save's host folder (UTF-8), the normalised guest path
// ("/a/b", it may rewrite it) and whether the command changes the file system; it runs on the calling guest thread before the path is
// used. With a hook set the save has no size limit (the hook may keep several copies of the game's save in the folder). Set it before
// the game mounts its save data; nullptr removes it.
using SavePathHook = void (*)(const std::string& host_root, std::string* path, bool write);
void set_save_path_hook(SavePathHook hook);

}  // namespace nxrt::services
