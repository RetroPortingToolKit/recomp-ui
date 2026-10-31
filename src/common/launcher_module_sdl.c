#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "launcher_module_sdl.h"
#include "launcher_module_contract.h"
#include "sha256.h"
#if defined(_WIN32)
/* SDL's consumer declarations can otherwise point at a PE import jump stub
 * in this EXE/DLL. Read this consumer's actual import addresses instead;
 * never resolve a DLL by name or load a second copy for identity checks. */
#ifdef SDL_DECLSPEC
#undef SDL_DECLSPEC
#endif
#define SDL_DECLSPEC __declspec(dllimport)
#endif
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>
#endif

uint32_t launcher_v2_sdl_identity(RecompLauncherV2SDL* out, char* diagnostic, uint32_t capacity) {
    RecompLauncherV2SDL identity = {0};
    FILE* file = NULL;
    if (!out) goto refused;
    identity.header = launcher_v2_header(sizeof(identity));
    identity.version = (uint32_t)SDL_GetVersion();
    if (identity.version < UINT32_C(3002000) || identity.version / UINT32_C(1000000) != 3) goto refused;
    identity.get_version_address = (uint64_t)(uintptr_t)&SDL_GetVersion;
    identity.get_joystick_address = (uint64_t)(uintptr_t)&SDL_GetJoystickFromID;
#if defined(_WIN32)
    HMODULE version_provider = NULL, joystick_provider = NULL;
    wchar_t wide_path[1024];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(uintptr_t)&SDL_GetVersion, &version_provider) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(uintptr_t)&SDL_GetJoystickFromID, &joystick_provider) || version_provider != joystick_provider ||
        version_provider == GetModuleHandleW(NULL)) goto refused;
    DWORD length = GetModuleFileNameW(version_provider, wide_path, 1024);
    if (!length || length >= 1024 ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_path, -1,
                            identity.provider_path, sizeof(identity.provider_path), NULL, NULL)) goto refused;
    file = _wfopen(wide_path, L"rb");
#else
    Dl_info version_provider = {0}, joystick_provider = {0};
    char resolved[PATH_MAX];
    if (!dladdr((void*)(uintptr_t)&SDL_GetVersion, &version_provider) ||
        !dladdr((void*)(uintptr_t)&SDL_GetJoystickFromID, &joystick_provider) ||
        version_provider.dli_fbase != joystick_provider.dli_fbase || !version_provider.dli_fname ||
        !realpath(version_provider.dli_fname, resolved) || strlen(resolved) >= sizeof(identity.provider_path)) goto refused;
    /* A statically linked SDL has its symbols in the host/module itself.
     * Require the provider basename as well as the actual shared target at
     * configure time. Local visibility prevents module/helper interposition. */
    const char* name = strrchr(resolved, '/');
    name = name ? name + 1 : resolved;
    if (!strstr(name, "SDL3") || (!strstr(name, ".so") && !strstr(name, ".dylib"))) goto refused;
    snprintf(identity.provider_path, sizeof(identity.provider_path), "%s", resolved);
    file = fopen(resolved, "rb");
#endif
    if (!file || fseek(file, 0, SEEK_END) != 0) goto refused;
    long bytes = ftell(file);
    /* Existing one-shot SHA helper; a bounded SDL artifact, no new hashing
     * dependency. Never allocate from a host-provided size. */
    if (bytes <= 0 || bytes > 64L * 1024L * 1024L || fseek(file, 0, SEEK_SET) != 0) goto refused;
    uint8_t* buffer = (uint8_t*)malloc((size_t)bytes);
    if (!buffer) goto refused;
    const int complete = fread(buffer, 1, (size_t)bytes, file) == (size_t)bytes && !ferror(file);
    fclose(file);
    file = NULL;
    if (complete) recompui_sha256_compute(buffer, (size_t)bytes, identity.sha256);
    free(buffer);
    if (!complete) goto refused;
    *out = identity;
    return RECOMP_LAUNCHER_V2_OK;
refused:
    if (file) fclose(file);
    launcher_v2_diagnostic(diagnostic, capacity, "Cannot identify the loaded shared SDL3 provider; launcher load refused before callbacks or settings edits.");
    return RECOMP_LAUNCHER_V2_BAD_SDL;
}

