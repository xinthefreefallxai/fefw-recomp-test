// SPDX-License-Identifier: GPL-3.0-or-later
// Itanium C++ ABI typeinfo matching: the personality routine's decision "does this handler catch the thrown type" (can_catch) over the
// guest's std::type_info objects, read in place. Covers exact types, derived-to-base class catches (single, multiple and virtual
// inheritance, ambiguity and access: only an unambiguous PUBLIC base matches), pointer catches with qualification conversion, pointer
// to class and void*, pointers to pointers, nullptr_t, pointers to members, and catch(...).
//
// Written from the Itanium C++ ABI (section 2.9.5, "Exception Handling": __cxa_type_match semantics) and from the behaviour of LLVM
// libc++abi's private_typeinfo.cpp (__class_type_info::can_catch, __pointer_type_info::can_catch, has_unambiguous_public_base,
// process_found_base_class; Apache-2.0 with LLVM exception, https://github.com/llvm/llvm-project/blob/main/libcxxabi/src/private_typeinfo.cpp,
// read 2026-10-21). The search keeps libc++abi's bookkeeping (a virtual-base cookie when there is no object to inspect, "most public
// path wins" when the same subobject is reached twice) so the results agree; the code is a reimplementation over guest memory.
#include <cstdio>
#include <cstring>

#include "nxrt/eh.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/nxmodule.h"

namespace nxrt::eh {
namespace {

TypeinfoVtables g_vt;

inline uint64_t rd64(uint64_t a) {
    uint64_t v;
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<uintptr_t>(a)), 8);
    return v;
}
inline uint32_t rd32(uint64_t a) {
    uint32_t v;
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<uintptr_t>(a)), 4);
    return v;
}

// layouts (LP64): all typeinfos start {vptr, const char* name}
constexpr uint64_t kOffName = 8;
// ARM64 libc++ sets bit 63 of the name pointer to mark a name that is not unique (compare by string); the SDK's typeinfos do
// (refs/ssbu-recomp/runtime/nncxxrt.c: "the top bit marks a non-unique name").
constexpr uint64_t kNonUniqueBit = 1ull << 63;
inline uint64_t name_ptr(uint64_t ti) { return rd64(ti + kOffName) & ~kNonUniqueBit; }
constexpr uint64_t kOffSiBase = 16;                       // __si_class_type_info::__base_type
[[maybe_unused]] constexpr uint64_t kOffVmiFlags = 16, kOffVmiCount = 20, kOffVmiBases = 24;     // __vmi_class_type_info; each base {type*, long offset_flags} = 16 bytes
constexpr uint64_t kOffPbaseFlags = 16, kOffPbasePointee = 24, kOffPmContext = 32;
constexpr uint64_t kBaseVirtual = 1, kBasePublic = 2, kBaseOffsetShift = 8;
constexpr uint32_t kConst = 1, kNoRemove = 1 | 2 | 4, kNoAdd = 0x20 | 0x40;

enum Path { kUnknown = 0, kPublic = 1, kNotPublic = 2 };

bool is_named(uint64_t ti, const char* name) { return std::strcmp(ti_name(ti), name) == 0; }

// ---- derived-to-base search (libc++abi has_unambiguous_public_base) -------------------------------------------------------------------
struct Search {
    uint64_t target;                // typeinfo of the class the handler names
    bool have_object;               // true: look through the object's vtables for virtual bases
    uint64_t found_ptr = 0;         // address of the matching subobject (first find)
    uint64_t found_cookie = 0;
    uint64_t cookie = 0;            // name of the innermost virtual base on the path (no object only)
    int number = 0;                 // finds so far (distinct subobjects)
    int path = kUnknown;
    bool done = false;
    int budget = 4096;              // guard against pathological/corrupt hierarchies (visits, and depth below)
    int depth = 0;
};

void found(Search& s, uint64_t ptr, int path_below) {
    if (s.number == 0) {
        s.found_ptr = ptr;
        s.path = path_below;
        s.found_cookie = s.cookie;
        s.number = 1;
    } else if (s.found_cookie == s.cookie && s.found_ptr == ptr) {
        if (s.path == kNotPublic) s.path = path_below;      // the same subobject on a more public path
    } else {
        s.number += 1;                                       // two different subobjects of the target type: ambiguous
        s.path = kNotPublic;
        s.done = true;
    }
}

struct DepthGuard {
    Search& s;
    explicit DepthGuard(Search& x) : s(x) { s.depth++; }
    ~DepthGuard() { s.depth--; }
};

void search(Search& s, uint64_t ti, uint64_t ptr, int path_below) {
    if (!ti || s.done || --s.budget < 0 || s.depth > 256) return;
    DepthGuard guard(s);
    if (ti_equal(ti, s.target)) { found(s, ptr, path_below); return; }
    switch (ti_kind(ti)) {
        case TiKind::SiClass:
            search(s, rd64(ti + kOffSiBase), ptr, path_below);
            break;
        case TiKind::VmiClass: {
            uint32_t count = rd32(ti + kOffVmiCount);
            if (count > 4096) return;
            for (uint32_t i = 0; i < count && !s.done; i++) {
                uint64_t e = ti + kOffVmiBases + 16ull * i;
                uint64_t base = rd64(e);
                int64_t offset_flags = static_cast<int64_t>(rd64(e + 8));
                bool is_virtual = (offset_flags & kBaseVirtual) != 0;
                int64_t off = offset_flags >> kBaseOffsetShift;     // arithmetic shift: the offset is signed
                uint64_t sub = ptr;
                uint64_t saved_cookie = s.cookie;
                if (s.have_object) {
                    if (is_virtual) off = static_cast<int64_t>(rd64(rd64(ptr) + static_cast<uint64_t>(off)));   // vbase offset stored in the vtable
                } else if (is_virtual) {
                    s.cookie = name_ptr(base);            // no object: identify the virtual base by its name pointer
                    sub = 0;
                    off = 0;
                }
                search(s, base, sub + static_cast<uint64_t>(off), (offset_flags & kBasePublic) ? path_below : kNotPublic);
                s.cookie = saved_cookie;
            }
            break;
        }
        default:
            break;                                              // a class without bases (or not a class): nothing below it
    }
}

// Is `target` an unambiguous public base of `from` (or equal)? On success *ptr is the subobject's address.
bool derived_to_base(uint64_t target, uint64_t from, uint64_t* ptr, bool have_object) {
    Search s;
    s.target = target;
    s.have_object = have_object;
    search(s, from, *ptr, kPublic);
    if (s.path == kPublic) {
        *ptr = have_object ? s.found_ptr : 0;
        return true;
    }
    return false;
}

// ---- pointers --------------------------------------------------------------------------------------------------------------------------
bool can_catch_nested(uint64_t catch_ptr_ti, uint64_t thrown_ti);

bool member_nested(uint64_t catch_ti, uint64_t thrown_ti) {
    if (ti_kind(thrown_ti) != TiKind::PointerToMember) return false;
    if (~rd32(catch_ti + kOffPbaseFlags) & rd32(thrown_ti + kOffPbaseFlags)) return false;
    return ti_equal(rd64(catch_ti + kOffPbasePointee), rd64(thrown_ti + kOffPbasePointee)) &&
           ti_equal(rd64(catch_ti + kOffPmContext), rd64(thrown_ti + kOffPmContext));
}

// __pointer_type_info::can_catch_nested: `thrown_ti` is the pointee of the thrown pointer, `catch_ptr_ti` a pointer type.
bool can_catch_nested(uint64_t catch_ptr_ti, uint64_t thrown_ti) {
    if (ti_kind(thrown_ti) != TiKind::Pointer) return false;
    uint32_t cf = rd32(catch_ptr_ti + kOffPbaseFlags), tf = rd32(thrown_ti + kOffPbaseFlags);
    if (tf & ~cf) return false;
    uint64_t cp = rd64(catch_ptr_ti + kOffPbasePointee), tp = rd64(thrown_ti + kOffPbasePointee);
    if (ti_equal(cp, tp)) return true;
    if (~cf & kConst) return false;                           // differing pointees need a const-qualified catch type at every level
    TiKind k = ti_kind(cp);
    if (k == TiKind::Pointer) return can_catch_nested(cp, tp);
    if (k == TiKind::PointerToMember) return member_nested(cp, tp);
    return false;
}

bool pointer_can_catch(uint64_t catch_ti, uint64_t thrown_ti, uint64_t* adjusted) {
    TiKind tk = ti_kind(thrown_ti);
    if (tk == TiKind::Fundamental && is_named(thrown_ti, "Dn")) { *adjusted = 0; return true; }         // bullet 4: nullptr_t
    if (ti_equal(catch_ti, thrown_ti)) {                                                                // bullet 1: same pointer type
        if (*adjusted) *adjusted = rd64(*adjusted);
        return true;
    }
    if (tk != TiKind::Pointer) return false;
    if (*adjusted) *adjusted = rd64(*adjusted);                                                         // the exception object holds the pointer
    uint32_t cf = rd32(catch_ti + kOffPbaseFlags), tf = rd32(thrown_ti + kOffPbaseFlags);
    if (tf & ~cf & kNoRemove) return false;                                                             // bullet 3B/3C: qualifiers may be added, not removed
    if (cf & ~tf & kNoAdd) return false;
    uint64_t cp = rd64(catch_ti + kOffPbasePointee), tp = rd64(thrown_ti + kOffPbasePointee);
    if (ti_equal(cp, tp)) return true;
    if (ti_kind(cp) == TiKind::Fundamental && is_named(cp, "v"))                                       // bullet 3A: any object pointer converts to void*
        return ti_kind(tp) != TiKind::Function;
    TiKind ck = ti_kind(cp);
    if (ck == TiKind::Pointer) return (cf & kConst) && can_catch_nested(cp, tp);
    if (ck == TiKind::PointerToMember) return (cf & kConst) && member_nested(cp, tp);
    if (!ti_is_class(ck) || !ti_is_class(ti_kind(tp))) return false;
    bool have_object = *adjusted != 0;
    uint64_t p = *adjusted;
    if (!derived_to_base(cp, tp, &p, have_object)) return false;
    *adjusted = have_object ? p : 0;
    return true;
}

bool member_pointer_can_catch(uint64_t catch_ti, uint64_t thrown_ti, uint64_t* adjusted) {
    static const uint64_t null_rep[2] = {0, 0};
    if (ti_kind(thrown_ti) == TiKind::Fundamental && is_named(thrown_ti, "Dn")) {
        *adjusted = reinterpret_cast<uint64_t>(&null_rep[0]);
        return true;
    }
    if (ti_equal(catch_ti, thrown_ti)) return true;
    if (ti_kind(thrown_ti) != TiKind::PointerToMember) return false;
    uint32_t cf = rd32(catch_ti + kOffPbaseFlags), tf = rd32(thrown_ti + kOffPbaseFlags);
    if (tf & ~cf & kNoRemove) return false;
    if (cf & ~tf & kNoAdd) return false;
    if (!ti_equal(rd64(catch_ti + kOffPbasePointee), rd64(thrown_ti + kOffPbasePointee))) return false;
    return ti_equal(rd64(catch_ti + kOffPmContext), rd64(thrown_ti + kOffPmContext));
}

}  // namespace

// ---- configuration and basic queries ---------------------------------------------------------------------------------------------------
void set_typeinfo_vtables(const TypeinfoVtables& v) { g_vt = v; }
const TypeinfoVtables& typeinfo_vtables() { return g_vt; }

size_t discover_typeinfo_vtables() {
    struct Item { uint64_t* slot; const char* sym; };
    TypeinfoVtables v;
    const Item items[] = {
        {&v.fundamental, "_ZTVN10__cxxabiv123__fundamental_type_infoE"},
        {&v.array, "_ZTVN10__cxxabiv117__array_type_infoE"},
        {&v.function, "_ZTVN10__cxxabiv120__function_type_infoE"},
        {&v.enumeration, "_ZTVN10__cxxabiv116__enum_type_infoE"},
        {&v.klass, "_ZTVN10__cxxabiv117__class_type_infoE"},
        {&v.si_class, "_ZTVN10__cxxabiv120__si_class_type_infoE"},
        {&v.vmi_class, "_ZTVN10__cxxabiv121__vmi_class_type_infoE"},
        {&v.pointer, "_ZTVN10__cxxabiv119__pointer_type_infoE"},
        {&v.pointer_to_member, "_ZTVN10__cxxabiv129__pointer_to_member_type_infoE"},
    };
    size_t n = 0;
    for (const Item& it : items) {
        uint64_t a = module_find_symbol(it.sym);
        if (!a) a = nxmodule_find_export(it.sym);
        if (a) { *it.slot = a; n++; }
        else NXLOG_D("eh", "typeinfo vtable symbol %s not found: that kind of typeinfo will not be recognised", it.sym);
    }
    g_vt = v;
    NXLOG_I("eh", "typeinfo vtables: %zu of 9 symbols found (class %s)", n, v.klass ? "yes" : "NO: derived-to-base catches cannot work");
    return n;
}

TiKind ti_kind(uint64_t ti) {
    if (!ti) return TiKind::Unknown;
    uint64_t vp = rd64(ti);
    if (!vp) return TiKind::Unknown;
    auto is = [&](uint64_t sym) { return sym && vp == sym + 16; };
    if (is(g_vt.klass)) return TiKind::Class;
    if (is(g_vt.si_class)) return TiKind::SiClass;
    if (is(g_vt.vmi_class)) return TiKind::VmiClass;
    if (is(g_vt.pointer)) return TiKind::Pointer;
    if (is(g_vt.fundamental)) return TiKind::Fundamental;
    if (is(g_vt.enumeration)) return TiKind::Enum;
    if (is(g_vt.pointer_to_member)) return TiKind::PointerToMember;
    if (is(g_vt.array)) return TiKind::Array;
    if (is(g_vt.function)) return TiKind::Function;
    return TiKind::Unknown;
}

const char* ti_name(uint64_t ti) {
    if (!ti) return "";
    uint64_t n = name_ptr(ti);
    return n ? reinterpret_cast<const char*>(static_cast<uintptr_t>(n)) : "";
}

bool ti_equal(uint64_t a, uint64_t b) {
    if (a == b) return true;
    if (!a || !b) return false;
    uint64_t na = name_ptr(a), nb = name_ptr(b);
    if (!na || !nb) return false;
    if (na == nb) return true;
    const char* sa = reinterpret_cast<const char*>(static_cast<uintptr_t>(na));
    const char* sb = reinterpret_cast<const char*>(static_cast<uintptr_t>(nb));
    if (sa[0] == '*' || sb[0] == '*') return false;           // internal linkage: only the same object is the same type
    return std::strcmp(sa, sb) == 0;
}

std::string ti_demangle_hint(uint64_t ti) {
    static const char* const kinds[] = {"unknown kind", "fundamental", "array", "function", "enum", "class", "class (single base)", "class (multiple bases)", "pointer", "pointer to member"};
    char buf[160];
    snprintf(buf, sizeof buf, "%s (%s, typeinfo 0x%llx)", ti_name(ti), kinds[static_cast<int>(ti_kind(ti))], static_cast<unsigned long long>(ti));
    return buf;
}

bool can_catch(uint64_t catch_ti, uint64_t thrown_ti, uint64_t* adjusted) {
    if (!catch_ti) return true;                               // catch (...)
    if (!thrown_ti) return false;                             // foreign exception: only catch (...)
    uint64_t scratch = 0;
    if (!adjusted) adjusted = &scratch;
    switch (ti_kind(catch_ti)) {
        case TiKind::Fundamental:
        case TiKind::Enum:
            return ti_equal(catch_ti, thrown_ti);
        case TiKind::Array:
        case TiKind::Function:
            return false;                                     // never thrown, so never caught
        case TiKind::Class:
        case TiKind::SiClass:
        case TiKind::VmiClass: {
            if (ti_equal(catch_ti, thrown_ti)) return true;   // bullet 1
            if (!ti_is_class(ti_kind(thrown_ti))) return false;
            uint64_t p = *adjusted;                           // bullet 2: unambiguous public base of the thrown class
            if (!derived_to_base(catch_ti, thrown_ti, &p, true)) return false;
            *adjusted = p;
            return true;
        }
        case TiKind::Pointer:
            return pointer_can_catch(catch_ti, thrown_ti, adjusted);
        case TiKind::PointerToMember:
            return member_pointer_can_catch(catch_ti, thrown_ti, adjusted);
        case TiKind::Unknown:
        default:
            return ti_equal(catch_ti, thrown_ti);             // a kind we cannot read: identity is all we can say
    }
}

}  // namespace nxrt::eh
