// consoles/psx/psx_kb_profiles.h - named keyboard profiles for PlayStation
// titles.
//
// A profile is one player's keyboard map under a name. It is a plain text
// file, <name>.ini, with one [player1] section in the keybinds.ini format
// (psx_binds.h), in one folder that every psxrecomp game of the same user
// reads. So a set of keys made in one game is there in the next, and a file
// copied into the folder is a profile.
//
// "Default" is not a file. It is the keyboard map psxrecomp starts with. It is
// always listed first and cannot be replaced, renamed or deleted, whatever its
// letter case; a file named Default.ini in the folder is not read.
//
// Keyboard only: a pad keeps its settings per controller in input.ini.
//
// A name is 1 to 32 characters: ASCII letters, digits, space and - _ . ( ) +.
// It does not start or end with a space or a dot and is not a device name of
// Windows (CON, NUL, COM1 ...), so that the same name is a file name on
// Windows, Linux and macOS. Two names that differ only in letter case are
// the same profile.
//
// Players are 0-based, as in psx_binds.h. `keybinds_path` is the resolved
// keybinds.ini of the running title.

#ifndef RUI_CONSOLE_PSX_KB_PROFILES_H
#define RUI_CONSOLE_PSX_KB_PROFILES_H

#ifdef __cplusplus
extern "C" {
#endif

#define RUI_PSX_KB_PROFILE_NAME_MAX 32
#define RUI_PSX_KB_PROFILE_DEFAULT  "Default"

typedef enum RuiPsxKbProfileResult {
    RUI_PSX_KBP_OK = 0,
    RUI_PSX_KBP_BAD_NAME,       // not a name a profile can have
    RUI_PSX_KBP_PROTECTED,      // "Default" cannot be replaced, renamed or deleted
    RUI_PSX_KBP_EXISTS,         // a profile of that name is there already
    RUI_PSX_KBP_NOT_FOUND,      // no profile of that name
    RUI_PSX_KBP_NOT_A_PROFILE,  // the file holds no keyboard map; keys unchanged
    RUI_PSX_KBP_IO              // the folder or the file could not be written
} RuiPsxKbProfileResult;

// The folder of the profiles. PSXRECOMP_KEYBOARD_PROFILES_DIR names it when
// set; else it is psxrecomp/keyboard-profiles in the user's settings folder
// (%APPDATA% on Windows, ~/Library/Application Support on macOS,
// $XDG_CONFIG_HOME or ~/.config elsewhere). Nothing is created here: the
// folder appears with the first profile saved. Returns 0, and an empty
// string, when the system names no such folder.
int rui_psx_kb_profiles_dir(char* out, int cap);

// OK, BAD_NAME, or PROTECTED for "Default".
int rui_psx_kb_profile_check_name(const char* name);

// The profile names: "Default" first, then the files of the folder in
// alphabetical order. Returns how many were written to `names` (at most
// `max`). A missing folder lists "Default" alone.
int rui_psx_kb_profiles_list(const char* dir,
                             char names[][RUI_PSX_KB_PROFILE_NAME_MAX + 1],
                             int max);

// Save the player's keys under `name`. An existing profile of that name is
// replaced only when `replace` is set; EXISTS otherwise.
int rui_psx_kb_profile_save_as(const char* dir, const char* name,
                               const char* keybinds_path, int player,
                               int replace);

// Put the profile's keys on the player and persist them to keybinds.ini.
// "Default" puts the default map there. Other players are not touched.
int rui_psx_kb_profile_load(const char* dir, const char* name,
                            const char* keybinds_path, int player);

// Give a profile another name. Its keys do not change.
int rui_psx_kb_profile_rename(const char* dir, const char* from, const char* to);

// Remove a profile. The keys a player has in use do not change.
int rui_psx_kb_profile_delete(const char* dir, const char* name);

// The profile whose keys are the player's keys now: "Default", else the first
// file that holds them. Returns 0, and an empty string, when no profile does.
int rui_psx_kb_profile_in_use(const char* dir, const char* keybinds_path,
                              int player, char* out, int cap);

#ifdef __cplusplus
}
#endif

#endif // RUI_CONSOLE_PSX_KB_PROFILES_H
