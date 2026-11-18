// psx_kb_profiles.c - see psx_kb_profiles.h.

#include "psx_kb_profiles.h"
#include "psx_binds.h"
#include "psx_profile.h"   // LNG_PSX_PAD_BUTTON_COUNT

#include <string.h>

/* Keep in sync with PSX_BINDS_MAX_PLAYERS (psx_binds.c). */
#define PSX_KBP_PLAYERS 5

/* Rebind-spec order (psx_profile.h kPsxPadButtons), the names psxrecomp's
 * keybinds.ini gives the keys. */
static const char* const kPsxKbpIds[LNG_PSX_PAD_BUTTON_COUNT] = {
    "up", "down", "left", "right",
    "triangle", "circle", "cross", "square",
    "l1", "l2", "r1", "r2", "l3", "r3",
    "start", "select",
    "ls_up", "ls_down", "ls_left", "ls_right",
    "rs_up", "rs_down", "rs_left", "rs_right",
};

static int psx_get(const RuiKbAdapter* a, const char* path, int p, int b, int slot) {
    (void)a;
    return rui_psx_binds_get_slot(path, p, b, slot);
}
static int psx_default(const RuiKbAdapter* a, int p, int b, int slot) {
    (void)a;
    return rui_psx_binds_default_key(p, b, slot);
}
static void psx_apply(const RuiKbAdapter* a, const char* path, int p, const int* keys) {
    (void)a;
    for (int b = 0; b < LNG_PSX_PAD_BUTTON_COUNT; ++b) {
        rui_psx_binds_set_slot(path, p, b, 1, 0);   /* free the alternate first: no key is taken twice */
        rui_psx_binds_set_slot(path, p, b, 0, keys[b * 2]);
        rui_psx_binds_set_slot(path, p, b, 1, keys[b * 2 + 1]);
    }
}
static void psx_reset(const RuiKbAdapter* a, const char* path, int p) {
    (void)a;
    rui_psx_binds_reset(path, p);
}
static int psx_save_file(const RuiKbAdapter* a, const char* path, int p, const char* dst) {
    (void)a;
    return rui_psx_binds_save_profile(path, p, dst);
}
static int psx_load_file(const RuiKbAdapter* a, const char* path, int p, const char* src) {
    (void)a;
    return rui_psx_binds_load_profile(path, p, src);
}
static int psx_matches_file(const RuiKbAdapter* a, const char* path, int p, const char* src) {
    (void)a;
    return rui_psx_binds_matches_profile(path, p, src);
}
static int psx_is_default(const RuiKbAdapter* a, const char* path, int p) {
    (void)a;
    return rui_psx_binds_is_default(path, p);
}

#define PSX_ADAPTER(ID, ALIAS, LEGACY) { \
    ID, ALIAS, LEGACY, NULL, PSX_KBP_PLAYERS, LNG_PSX_PAD_BUTTON_COUNT, 2, kPsxKbpIds, \
    psx_get, psx_default, psx_apply, psx_reset, \
    psx_save_file, psx_load_file, psx_matches_file, psx_is_default }

static const RuiKbAdapter kPsxAdapter =
    PSX_ADAPTER("psx", "PSXRECOMP_KEYBOARD_PROFILES_DIR", "psxrecomp/keyboard-profiles");
static const RuiKbAdapter kPs2Adapter = PSX_ADAPTER("ps2", NULL, NULL);

const RuiKbAdapter* rui_psx_kb_adapter(const char* console_id) {
    if (console_id && !strcmp(console_id, "psx")) return &kPsxAdapter;
    if (console_id && !strcmp(console_id, "ps2")) return &kPs2Adapter;
    return NULL;
}
