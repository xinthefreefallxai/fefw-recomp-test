// SPDX-License-Identifier: GPL-3.0-or-later
// Thread, event, synchronisation, time and diagnostic SVCs. Semantics from the Switchbrew SVC page; structure of the handlers
// adapted from SuperMarioWonderRecomp runtime/src/kernel.cpp svc_impl (MIT, Copyright (c) 2026 BelmanteGu; see THIRD_PARTY_NOTICES.md).
// Argument registers (AArch64): see include/nxrt/svc.h.
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>

#include <cstdio>
#include "nxrt/pgo.h"
#include "nxrt/diag.h"
#include "nxrt/dispatch.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/runtime.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/svc.h"
#include "kernel_internal.h"

namespace nxrt {
namespace {

using namespace kernel;

uint64_t& X(::Ctx* c, int n) { return c->x[n]; }

void svc_exit_process(::Ctx* c) {
    NXLOG_I("svc", "svcExitProcess from thread %s", current_thread() ? current_thread()->name.c_str() : "?");
    {   // who asked: lr and the x29 frame chain (a game that quits during boot leaves no other trace)
        std::string chain;
        char buf[32];
        std::snprintf(buf, sizeof buf, "lr=0x%llx", static_cast<unsigned long long>(c->x[30]));
        chain += buf;
        uint64_t f = c->x[29];
        for (int i = 0; i < 24 && f && !IsBadReadPtr(reinterpret_cast<const void*>(f), 16); ++i) {
            const uint64_t next = *reinterpret_cast<const uint64_t*>(f), ret = *reinterpret_cast<const uint64_t*>(f + 8);
            std::snprintf(buf, sizeof buf, " <- 0x%llx", static_cast<unsigned long long>(ret));
            if (ret) chain += buf;
            if (next <= f) break;
            f = next;
        }
        NXLOG_I("svc", "svcExitProcess caller chain: %s", chain.c_str());
    }
    NXLOG_I("runtime", "state at svcExitProcess:\n%s", summary().c_str());
    log_flush();
    profile_flush();                                 // profile-guided builds: ExitProcess skips the profile runtime's atexit
    ExitProcess(0);
}

void svc_create_thread(::Ctx* c) {            // x1 entry, x2 arg, x3 stack top, w4 priority, w5 core -> x1 handle
    int32_t prio = static_cast<int32_t>(X(c, 4)), core = static_cast<int32_t>(X(c, 5));
    if (prio < 0 || prio > 63) return svc_ret(c, kInvalidPriority);
    if (core != -2 && (core < 0 || core > std::max(7, app_core_count() - 1))) return svc_ret(c, kInvalidCoreId);
    ThreadParams p;
    p.entry = X(c, 1);
    p.arg = X(c, 2);
    p.stack_top = X(c, 3);
    p.prio = prio;
    p.core = core;
    uint32_t h = thread_create(p);
    if (!h) return svc_ret(c, kOutOfResource);
    thread_diag_note_created(h);
    NXLOG_D("svc", "CreateThread entry %s prio %d core %d -> handle 0x%x", module_describe(p.entry).c_str(), prio, core, h);
    X(c, 1) = h;
    svc_ret(c, kOk);
}

void svc_start_thread(::Ctx* c) { svc_ret(c, thread_start(static_cast<uint32_t>(X(c, 0)))); }

void svc_exit_thread(::Ctx*) { thread_exit(); }

void svc_sleep_thread(::Ctx* c) {              // x0 ns (0, -1, -2 = yield variants)
    int64_t ns = static_cast<int64_t>(X(c, 0));
    if (ns > 0) sleep_ns(ns);
    else yield_host();
}

void svc_get_thread_priority(::Ctx* c) {       // w1 handle -> w1 priority
    auto t = handle_as<KThread>(static_cast<uint32_t>(X(c, 1)));
    if (!t) return svc_ret(c, kInvalidHandle);
    X(c, 1) = static_cast<uint32_t>(t->prio);
    svc_ret(c, kOk);
}

void svc_set_thread_priority(::Ctx* c) {       // w0 handle, w1 priority
    auto t = handle_as<KThread>(static_cast<uint32_t>(X(c, 0)));
    if (!t) return svc_ret(c, kInvalidHandle);
    if (!thread_set_priority(*t, static_cast<int32_t>(X(c, 1)))) return svc_ret(c, kInvalidPriority);
    svc_ret(c, kOk);
}

void svc_get_thread_core_mask(::Ctx* c) {      // w2 handle -> w1 ideal core, x2 affinity mask
    auto t = handle_as<KThread>(static_cast<uint32_t>(X(c, 2)));
    if (!t) return svc_ret(c, kInvalidHandle);
    X(c, 1) = static_cast<uint32_t>(t->ideal_core);
    X(c, 2) = t->core_mask;
    svc_ret(c, kOk);
}

void svc_set_thread_core_mask(::Ctx* c) {      // w0 handle, w1 ideal core (0..3, -1 do not care, -2 process default, -3 keep), x2 mask
    auto t = handle_as<KThread>(static_cast<uint32_t>(X(c, 0)));
    auto fail = [&](uint32_t r) {
        NXLOG_W("svc", "SetThreadCoreMask(0x%x, ideal %d, mask 0x%llx) failed: 0x%x", static_cast<uint32_t>(X(c, 0)), static_cast<int32_t>(X(c, 1)),
                static_cast<unsigned long long>(X(c, 2)), r);
        return svc_ret(c, r);
    };
    if (!t) {
        NXLOG_W("svc", "SetThreadCoreMask(0x%x, ideal %d, mask 0x%llx): not a thread handle", static_cast<uint32_t>(X(c, 0)), static_cast<int32_t>(X(c, 1)),
                static_cast<unsigned long long>(X(c, 2)));
        return svc_ret(c, kInvalidHandle);
    }
    int32_t ideal = static_cast<int32_t>(X(c, 1));
    uint64_t mask = X(c, 2);
    if (ideal == -3) {                                   // keep the ideal core, replace the mask
        if (!mask) return fail(kInvalidCombination);
        ideal = t->ideal_core;
    } else if (ideal != -2 && (ideal < -1 || ideal > std::max(7, app_core_count() - 1))) {              // -1 = IdealCoreDontCare: only the mask matters
        return fail(kInvalidCoreId);
    } else if (!mask) {
        return fail(kInvalidCombination);
    }
    if (!thread_set_core_mask(*t, ideal, mask)) return fail(kInvalidCombination);
    svc_ret(c, kOk);
}

void svc_get_current_processor_number(::Ctx* c) { X(c, 0) = static_cast<uint32_t>(sched_current_core()); }

void svc_signal_event(::Ctx* c) {              // w0 event handle (writable)
    auto e = handle_as<KEvent>(static_cast<uint32_t>(X(c, 0)));
    if (!e) return svc_ret(c, kInvalidHandle);
    e->state->signaled = true;
    notify_waitable_changed();
    svc_ret(c, kOk);
}

void svc_clear_event(::Ctx* c) {               // w0 event handle
    auto e = handle_as<KEvent>(static_cast<uint32_t>(X(c, 0)));
    if (!e) return svc_ret(c, kInvalidHandle);
    e->state->signaled = false;
    svc_ret(c, kOk);
}

void svc_reset_signal(::Ctx* c) {              // w0 readable event handle: InvalidState when it was not signaled
    auto e = handle_as<KEvent>(static_cast<uint32_t>(X(c, 0)));
    if (!e) return svc_ret(c, kInvalidHandle);
    if (!e->state->signaled) return svc_ret(c, kInvalidState);
    e->state->signaled = false;
    svc_ret(c, kOk);
}

void svc_close_handle(::Ctx* c) {
    uint32_t h = static_cast<uint32_t>(X(c, 0));
    if (h == kPseudoCurrentThread || h == kPseudoCurrentProcess || !handle_get(h)) {
        NXLOG_W("svc", "CloseHandle(0x%x): not a handle (lr=0x%llx)", h, static_cast<unsigned long long>(c->x[30]));
        return svc_ret(c, kInvalidHandle);
    }
    handle_close(h);
    svc_ret(c, kOk);
}

void svc_create_event(::Ctx* c) {              // -> w1 write handle, w2 read handle
    auto st = std::make_shared<EventState>();
    auto w = std::make_shared<KEvent>(), r = std::make_shared<KEvent>();
    w->state = r->state = st;
    w->writable = true;
    uint32_t hw = handle_add(w), hr = handle_add(r);
    if (!hw || !hr) {
        if (hw) handle_close(hw);
        if (hr) handle_close(hr);
        return svc_ret(c, kOutOfHandles);
    }
    X(c, 1) = hw;
    X(c, 2) = hr;
    svc_ret(c, kOk);
}

void svc_wait_synchronization(::Ctx* c) {      // x1 handles*, w2 count, x3 timeout -> w1 index
    int32_t index = -1;
    uint32_t count = static_cast<uint32_t>(X(c, 2));
    if (count > 0x40) return svc_ret(c, kOutOfRange);
    uint32_t handles[0x40];
    if (count && !mem::is_mapped(X(c, 1), count * 4ull)) return svc_ret(c, kInvalidPointer);
    for (uint32_t i = 0; i < count; i++) handles[i] = *reinterpret_cast<uint32_t*>(X(c, 1) + 4 * i);
    uint32_t rc = wait_synchronization(handles, static_cast<int32_t>(count), static_cast<int64_t>(X(c, 3)), &index);
    X(c, 1) = static_cast<uint32_t>(index);
    svc_ret(c, rc);
}

void svc_cancel_synchronization(::Ctx* c) { svc_ret(c, cancel_synchronization(static_cast<uint32_t>(X(c, 0)))); }
void svc_arbitrate_lock(::Ctx* c) { svc_ret(c, arbitrate_lock(static_cast<uint32_t>(X(c, 0)), X(c, 1), static_cast<uint32_t>(X(c, 2)))); }
void svc_arbitrate_unlock(::Ctx* c) { svc_ret(c, arbitrate_unlock(X(c, 0))); }
void svc_wait_pwk(::Ctx* c) { svc_ret(c, wait_process_wide_key(X(c, 0), X(c, 1), static_cast<uint32_t>(X(c, 2)), static_cast<int64_t>(X(c, 3)))); }
void svc_signal_pwk(::Ctx* c) { signal_process_wide_key(X(c, 0), static_cast<int32_t>(X(c, 1))); }   // returns void on Horizon: x0 untouched
void svc_get_system_tick(::Ctx* c) { X(c, 0) = system_tick(); }
void svc_wait_for_address(::Ctx* c) { svc_ret(c, wait_for_address(X(c, 0), static_cast<uint32_t>(X(c, 1)), static_cast<int32_t>(X(c, 2)), static_cast<int64_t>(X(c, 3)))); }
void svc_signal_to_address(::Ctx* c) { svc_ret(c, signal_to_address(X(c, 0), static_cast<uint32_t>(X(c, 1)), static_cast<int32_t>(X(c, 2)), static_cast<int32_t>(X(c, 3)))); }
void svc_nop_ok(::Ctx* c) { svc_ret(c, kOk); }
void svc_nop_void(::Ctx*) {}

// svcBreak(reason, arg, size): the guest aborted. Bit 31 of reason = "notification only" (the process continues). Otherwise this is
// fatal: log what the SDK said (an nn::Result for reason 4 with size 4) and raise the guest-break exception so the crash handler
// prints the guest context.
void svc_break(::Ctx* c) {
    uint32_t reason = static_cast<uint32_t>(X(c, 0));
    uint64_t arg = X(c, 1), size = X(c, 2);
    bool notify_only = (reason & 0x80000000u) != 0;
    char extra[96] = "";
    // SDK 20's nn::diag abort passes the Result with reason 0 (Panic), older SDKs with reason 4: any 4-byte argument is shown.
    if (size == 4 && arg && mem::is_mapped(arg, 4)) {
        uint32_t r = *reinterpret_cast<uint32_t*>(arg);
        snprintf(extra, sizeof extra, " (4-byte argument 0x%x: as a Result module %u, description %u)", r, r & 0x1FF, (r >> 9) & 0x1FFF);
    }
    NXLOG_E("svc", "svcBreak reason 0x%x arg 0x%llx size 0x%llx%s lr=0x%llx in f_%llx", reason, (unsigned long long)arg, (unsigned long long)size, extra,
            (unsigned long long)X(c, 30), (unsigned long long)c->fn);
    if (notify_only) return;
    NXLOG_I("runtime", "state at svcBreak:\n%s", summary().c_str());
    log_flush();
    RaiseException(0xE0000003, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    TerminateProcess(GetCurrentProcess(), 2);
}

void svc_output_debug_string(::Ctx* c) {       // x0 pointer, x1 length
    uint64_t p = X(c, 0), n = X(c, 1);
    if (n > 1024) n = 1024;
    if (n && !mem::is_mapped(p, n)) return svc_ret(c, kInvalidPointer);
    std::string s(reinterpret_cast<const char*>(p), static_cast<size_t>(n));
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '\0')) s.pop_back();
    NXLOG_I("guest", "%s", s.c_str());
    svc_ret(c, kOk);
}

void svc_return_from_exception(::Ctx* c) {
    NXLOG_E("svc", "svcReturnFromException(0x%llx): guest exception handlers are not supported", (unsigned long long)X(c, 0));
    log_flush();
    RaiseException(0xE0000003, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

}  // namespace

void svc_register_thread_group() {
    svc_register(0x07, svc_exit_process);
    svc_register(0x08, svc_create_thread);
    svc_register(0x09, svc_start_thread);
    svc_register(0x0A, svc_exit_thread);
    svc_register(0x0B, svc_sleep_thread);
    svc_register(0x0C, svc_get_thread_priority);
    svc_register(0x0D, svc_set_thread_priority);
    svc_register(0x0E, svc_get_thread_core_mask);
    svc_register(0x0F, svc_set_thread_core_mask);
    svc_register(0x10, svc_get_current_processor_number);
    svc_register(0x11, svc_signal_event);
    svc_register(0x12, svc_clear_event);
    svc_register(0x16, svc_close_handle);
    svc_register(0x17, svc_reset_signal);
    svc_register(0x18, svc_wait_synchronization);
    svc_register(0x19, svc_cancel_synchronization);
    svc_register(0x1A, svc_arbitrate_lock);
    svc_register(0x1B, svc_arbitrate_unlock);
    svc_register(0x1C, svc_wait_pwk);
    svc_register(0x1D, svc_signal_pwk);
    svc_register(0x1E, svc_get_system_tick);
    svc_register(0x26, svc_break);
    svc_register(0x27, svc_output_debug_string);
    svc_register(0x28, svc_return_from_exception);
    svc_register(0x32, svc_nop_ok);          // SetThreadActivity: threads run until they exit
    svc_register(0x34, svc_wait_for_address);
    svc_register(0x35, svc_signal_to_address);
    svc_register(0x36, svc_nop_void);        // SynchronizePreemptionState
    svc_register(0x45, svc_create_event);
}

}  // namespace nxrt
