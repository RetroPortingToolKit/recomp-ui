// consoles/n64/n64_pad_binds.h — N64 gamepad bind persistence bridge.
//
// The same per-GUID input.ini store PSX and SNES use (common/pad_binds.h);
// this unit supplies only the N64 button table, the defaults, and the banner:
//
//   [gamepads]           guid = display name
//   [mapping.<guid>]     button sources + deadzone (percent) + name_custom
//
// WHY a second store when consoles/n64/n64_binds.c already persists binds.
// input.cfg is a per-device-TYPE table: ONE `pad.*` set shared by every
// controller ever plugged in. Two players on different pads cannot have
// different layouts, and swapping a controller silently inherits the other
// one's mapping. Identity is the SDL joystick GUID, which is what this store
// keys on — the same reason psxrecomp and snesrecomp moved.
//
// The split is the PSX split, not a new one: the KEYBOARD half stays in the
// console's native file (input.cfg, device 0), because that is the format the
// N64 runners parse; only the GAMEPAD half moves here.
//
// The launcher still MIRRORS each pad bind into input.cfg's `pad.*` table
// (launcher_binds.c), because the RT64-era N64 runners read that file and
// nothing else. The mirror is lossy — one table for every controller — which
// is the defect this store exists to fix, not a second source of truth: what
// the page shows, and what n64lle's host evaluates, is this file.
//
// One capability does not survive the move: input.cfg can name a RAW joystick
// button or axis (`joybtn:`/`joyaxis+:`), for pads whose SDL_GameController
// mapping cannot express an input. This store's vocabulary is SDL gamepad
// names, so a raw field cannot be captured here. n64lle builds on SDL3, where
// that capture path was already compiled out.
//
// Button indices are the kN64PadButtons rebind-spec order (n64_profile.h),
// which IS PSR's N64Input enum order — the identity mapping n64_binds.c and
// the host both address by. No second order exists for this console.

#ifndef RUI_CONSOLE_N64_PAD_BINDS_H
#define RUI_CONSOLE_N64_PAD_BINDS_H

#ifdef __cplusplus
extern "C" {
#endif

// Matches the host's own fallback (n64lle runtime/host/host_input.c), so a pad
// with no saved profile behaves the same whichever side supplied the number.
#define RUI_N64_PAD_DEFAULT_DEADZONE_PCT 15

// Resolve input.ini beside `bind_cfg_path` (the resolved input.cfg). Both the
// launcher and the game's host process call this; one rule, one location.
void rui_n64_pad_binds_path(const char* bind_cfg_path, char* out, int cap);

// Button `b` is 0..LNG_N64_PAD_BUTTON_COUNT-1 (n64_profile.h). The count is
// NOT restated here: one definition, derived from the button table itself.
void rui_n64_pad_binds_init(const char* path);

// The stored value IS the SDL source name ("a", "dpup", "lefty-"), so this is
// both the chip text the launcher shows and the string the host evaluates —
// there is no second, prettier spelling that could disagree with the file.
void rui_n64_pad_binds_source(const char* path, const char* guid, int b,
                              char* out, int cap);
void rui_n64_pad_binds_set(const char* path, const char* guid, int b,
                           int kind, int code, int axis_dir);
void rui_n64_pad_binds_reset(const char* path, const char* guid);

void rui_n64_pad_binds_remember(const char* path, const char* guid,
                                const char* name, int deadzone_pct);
void rui_n64_pad_binds_save_profile(const char* path, const char* guid,
                                    const char* name, int name_custom,
                                    int deadzone_pct);
void rui_n64_pad_binds_rename(const char* path, const char* guid,
                              const char* name);
void rui_n64_pad_binds_delete(const char* path, const char* guid);

int  rui_n64_pad_binds_known_count(const char* path);
int  rui_n64_pad_binds_known_at(const char* path, int index,
                                char* guid, int guid_cap,
                                char* name, int name_cap);

void rui_n64_pad_binds_name(const char* path, const char* guid,
                            char* out, int cap);
int  rui_n64_pad_binds_name_is_custom(const char* path, const char* guid);
int  rui_n64_pad_binds_deadzone(const char* path, const char* guid);

#ifdef __cplusplus
}
#endif

#endif // RUI_CONSOLE_N64_PAD_BINDS_H
