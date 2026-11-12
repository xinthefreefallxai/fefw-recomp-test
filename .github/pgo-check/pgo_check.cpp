// CI check of nxrt::profile_flush (runtime/src/core/pgo.cpp, docs/PGO.md): built once with -fprofile-instr-generate (the counters must reach
// LLVM_PROFILE_FILE although the process ends with TerminateProcess) and once without (the /alternatename stub: links and does nothing).
#include <windows.h>

#include <cstdarg>
#include <cstdio>

#include "nxrt/log.h"
#include "nxrt/pgo.h"

namespace nxrt {
void logf(LogLevel, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::printf("%s: ", tag);
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
}
void log_flush() { std::fflush(stdout); }
}  // namespace nxrt

__declspec(noinline) int hot(int x) { return x * 3 + 1; }

int main() {
    volatile int s = 0;
    for (int i = 0; i < 1000; ++i) s = s + hot(i);
    nxrt::profile_flush();
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);   // as the game ends: no atexit handlers
    return 1;
}
