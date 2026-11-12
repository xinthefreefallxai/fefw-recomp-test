// SPDX-License-Identifier: GPL-3.0-or-later
// Profile-guided builds (docs/PGO.md). See nxrt/pgo.h.
#include "nxrt/pgo.h"

#include <atomic>

#include "nxrt/log.h"

// The profile runtime's entry point. A build without the profile runtime resolves the name to the stub below (MSVC / lld-link /alternatename),
// so normal builds need nothing and link as before. (One stub for the whole process: a second /alternatename for the name would conflict.)
extern "C" int __llvm_profile_write_file(void);
extern "C" int nxrt_no_profile_write(void) { return -1; }
#pragma comment(linker, "/alternatename:__llvm_profile_write_file=nxrt_no_profile_write")

namespace nxrt {

bool profile_runtime_linked() {
    int (*volatile write_profile)(void) = &__llvm_profile_write_file;   // volatile: the comparison is made at run time, not folded
    return write_profile != &nxrt_no_profile_write;
}

void profile_flush() {
    if (!profile_runtime_linked()) return;      // not an instrumented build
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    const int rc = __llvm_profile_write_file();
    if (rc == 0) NXLOG_I("pgo", "profile counters written (LLVM_PROFILE_FILE, default default.profraw in the working folder)");
    else NXLOG_W("pgo", "profile counters could not be written (error %d)", rc);
    log_flush();
}

}  // namespace nxrt
