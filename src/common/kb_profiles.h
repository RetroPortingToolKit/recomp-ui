// common/kb_profiles.h - named keyboard profiles, for any console whose
// keyboard binds a RuiKbAdapter can read and write.
//
// A profile is one player's keyboard map under a name: a plain text file,
// <name>.ini, with one [player1] section (`<button> = <key>[, <alternate>]`,
// SDL key names), in a folder of the user that every game of the console
// reads. A key set made in one game is there in the next, and a file copied
// into the folder is a profile.
//
// "Default" is not a file. It is the keyboard map the console starts with,
// as its store's Reset puts it (a host's own defaults when it supplied
// them). It is always listed first and cannot be replaced, renamed or
// deleted, whatever its letter case; a Default.ini in the folder is not read.
//
// Keyboard only: a pad keeps its settings in its own store.
//
// A name is 1 to 32 characters: ASCII letters, digits, space and - _ . ( ) +.
// It does not start or end with a space or a dot and is not a device name of
// Windows (CON, NUL, COM1 ...), so that the same name is a file name on
// Windows, Linux and macOS. Two names that differ only in letter case are
// the same profile.
//
// Folder, in the order tried:
//   1. the adapter's `env_alias` (PSX: PSXRECOMP_KEYBOARD_PROFILES_DIR),
//      naming the folder itself;
//   2. RECOMP_UI_KEYBOARD_PROFILES_DIR, naming a root; the folder is
//      <root>/<console id>;
//   3. <settings folder>/recomp-ui/keyboard-profiles/<console id>, or the
//      adapter's `legacy_subdir` under the settings folder when it has one
//      (PSX: psxrecomp/keyboard-profiles, where its first release saved).
// The settings folder is %APPDATA% on Windows, ~/Library/Application Support
// on macOS, $XDG_CONFIG_HOME or ~/.config elsewhere.
//
// Players are 0-based. `keys_path` is the resolved bind file of the running
// title, handed back to the adapter untouched.

#ifndef RUI_COMMON_KB_PROFILES_H
#define RUI_COMMON_KB_PROFILES_H

#ifdef __cplusplus
extern "C" {
#endif

#define RUI_KBP_NAME_MAX    32
#define RUI_KBP_DEFAULT     "Default"
#define RUI_KBP_MAX_BUTTONS 32
#define RUI_KBP_MAX_SLOTS   2
#define RUI_KBP_ENV_ROOT    "RECOMP_UI_KEYBOARD_PROFILES_DIR"

typedef enum RuiKbpResult {
    RUI_KBP_OK = 0,
    RUI_KBP_BAD_NAME,       // not a name a profile can have
    RUI_KBP_PROTECTED,      // "Default" cannot be replaced, renamed or deleted
    RUI_KBP_EXISTS,         // a profile of that name is there already
    RUI_KBP_NOT_FOUND,      // no profile of that name
    RUI_KBP_NOT_A_PROFILE,  // the file holds no keyboard map; keys unchanged
    RUI_KBP_IO              // the folder or the file could not be written
} RuiKbpResult;

// What a console's keyboard store has to offer. Keys are SDL scancodes as int,
// 0 (SDL_SCANCODE_UNKNOWN) for unbound. A slot is a primary (0) or alternate
// (1) key of one button; `slots` is how many the store keeps.
typedef struct RuiKbAdapter RuiKbAdapter;
struct RuiKbAdapter {
    const char* id;                  // console id; the folder name
    const char* env_alias;           // env var naming the folder itself, or NULL
    const char* legacy_subdir;       // folder under the settings folder, or NULL
    const void* ctx;                 // the adapter's own; the store ignores it
    int players, buttons, slots;     // buttons <= RUI_KBP_MAX_BUTTONS, slots 1..2
    const char* const* button_ids;   // file keys, lowercase [a-z0-9_]; one per button

    int  (*get)(const RuiKbAdapter* a, const char* keys_path, int player, int b, int slot);
    // What Reset puts there. Needs no file.
    int  (*default_key)(const RuiKbAdapter* a, int player, int b, int slot);
    // Put the player's whole map (keys[b * slots + slot]) in the store and persist it.
    void (*apply)(const RuiKbAdapter* a, const char* keys_path, int player, const int* keys);

    // Optional: Reset as the store does it. Missing: apply() of default_key().
    void (*reset)(const RuiKbAdapter* a, const char* keys_path, int player);

    // Optional. A store whose own file format is richer than the generic one
    // (PSX: mouse buttons, the psxrecomp keybinds.ini) supplies all four; a
    // missing one is done generically from get/default_key/apply.
    int  (*save_file)(const RuiKbAdapter* a, const char* keys_path, int player, const char* dst);
    int  (*load_file)(const RuiKbAdapter* a, const char* keys_path, int player, const char* src);
    int  (*matches_file)(const RuiKbAdapter* a, const char* keys_path, int player, const char* src);
    int  (*is_default)(const RuiKbAdapter* a, const char* keys_path, int player);
};

// The folder of the profiles. Nothing is created here: it appears with the
// first profile saved. Returns 0, and an empty string, when the system names
// no such folder.
int rui_kbp_dir(const RuiKbAdapter* a, char* out, int cap);

// OK, BAD_NAME, or PROTECTED for "Default".
int rui_kbp_check_name(const char* name);

// "Default" first, then the files of the folder in alphabetical order.
// Returns how many were written to `names` (at most `max`).
int rui_kbp_list(const char* dir, char names[][RUI_KBP_NAME_MAX + 1], int max);

// Save the player's keys under `name`; written to a temporary file and moved
// into place, so a failed write leaves an existing profile as it was. An
// existing profile is replaced only when `replace` is set; EXISTS otherwise.
int rui_kbp_save_as(const RuiKbAdapter* a, const char* dir, const char* name,
                    const char* keys_path, int player, int replace);

// Put the profile's keys on the player and persist them. "Default" puts the
// default map there. Other players are not touched.
int rui_kbp_load(const RuiKbAdapter* a, const char* dir, const char* name,
                 const char* keys_path, int player);

// Put the keys of any keyboard file (a profile, or another game's bind file
// of the same format) on the player: its [player<N>] section, else [player1].
// 1 on success; 0 and the keys unchanged when it holds no keyboard map.
int rui_kbp_load_file(const RuiKbAdapter* a, const char* keys_path, int player,
                      const char* src);

int rui_kbp_rename(const char* dir, const char* from, const char* to);
int rui_kbp_delete(const char* dir, const char* name);

// The profile whose keys are the player's keys now: "Default", else the first
// file that holds them. Returns 0, and an empty string, when no profile does.
// The answer is kept while the player's keys, the folder and this process's
// saves, loads, renames and deletes are unchanged, so asking every frame
// costs a hash of the keys. A file put in the folder by another process is
// seen after rui_kbp_invalidate().
int rui_kbp_in_use(const RuiKbAdapter* a, const char* dir, const char* keys_path,
                   int player, char* out, int cap);

// Forget what rui_kbp_in_use kept.
void rui_kbp_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif // RUI_COMMON_KB_PROFILES_H
