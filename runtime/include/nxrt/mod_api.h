/* SPDX-License-Identifier: GPL-3.0-or-later
 * Code mods as DLLs: the interface between the runtime and a mod (services/mods_dll.cpp loads them).
 *
 * A mod is a folder under [fs] mods in the layout emulators use. Its exefs/ (where an emulator mod keeps IPS patches, which are not
 * applied here because the game code is compiled ahead of time) may also hold DLLs: every *.dll there is loaded when play mode starts,
 * in the order of the mods (sorted by name, as for RomFS), and its export
 *
 *     extern "C" __declspec(dllexport) int nxmod_init(const NxModApi* api, const char* mod_dir);
 *
 * is called once at the game's first frame boundary, on the main thread, with mod_dir = the mod's folder (UTF-8). By then the game's
 * modules are mapped and relocated, so nxmod_init can check the game's code and data (e.g. a signature check of the build it targets).
 * Nonzero = the mod declines (wrong game build, ...): its hooks are removed and it gets no frame calls. So one mod folder can carry both an emulator's IPS and this runtime's DLL.
 *
 * Plain C, so a mod can be built with any compiler. The table only grows: a mod checks api->version >= the version it was written for, and
 * api->size before reading a field added later. Guest state is the lifted code's Ctx (recomp/include/nxrecomp/ctx.h).
 *
 * What a mod can do:
 *   - attach to the hook sites the game was lifted with (recomp/games/<game>/main.sites): inline, midhook and entry hooks, the same as
 *     the runtime's own features (nxrt/hooks.h). A site that is not in that list is never reached: a new site needs a change to
 *     main.sites (and a Set up run) like any other hook.
 *   - call any lifted guest function (call_guest) from a hook or the frame callback, on the thread and Ctx it was given.
 *   - read and write guest memory directly: guest addresses are host addresses (main is at main_base); is_mapped checks a range first.
 *   - run once per presented frame (on_frame), on the main thread with its Ctx.
 *
 * Loading can be turned off with [fs] dll_mods = false. A DLL runs with the same rights as the game: install only mods you trust.
 */
#ifndef NXRT_MOD_API_H
#define NXRT_MOD_API_H

#include <stdint.h>

#include <nxrecomp/ctx.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXMOD_API_VERSION 1u

typedef void (*NxModInlineFn)(Ctx* c, uint64_t addr, void* user);
typedef int (*NxModSiteFn)(Ctx* c, uint64_t addr, void* user);   /* midhook / entry hook: nonzero = handled (nxrt/hooks.h) */
typedef void (*NxModFrameFn)(Ctx* c, void* user);

typedef struct NxModApi {
    uint32_t version;            /* NXMOD_API_VERSION of the runtime */
    uint32_t size;               /* sizeof(NxModApi) of the runtime */
    const char* game_id;         /* the game module's id, e.g. "fefw" */
    uint64_t main_base;          /* guest address of main's image */

    /* a log line under the tag "mod", naming the mod: pass the api pointer the mod was given (each mod gets its own table).
     * level 0 error, 1 warning, 2 info, 3 debug */
    void (*log)(const struct NxModApi* api, int level, const char* text);

    /* hooks at lifted sites (absolute guest addresses); each returns an id, 0 on failure. All of a mod's hooks are removed if it declines. */
    uint32_t (*hook_inline)(uint64_t addr, NxModInlineFn fn, void* user);
    uint32_t (*hook_mid)(uint64_t addr, NxModSiteFn fn, void* user);
    uint32_t (*hook_entry)(uint64_t addr, NxModSiteFn fn, void* user);
    int (*hook_remove)(uint32_t id);

    /* Calls the lifted function at addr with x0..x7 = args[0..nargs-1] and returns x0. Every register of c is as it was afterwards (the
     * call runs below c's stack pointer). Only from a hook or on_frame, with the Ctx they were given. An address that is not a lifted
     * function (is_function) is not called: a warning is logged and 0 returned. The function must return normally: a guest C++
     * exception that escapes it is not caught here. */
    uint64_t (*call_guest)(Ctx* c, uint64_t addr, const uint64_t* args, int nargs);

    /* 1 when every byte of [addr, addr + size) is mapped */
    int (*is_mapped)(uint64_t addr, uint64_t size);

    /* fn(c, user) once per presented frame on the main thread (the game's frame boundary), in the order registered */
    void (*on_frame)(NxModFrameFn fn, void* user);

    /* 1 when addr is the entry of a lifted guest function (or a host import thunk), i.e. something call_guest can call */
    int (*is_function)(uint64_t addr);
} NxModApi;

typedef int (*NxModInitFn)(const NxModApi* api, const char* mod_dir);

#ifdef __cplusplus
}
#endif

#endif /* NXRT_MOD_API_H */
