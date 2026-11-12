// SPDX-License-Identifier: GPL-3.0-or-later
// Profile-guided builds (docs/PGO.md): writes the LLVM profile counters before the process ends.
#pragma once

namespace nxrt {

// Code built with -fprofile-instr-generate (the lifted modules through NX_EXTRA_FLAGS) links LLVM's profile runtime, which writes its counters
// (to LLVM_PROFILE_FILE, default default.profraw) from an atexit handler. This process ends through TerminateProcess or ExitProcess, which skip
// those handlers, so its exits call this first. Without the profile runtime (every normal build) it does nothing. Writes once per process.
void profile_flush();

}  // namespace nxrt
