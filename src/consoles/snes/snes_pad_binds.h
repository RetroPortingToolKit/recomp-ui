// consoles/snes/snes_pad_binds.h — SNES gamepad bind persistence bridge.
//
// Same per-GUID input.ini store every console uses (common/pad_binds.h); this
// unit only supplies the SNES button table and defaults.
//
//   [gamepads]           guid = display name
//   [mapping.<guid>]     button sources + deadzone (percent) + name_custom
//
// Button ORDER is the runner's, not the launcher's display order: the runner
// documents config.ini [Controls] as
//     Up Down Left Right Select Start A B X Y L R
// and input.ini is keyed by NAME, so the two orders never have to agree.
//
// Defaults match the runner's built-in gamepad map exactly (mmx_config.c
// kDefaultGamepadCmds and the shipped "Controls =" line), which swaps the face
// buttons against SDL's layout: SNES A is SDL b, SNES B is SDL a, SNES X is
// SDL y, SNES Y is SDL x. Inventing a different default here would silently
// disagree with a runtime that has no input.ini yet.
//
// NOTE: the snesrecomp runner does not read input.ini yet — it still uses
// config.ini [GamepadMap], which has no per-device identity. Until that lands,
// what this writes is remembered by the launcher but not consumed in-game.

#ifndef RUI_CONSOLE_SNES_PAD_BINDS_H
#define RUI_CONSOLE_SNES_PAD_BINDS_H

#ifdef __cplusplus
extern "C" {
#endif

/* snes_profile.h derives the same count from its button table. Both say 12;
 * the guard is so a translation unit may include both headers (launcher_binds.c
 * wants the deadzone default and the profile) without a redefinition warning. */
#ifndef LNG_SNES_PAD_BUTTON_COUNT
#define LNG_SNES_PAD_BUTTON_COUNT 12
#endif
#define RUI_SNES_PAD_DEFAULT_DEADZONE_PCT 10

void rui_snes_pad_binds_init(const char* path);

void rui_snes_pad_binds_label(const char* path, const char* guid, int b,
                              char* out, int cap);
void rui_snes_pad_binds_set(const char* path, const char* guid, int b,
                            int kind, int code, int axis_dir);
void rui_snes_pad_binds_reset(const char* path, const char* guid);

void rui_snes_pad_binds_remember(const char* path, const char* guid,
                                 const char* name, int deadzone_pct);
void rui_snes_pad_binds_save_profile(const char* path, const char* guid,
                                     const char* name, int name_custom,
                                     int deadzone_pct);
void rui_snes_pad_binds_rename(const char* path, const char* guid,
                               const char* name);
void rui_snes_pad_binds_delete(const char* path, const char* guid);

int  rui_snes_pad_binds_known_count(const char* path);
int  rui_snes_pad_binds_known_at(const char* path, int index,
                                 char* guid, int guid_cap,
                                 char* name, int name_cap);

void rui_snes_pad_binds_name(const char* path, const char* guid,
                             char* out, int cap);
int  rui_snes_pad_binds_name_is_custom(const char* path, const char* guid);
int  rui_snes_pad_binds_deadzone(const char* path, const char* guid);

// Convert one legacy config.ini [GamepadMap] token ("DpadUp", "Lb", "A", ...)
// to the SDL controller name this store persists ("dpup", "leftshoulder",
// "a"). Case-insensitive. Returns NULL when the token is not one the runner
// recognises, so a migration can report it instead of writing a guess.
//
// The two vocabularies are genuinely different — the runner's own parser
// (mmx_config.c ParseGamepadButtonName) accepts both "L1" and "Lb" for the
// same physical button — which is why a migration cannot just copy strings.
const char* rui_snes_pad_binds_legacy_token(const char* legacy);

// Map a launcher button index (snes_profile.h ControllerSpec order:
// Up Down Left Right A B X Y L R Start Select) onto this store's index
// (the runner's [Controls] order). Returns -1 when out of range.
int  rui_snes_pad_binds_index_from_ui(int ui_button);

#ifdef __cplusplus
}
#endif

#endif // RUI_CONSOLE_SNES_PAD_BINDS_H
