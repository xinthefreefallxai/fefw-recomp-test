// SPDX-License-Identifier: GPL-3.0-or-later
// Crash handler (declared in nxrt/diag.h): an unhandled-exception filter that writes the "HOST EXCEPTION" report, plus a
// vectored handler that catches stack overflow on a fresh helper thread. Guest traps (0xE0000001 / 0xE0000002) arrive at
// the unhandled filter like any other exception. The layout of the report follows crash_build_report() in diag.h.
// Idea from smo-recomp runtime/core/hostrt.c crash_filter; written here from scratch.
#include "nxrt/diag.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

#include <stdexcept>
#include <thread>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/write_watch.h"
#include "nxrt/memory.h"
#include "nxrt/symbols.h"
#include "stackwalk.h"

namespace nxrt {
namespace {

constexpr DWORD kTrapCode = 0xE0000001u;
constexpr DWORD kUntranslatedCode = 0xE0000002u;
constexpr DWORD kGuestBreakCode = 0xE0000003u;   // svcBreak / svcReturnFromException (svc_thread.cpp)
// Failures that never reach the unhandled-exception filter, so they ended the process with no report (#132, #127):
//  - STATUS_HEAP_CORRUPTION: ntdll raises it from inside the heap and ends the process itself (WER) when no handler takes it; only a
//    vectored handler sees it.
//  - abort(), std::terminate, an invalid parameter to a C runtime function, a pure virtual call: the C runtime ends the process with a
//    fail-fast (0xC0000409) after calling its handlers, which are ours now. The fail-fast itself cannot be caught.
// (Approach after NX2navi's runtime/platform/win32.c crash hooks; written here from scratch.)
constexpr DWORD kHeapCorruptionCode = 0xC0000374u;
constexpr DWORD kFailFastCode = 0xC0000409u;
constexpr DWORD kInvalidCrtParamCode = 0xC0000417u;
constexpr size_t kMaxHostFrames = 32;
constexpr int kMaxGuestFrames = 24;
constexpr DWORD kOverflowWaitMs = 30000;

std::mutex g_install_mu;
bool g_installed = false;
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;
void* g_veh = nullptr;

// Options copied at install time and read by the handler.
bool g_path_given = false;  // false: default "<exe>_crash.txt" next to the exe; true with "" : no file
std::string g_path;
bool g_terminate = true;
uint32_t g_exit_code = 4;
void (*g_on_report)(const char*, void*) = nullptr;
void* g_user = nullptr;

std::atomic<DWORD> g_handler_tid{0};           // the thread whose report is being written; 0 = none
std::atomic<bool> g_overflow_reported{false};   // the vectored handler already reported this stack overflow
std::atomic<uint32_t> g_reports{0};

// Stack overflow: the faulting thread has no stack left, so its record and context are copied into these statics and a
// helper thread builds the report from them.
EXCEPTION_RECORD g_ov_rec{};
CONTEXT g_ov_ctx{};
DWORD g_ov_tid = 0;

// Heap corruption: the report path worked out at install time (writing the first report must not allocate), and the watchdog that
// ends the process when the full report hangs on the damaged heap (its lock, or the log writer waiting for it).
char g_raw_path[1024] = {};
HANDLE g_heap_watch_event = nullptr;
constexpr DWORD kHeapWatchMs = 10000;
const char* g_failure_what = nullptr;   // the header of a C runtime failure report (report_runtime_failure)

#if defined(__clang__) || defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void appendf(std::string& out, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    if (n >= 0 && static_cast<size_t>(n) < sizeof buf) {
        out.append(buf, static_cast<size_t>(n));
    } else if (n > 0) {
        std::string big(static_cast<size_t>(n) + 1, '\0');
        std::vsnprintf(big.data(), big.size(), fmt, ap2);
        out.append(big.data(), static_cast<size_t>(n));
    }
    va_end(ap);
    va_end(ap2);
}

// True when every byte of [addr, addr+n) is committed, readable and not a guard page. Never faults.
bool readable(uint64_t addr, uint64_t n) {
    if (n == 0) return true;
    uint64_t p = addr;
    const uint64_t end = addr + n;
    if (end < addr) return false;
    const DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(p)), &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if (!(mbi.Protect & kReadable)) return false;
        const uint64_t next = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)) + mbi.RegionSize;
        if (next <= p) return false;
        p = next;
    }
    return true;
}

// <exe path without extension>_crash.txt
std::string default_report_path() {
    char exe[1024] = {};
    DWORD n = GetModuleFileNameA(nullptr, exe, sizeof exe);
    std::string p(exe, n < sizeof exe ? n : sizeof exe - 1);
    size_t slash = p.find_last_of("\\/");
    size_t dot = p.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) p.erase(dot);
    return p + "_crash.txt";
}

bool write_file(const std::string& path, const std::string& text) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD done = 0;
    BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &done, nullptr);
    CloseHandle(h);
    return ok && done == text.size();
}

// Builds the whole report text for an exception raised on thread `tid`.
std::string build_report(const EXCEPTION_RECORD* rec, const CONTEXT* cx, DWORD tid) {
    g_reports.fetch_add(1, std::memory_order_relaxed);
    std::string out;
    const uint64_t rip = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(rec->ExceptionAddress));
    const std::vector<ThreadRecord> threads = diag_threads();

    // The faulting thread: its registered name and Ctx. The current thread uses its thread-local Ctx; another thread
    // (stack-overflow helper) uses the Ctx its registry record holds.
    std::string tname = "unregistered";
    bool named = false;
    ::Ctx* gctx = nullptr;
    for (const auto& t : threads) {
        if (t.host_tid != tid) continue;
        if (!named) {
            tname = t.name.empty() ? std::string("(unnamed)") : t.name;
            named = true;
        }
        if (!gctx && t.ctx) gctx = t.ctx;
    }
    if (tid == GetCurrentThreadId()) gctx = diag_thread_ctx();
    if (gctx && !readable(reinterpret_cast<uint64_t>(gctx), sizeof(::Ctx))) gctx = nullptr;

    // Header.
    const char* code_name = nullptr;
    if (rec->ExceptionCode == kTrapCode) code_name = "GUEST TRAP";
    else if (rec->ExceptionCode == kUntranslatedCode) code_name = "UNTRANSLATED INSTRUCTION";
    else if (rec->ExceptionCode == kGuestBreakCode) code_name = "GUEST BREAK (svcBreak)";
    else if (rec->ExceptionCode == kHeapCorruptionCode) code_name = "HEAP CORRUPTION";
    else if (rec->ExceptionCode == kFailFastCode || rec->ExceptionCode == kInvalidCrtParamCode)
        code_name = g_failure_what ? g_failure_what : "FAIL FAST";
    if (code_name) {
        appendf(out, "HOST EXCEPTION 0x%08lX (%s) at host address 0x%llx on thread %s (tid %lu)\n",
                static_cast<unsigned long>(rec->ExceptionCode), code_name, static_cast<unsigned long long>(rip),
                tname.c_str(), static_cast<unsigned long>(tid));
    } else {
        appendf(out, "HOST EXCEPTION 0x%08lX at host address 0x%llx on thread %s (tid %lu)\n",
                static_cast<unsigned long>(rec->ExceptionCode), static_cast<unsigned long long>(rip), tname.c_str(),
                static_cast<unsigned long>(tid));
    }
    appendf(out, "  in %s\n", symbols_describe(rip).c_str());
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const ULONG_PTR kind = rec->ExceptionInformation[0];
        const char* what = kind == 0 ? "reading" : kind == 1 ? "writing" : kind == 8 ? "executing" : "accessing";
        appendf(out, "  %s address 0x%llx\n", what, static_cast<unsigned long long>(rec->ExceptionInformation[1]));
        const uint64_t addr = rec->ExceptionInformation[1];
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi)) {
            appendf(out, "  host page: base=0x%llx size=0x%llx allocation=0x%llx state=0x%lx protect=0x%lx allocation_protect=0x%lx type=0x%lx\n",
                (unsigned long long)mbi.BaseAddress, (unsigned long long)mbi.RegionSize, (unsigned long long)mbi.AllocationBase,
                mbi.State, mbi.Protect, mbi.AllocationProtect, mbi.Type);
        } else appendf(out, "  host page: VirtualQuery failed error=%lu\n", GetLastError());
        out += mem::describe_page(addr);
        out += mem::ww::describe_page(addr);
    }

    // Last guest function: the lifted function whose host code contains the faulting instruction.
    if (const uint64_t g = guest_function_of(rip))
        appendf(out, "last guest function entered: f_%llx\n", static_cast<unsigned long long>(g));
    else
        out += "last guest function entered: (unknown: fault address is not inside lifted code)\n";

    // Guest registers.
    if (!gctx) {
        out += "guest registers: (no guest context on this thread)\n";
    } else {
        out += "guest registers:\n";
        for (int n = 0; n <= 30; ++n) {
            out += (n % 3 == 0) ? (n == 0 ? "  " : "\n  ") : "   ";
            appendf(out, "x%-2d %016llx", n, static_cast<unsigned long long>(reg(gctx, n)));
        }
        out += "\n";
        appendf(out, "  sp %016llx   tpidr %016llx   tpidrro %016llx\n", static_cast<unsigned long long>(sp(gctx)),
                static_cast<unsigned long long>(tpidr_el0(gctx)), static_cast<unsigned long long>(tpidrro_el0(gctx)));
    }

    // Guest frame chain: lr of the Ctx, then the return address of every frame reachable through x29.
    if (!gctx) {
        out += "guest frame chain (x29): (none)\n";
    } else {
        appendf(out, "guest frame chain (x29): lr=0x%llx", static_cast<unsigned long long>(lr(gctx)));
        uint64_t f = fp(gctx);
        for (int i = 0; i < kMaxGuestFrames && f && readable(f, 16); ++i) {
            const uint64_t next = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(f));
            const uint64_t ret = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(f + 8));
            if (ret) appendf(out, " <- 0x%llx", static_cast<unsigned long long>(ret));
            if (next <= f) break;  // frames must go up the stack; anything else is a broken chain
            f = next;
        }
        out += "\n";
    }

    // Host call stack, symbolised.
    uint64_t frames[kMaxHostFrames] = {};
    const size_t nframes = detail::capture_stack(cx, frames, kMaxHostFrames);
    out += "host call stack (innermost first):\n";
    if (!nframes) out += "  (none)\n";
    for (size_t i = 0; i < nframes; ++i) appendf(out, "  %s\n", symbols_describe(frames[i]).c_str());

    // Guest call stack: the lifted functions among the host frames, consecutive repeats dropped.
    out += "guest call stack (innermost first):";
    uint64_t last = 0;
    bool any = false;
    for (size_t i = 0; i < nframes; ++i) {
        const uint64_t g = guest_function_of(frames[i]);
        if (!g || g == last) continue;
        appendf(out, " f_%llx", static_cast<unsigned long long>(g));
        last = g;
        any = true;
    }
    if (!any) out += " (none)";
    out += "\n";

    // Threads of the registry.
    out += "threads:\n";
    if (threads.empty()) out += "  (none)\n";
    for (const auto& t : threads) {
        char lrbuf[32] = "-";
        if (t.ctx && readable(reinterpret_cast<uint64_t>(t.ctx), sizeof(::Ctx)))
            std::snprintf(lrbuf, sizeof lrbuf, "0x%llx", static_cast<unsigned long long>(lr(t.ctx)));
        appendf(out, "  %s tid=%lu guest_tid=%llu lr=%s\n", t.name.empty() ? "(unnamed)" : t.name.c_str(),
                static_cast<unsigned long>(t.host_tid), static_cast<unsigned long long>(t.guest_tid), lrbuf);
    }

    // Recent files, oldest first.
    const std::vector<std::string> files = diag_recent_files();
    out += "last files opened (oldest first):\n";
    if (files.empty()) out += "  (none)\n";
    for (const auto& f : files) appendf(out, "  %s\n", f.c_str());

    // The last 30 s of frame times: what the game was doing before it went wrong.
    out += frame_times_summary(30.0);
    return out;
}

// crash_set_hint: a known setup problem found earlier, put first in the report ("LIKELY CAUSE: ..."; the launcher shows that line).
char g_hint[1024] = {};

// Log block, report file, stderr, then the on_report callback. Terminating is the caller's decision.
void emit(const std::string& report) {
    const std::string text = g_hint[0] ? std::string("LIKELY CAUSE: ") + g_hint + "\n\n" + report : report;
    for (size_t pos = 0; pos < text.size();) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        NXLOG_E("crash", "%.*s", static_cast<int>(eol - pos), text.data() + pos);
        pos = eol + 1;
    }
    log_flush();
    const std::string path = g_path_given ? g_path : default_report_path();
    if (!path.empty()) write_file(path, text);
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
    if (g_on_report) g_on_report(text.c_str(), g_user);
}

[[noreturn]] void second_exception() {
    std::fputs("nxrt: second exception inside the crash handler, terminating\n", stderr);
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), g_exit_code);
    for (;;) Sleep(1000);
}

// Takes the handler for this thread. The same thread faulting again inside it terminates; another thread faulting at the same time
// (a bad object shared by two guest threads often makes both fault together) waits, so the first report is finished and written
// instead of the process ending with an empty one. The owner terminates the process, or (tests) releases the handler to the waiter.
void enter_handler() {
    const DWORD me = GetCurrentThreadId();
    for (;;) {
        DWORD owner = 0;
        if (g_handler_tid.compare_exchange_strong(owner, me)) return;
        if (owner == me) second_exception();
        Sleep(50);
    }
}

void leave_handler() { g_handler_tid = 0; }

LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW &&
        g_overflow_reported.exchange(false))
        return EXCEPTION_CONTINUE_SEARCH;  // the vectored handler already wrote this report
    enter_handler();
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        leave_handler();
        return EXCEPTION_CONTINUE_SEARCH;
    }
    emit(build_report(ep->ExceptionRecord, ep->ContextRecord, GetCurrentThreadId()));
    if (g_terminate) TerminateProcess(GetCurrentProcess(), g_exit_code);
    leave_handler();
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI overflow_thread(LPVOID) {
    emit(build_report(&g_ov_rec, &g_ov_ctx, g_ov_tid));
    if (g_terminate) TerminateProcess(GetCurrentProcess(), g_exit_code);
    return 0;
}

// The first report of a heap corruption, made without the heap: the exception, and the host frames as module+offset, into a static buffer
// and straight to the report file. The full report (which allocates) is tried after it and replaces it when it gets through.
void raw_heap_report(const EXCEPTION_RECORD* rec, const CONTEXT* cx) {
    static char buf[6144];
    const size_t cap = sizeof buf;
    size_t n = 0;
    auto put = [&](int w) { if (w > 0) n = std::min(cap - 1, n + static_cast<size_t>(w)); };
    put(std::snprintf(buf, cap, "HOST EXCEPTION 0x%08lX (HEAP CORRUPTION) at host address 0x%llx on thread tid %lu\n"
                      "  the process heap was found damaged; this short report was written first, without the heap.\n"
                      "  The damage was done earlier, elsewhere: the frames below are where it was noticed.\n"
                      "host call stack (innermost first):\n",
                      static_cast<unsigned long>(rec->ExceptionCode), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(rec->ExceptionAddress)),
                      static_cast<unsigned long>(GetCurrentThreadId())));
    uint64_t frames[kMaxHostFrames] = {};
    const size_t nframes = detail::capture_stack(cx, frames, kMaxHostFrames);
    for (size_t i = 0; i < nframes && n < cap - 160; ++i) {
        void* base = nullptr;
        RtlPcToFileHeader(reinterpret_cast<void*>(static_cast<uintptr_t>(frames[i])), &base);
        char mod[MAX_PATH] = "?";
        if (base) {
            const DWORD m = GetModuleFileNameA(static_cast<HMODULE>(base), mod, sizeof mod);
            if (!m || m >= sizeof mod) std::snprintf(mod, sizeof mod, "?");
        }
        const char* slash = std::strrchr(mod, '\\');
        put(std::snprintf(buf + n, cap - n, "  %s+0x%llx\n", slash ? slash + 1 : mod,
                          static_cast<unsigned long long>(frames[i] - reinterpret_cast<uintptr_t>(base))));
    }
    if (g_raw_path[0]) {
        HANDLE h = CreateFileA(g_raw_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD done = 0;
            WriteFile(h, buf, static_cast<DWORD>(n), &done, nullptr);
            CloseHandle(h);
        }
    }
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE) {
        DWORD done = 0;
        WriteFile(err, buf, static_cast<DWORD>(n), &done, nullptr);
    }
}

DWORD WINAPI heap_watchdog(LPVOID) {
    WaitForSingleObject(g_heap_watch_event, INFINITE);
    Sleep(kHeapWatchMs);
    TerminateProcess(GetCurrentProcess(), g_exit_code);
    return 0;
}

LONG heap_corruption(EXCEPTION_POINTERS* ep) {
    enter_handler();
    if (g_heap_watch_event) SetEvent(g_heap_watch_event);
    raw_heap_report(ep->ExceptionRecord, ep->ContextRecord);
    // the full report: the file first (the log writer may be stuck behind the heap), then the log
    const std::string text = build_report(ep->ExceptionRecord, ep->ContextRecord, GetCurrentThreadId());
    const std::string path = g_path_given ? g_path : default_report_path();
    if (!path.empty()) write_file(path, text);
    emit(text);
    if (g_terminate) TerminateProcess(GetCurrentProcess(), g_exit_code);
    leave_handler();
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort, std::terminate, an invalid C runtime parameter, a pure virtual call: the report of the calling thread, from here.
void report_runtime_failure(const char* what, DWORD code) {
    CONTEXT cx{};
    RtlCaptureContext(&cx);
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = code;
    rec.ExceptionAddress = reinterpret_cast<void*>(static_cast<uintptr_t>(cx.Rip));
    enter_handler();
    g_failure_what = what;
    emit(build_report(&rec, &cx, GetCurrentThreadId()));
    if (g_terminate) TerminateProcess(GetCurrentProcess(), g_exit_code);
    g_failure_what = nullptr;
    leave_handler();
}

void on_sigabrt(int) { report_runtime_failure("ABORT (abort(), or std::terminate: an uncaught C++ exception)", kFailFastCode); }
void on_terminate() {
    report_runtime_failure("STD::TERMINATE (an uncaught C++ exception, or one leaving a noexcept function)", kFailFastCode);
    std::abort();
}
void on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    report_runtime_failure("INVALID C RUNTIME PARAMETER", kInvalidCrtParamCode);
}
void on_purecall() {
    report_runtime_failure("PURE VIRTUAL CALL (an object used while or after it was destroyed)", kFailFastCode);
    std::abort();
}

// [test] crash_kind / crash_after_s: the failure this runtime reports without the unhandled-exception filter, made on purpose from a helper
// thread, to check the reports (heap: a double free of a process-heap block).
struct PureBase {
    PureBase();
    virtual ~PureBase() = default;
    virtual void f() = 0;
};
void call_f(PureBase* p) { p->f(); }
PureBase::PureBase() { call_f(this); }   // during construction the vtable is PureBase's: a pure virtual call
struct PureDerived : PureBase { void f() override {} };

DWORD WINAPI test_crash_thread(LPVOID arg) {
    const std::string kind = *static_cast<std::string*>(arg);
    Sleep(static_cast<DWORD>(std::clamp<int64_t>(config_get_int("test", "crash_after_s", 30), 0, 86400) * 1000));
    NXLOG_W("crash", "[test] crash_kind = %s: failing now", kind.c_str());
    log_flush();
    if (kind == "heap") {
        HANDLE heap = GetProcessHeap();
        void* volatile p = HeapAlloc(heap, 0, 64);
        HeapFree(heap, 0, p);
        HeapFree(heap, 0, p);
        for (int i = 0; i < 64; ++i) HeapFree(heap, 0, HeapAlloc(heap, 0, 64));   // in case the second free went unnoticed
    } else if (kind == "abort") {
        std::abort();
    } else if (kind == "terminate") {
        std::thread([] { throw std::runtime_error("[test] crash_kind = terminate"); }).join();
    } else if (kind == "purecall") {
        PureDerived d;
    } else if (kind == "invalid_parameter") {
        char buf[4];
        const char* volatile src = "too long for the buffer";
        strcpy_s(buf, sizeof buf, src);
    }
    NXLOG_W("crash", "[test] crash_kind = %s: the process went on", kind.c_str());
    return 0;
}

LONG WINAPI crash_vectored(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode == kHeapCorruptionCode) return heap_corruption(ep);
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    enter_handler();
    g_ov_rec = *ep->ExceptionRecord;
    g_ov_ctx = *ep->ContextRecord;
    g_ov_tid = GetCurrentThreadId();
    HANDLE th = CreateThread(nullptr, 0, overflow_thread, nullptr, 0, nullptr);
    if (th) {
        WaitForSingleObject(th, kOverflowWaitMs);
        CloseHandle(th);
        g_overflow_reported = true;
    }
    leave_handler();
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void crash_install(const CrashOptions& opt) {
    std::lock_guard<std::mutex> lk(g_install_mu);
    g_path_given = opt.report_path != nullptr;
    g_path = opt.report_path ? opt.report_path : "";
    g_terminate = opt.terminate;
    g_exit_code = opt.exit_code;
    g_on_report = opt.on_report;
    g_user = opt.user;
    {
        const std::string p = g_path_given ? g_path : default_report_path();
        std::snprintf(g_raw_path, sizeof g_raw_path, "%s", p.size() < sizeof g_raw_path ? p.c_str() : "");
    }
    if (g_installed) return;
    if (g_terminate && !g_heap_watch_event) {
        g_heap_watch_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (g_heap_watch_event) {
            if (HANDLE th = CreateThread(nullptr, 64 * 1024, heap_watchdog, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)) CloseHandle(th);
        }
    }
    // the C runtime's last steps before its fail-fast (this module's runtime: the runtime is linked with the static one)
    signal(SIGABRT, on_sigabrt);
    std::set_terminate(on_terminate);
    _set_invalid_parameter_handler(on_invalid_parameter);
    _set_purecall_handler(on_purecall);
    g_prev_filter = SetUnhandledExceptionFilter(crash_filter);
    ULONG guarantee = 64 * 1024;  // stack reserved for the handlers if the thread overflows
    SetThreadStackGuarantee(&guarantee);
    g_veh = AddVectoredExceptionHandler(0, crash_vectored);
    g_installed = true;
    static std::string test_kind = config_get_str("test", "crash_kind", "");
    if (!test_kind.empty())
        if (HANDLE th = CreateThread(nullptr, 0, test_crash_thread, &test_kind, 0, nullptr)) CloseHandle(th);
}

void crash_uninstall() {
    std::lock_guard<std::mutex> lk(g_install_mu);
    if (!g_installed) return;
    SetUnhandledExceptionFilter(g_prev_filter);
    if (g_veh) RemoveVectoredExceptionHandler(g_veh);
    g_veh = nullptr;
    g_installed = false;
}

std::string crash_build_report(void* exception_pointers) {
    auto* ep = static_cast<EXCEPTION_POINTERS*>(exception_pointers);
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return "HOST EXCEPTION: no exception information\n";
    return build_report(ep->ExceptionRecord, ep->ContextRecord, GetCurrentThreadId());
}

std::string crash_report_path() { return g_path_given ? g_path : default_report_path(); }

void crash_set_hint(const char* text) {
    std::lock_guard<std::mutex> lk(g_install_mu);
    std::snprintf(g_hint, sizeof g_hint, "%s", text ? text : "");
}

uint32_t crash_exceptions_reported() { return g_reports.load(std::memory_order_relaxed); }

}  // namespace nxrt
