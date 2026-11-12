// SPDX-License-Identifier: GPL-3.0-or-later
// Profile-guided builds (docs/PGO.md). See nxrt/pgo.h.
#include "nxrt/pgo.h"

#include <atomic>

#include "nxrt/log.h"

// The profile runtime's entry point. A build without the profile runtime resolves the name to the stub below (MSVC / lld-link /alternatename),
// so normal builds need nothing and link as before.
extern "C" int __llvm_profile_write_file(void);
extern "C" int nxrt_llvm_profile_write_file_none(void) { return -1; }
#pragma comment(linker, "/alternatename:__llvm_profile_write_file=nxrt_llvm_profile_write_file_none")

namespace nxrt {

void profile_flush() {
    if (&__llvm_profile_write_file == &nxrt_llvm_profile_write_file_none) return;   // not an instrumented build
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    const int rc = __llvm_profile_write_file();
    if (rc == 0) NXLOG_I("pgo", "profile counters written (LLVM_PROFILE_FILE, default default.profraw in the working folder)");
    else NXLOG_W("pgo", "profile counters could not be written (error %d)", rc);
    log_flush();
}

}  // namespace nxrt
