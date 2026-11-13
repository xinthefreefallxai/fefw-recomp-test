// SPDX-License-Identifier: GPL-3.0-or-later
// Shared vocabulary of the audio renderer port (S3c). The renderer is a port of Ryujinx.Audio (Ryubing fork, MIT) to C++; this
// header holds the few C# idioms every ported file uses, so that classes written by different people fit together.
//
// PORTING CONVENTIONS (every file under src/services/audren/ follows them)
//   * Namespace nxrt::audren for everything (the parameter structs in audren_types.h are there already). One C# file -> one header
//     `src/services/audren/<Folder>/<Name>.h` (folders as in Ryujinx.Audio/Renderer: Common, Dsp, Dsp/Command, Dsp/State, Dsp/Effect, Server,
//     Server/Effect, ...); header-only (inline functions, classes defined in the header) unless the file is big, then a matching .cpp.
//   * Names are kept exactly: classes, methods, enumerators and PUBLIC FIELDS keep their C# PascalCase spelling. A C# auto-property
//     (`T X { get; set; }`) becomes a public data member `T X;`. A C# property with a body (`=> expr` / `get { ... }`) becomes a method
//     `T X() const`. Private fields `_name` stay `_name`. Method parameters keep their C# order; `ref`/`out` become references.
//     So code written in one place can call code written in another by reading the C# signature only.
//     EXCEPTION: the plain parameter structs of audren_types.h (the binary layouts) use lower_snake_case field names; use those as they are.
//   * C# `uint/int/ulong/long/ushort/short/byte/sbyte/float/double/bool` -> uint32_t/int32_t/uint64_t/int64_t/uint16_t/int16_t/uint8_t/
//     int8_t/float/double/bool. `CpuAddress` / `DspAddress` = uint64_t. C# `Span<T>`, `ReadOnlySpan<T>`, `Memory<T>`, `ReadOnlyMemory<T>` ->
//     Span<T> below (const-ness by T). `T[]` that is allocated once -> std::vector<T> member (or Span<T> into a work buffer when the C#
//     takes the memory from the guest work buffer: see WorkBufferAllocator).
//   * Guest memory: Ryujinx reads the guest through IVirtualMemoryManager. Here guest pointers are host pointers, so
//     `memoryManager.GetSpan(address, size)` / `Read/Write` become `GuestSpan<T>(address, count)` / direct dereference. Drop the memoryManager parameter from signatures.
//   * `unsafe` pointer code and `MemoryMarshal.Cast<A,B>(span)` -> Span<A>::Cast<B>(). `Unsafe.SizeOf<T>()` -> sizeof(T). `BitOperations` ->
//     helpers below or <bit>. `Math.Min/Max/Clamp` -> std::min/std::max/ClampT. `MathF.*` -> std::*f. `Debug.Assert` -> NXAUDREN_ASSERT.
//   * No exceptions for control flow: C# `throw new InvalidOperationException` -> NXAUDREN_FATAL("message") (logs and aborts).
//   * ResultCode / ErrorCode return values: use the `ResultCode` enum already ported in audren_types.h (a uint32_t result), with the Ryujinx names.
//   * No SIMD intrinsics: where Ryujinx has AVX/SSE/AdvSimd variants plus a scalar "Slow" variant, port the scalar one and call it from the dispatcher.
#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#include <nxrt/log.h>
#include <vector>

namespace nxrt::audren {

using CpuAddress = uint64_t;
using DspAddress = uint64_t;

[[noreturn]] inline void FatalImpl(const char* file, int line, const char* msg) {
    std::fprintf(stderr, "audren fatal: %s (%s:%d)\n", msg, file, line);
    NXLOG_E("audren", "fatal: %s (%s:%d)", msg, file, line);   // the crash report (abort) follows in the log
    nxrt::log_flush();
    std::abort();
}
#define NXAUDREN_FATAL(msg) ::nxrt::audren::FatalImpl(__FILE__, __LINE__, msg)
#ifndef NDEBUG
#define NXAUDREN_ASSERT(cond) \
    do {                      \
        if (!(cond)) ::nxrt::audren::FatalImpl(__FILE__, __LINE__, "assertion failed: " #cond); \
    } while (0)
#else
#define NXAUDREN_ASSERT(cond) \
    do {                      \
    } while (0)
#endif

// A view of `Length` contiguous elements (C# Span<T> / ReadOnlySpan<T> / Memory<T>). Does not own the memory.
template <class T>
struct Span {
    T* Data = nullptr;
    size_t Length = 0;

    Span() = default;
    Span(T* data, size_t length) : Data(data), Length(length) {}
    template <class U, class = std::enable_if_t<std::is_same_v<std::remove_const_t<T>, U>>>
    Span(std::vector<U>& v) : Data(v.data()), Length(v.size()) {}
    template <class U, class = std::enable_if_t<std::is_const_v<T> && std::is_same_v<std::remove_const_t<T>, U>>>
    Span(const std::vector<U>& v) : Data(v.data()), Length(v.size()) {}
    operator Span<const T>() const { return Span<const T>(Data, Length); }

    bool IsEmpty() const { return Length == 0; }
    T& operator[](size_t i) const {
        NXAUDREN_ASSERT(i < Length);
        return Data[i];
    }
    Span Slice(size_t start) const {
        NXAUDREN_ASSERT(start <= Length);
        return Span(Data + start, Length - start);
    }
    Span Slice(size_t start, size_t length) const {
        NXAUDREN_ASSERT(start <= Length && length <= Length - start);
        return Span(Data + start, length);
    }
    void Fill(const T& v) const { std::fill(Data, Data + Length, v); }
    void Clear() const { if (Length) std::memset(static_cast<void*>(const_cast<std::remove_const_t<T>*>(Data)), 0, Length * sizeof(T)); }
    // CopyTo: dst must hold at least Length elements.
    void CopyTo(Span<std::remove_const_t<T>> dst) const {
        NXAUDREN_ASSERT(dst.Length >= Length);
        if (Length) std::memmove(static_cast<void*>(dst.Data), static_cast<const void*>(Data), Length * sizeof(T));
    }
    // MemoryMarshal.Cast<T, U>
    template <class U>
    Span<U> Cast() const {
        return Span<U>(reinterpret_cast<U*>(Data), Length * sizeof(T) / sizeof(U));
    }
    T* begin() const { return Data; }
    T* end() const { return Data + Length; }
};

// Guest memory is host memory: a guest address becomes a pointer.
template <class T>
inline Span<T> GuestSpan(CpuAddress address, size_t count) {
    return Span<T>(reinterpret_cast<T*>(static_cast<uintptr_t>(address)), count);
}
template <class T>
inline T* GuestPtr(CpuAddress address) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(address));
}

template <class T>
inline T ClampT(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace nxrt::audren
