/* n64_pad_binds_test.c — the N64 controller page's layout contract and the
 * per-GUID store behind it.
 *
 * Two things are pinned here because both are silent when wrong:
 *
 *  1. The Gamepad Bindings order. launcher_imgui.cpp only reads a declared
 *     order COLUMN-MAJOR when cols*rows equals the number of buttons shown; a
 *     mismatch falls back to the width-derived row-major grid with no error,
 *     so the page would quietly look like it did before. The order itself is
 *     the reading order the page was specified with — down each column.
 *
 *  2. The store is keyed by SDL joystick GUID. That is the whole reason it
 *     exists next to input.cfg, and "two pads, two layouts" is exactly what a
 *     shared table cannot do.
 *
 * argv[1] is a scratch directory. */

#include "consoles/n64/n64_profile.h"
#include "consoles/n64/n64_pad_binds.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char* kExpectedOrder[] = {
    /* col 1 */ "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right", "A", "B",
    /* col 2 */ "Z", "Start", "Stick Up", "Stick Down", "Stick Left", "Stick Right",
    /* col 3 */ "C-Up", "C-Down", "C-Left", "C-Right", "L", "R",
};

static void check_layout(void) {
    const ControllerSpec* c = &kSystemProfileN64.controller;

    /* The page draws one label and one bind chip per cell. Two chips per row
     * is the alternate-slot layout this console deliberately left. */
    assert(c->binds_per_input == 1);
    assert(c->has_pad_binds == 1);

    /* The gate launcher_imgui.cpp applies before honouring the order. */
    assert(c->pad_bind_order == kN64GamepadBindOrder);
    assert(c->pad_bind_cols == 3);
    assert(c->pad_bind_rows == 6);
    assert(c->pad_bind_cols * c->pad_bind_rows == c->button_count);
    assert(c->button_count == LNG_N64_PAD_BUTTON_COUNT);

    /* Reading order: down column 1, then 2, then 3. */
    for (int i = 0; i < LNG_N64_PAD_BUTTON_COUNT; ++i) {
        const int b = kN64GamepadBindOrder[i];
        assert(b >= 0 && b < LNG_N64_PAD_BUTTON_COUNT);
        assert(!strcmp(c->buttons[b].label, kExpectedOrder[i]));
    }

    /* Every input appears exactly once — a duplicate would hide one control
     * and show another twice, and both cells would look plausible. */
    int seen[LNG_N64_PAD_BUTTON_COUNT] = {0};
    for (int i = 0; i < LNG_N64_PAD_BUTTON_COUNT; ++i)
        seen[kN64GamepadBindOrder[i]]++;
    for (int i = 0; i < LNG_N64_PAD_BUTTON_COUNT; ++i)
        assert(seen[i] == 1);
}

/* kN64PadButtons indices used below. */
enum { B_A = 0, B_Z = 2, B_DUP = 4, B_CUP = 10, B_STICK_UP = 14 };

static void check_store(const char* path) {
    const char* guid_a = "03000000d62000000228000000010000";
    const char* guid_b = "0300000079000000181100000001000";
    char v[64];

    remove(path);
    rui_n64_pad_binds_init(path);

    /* An unconfigured pad reads the PSR controller defaults, transcribed. */
    rui_n64_pad_binds_source(path, guid_a, B_A, v, (int)sizeof v);
    assert(!strcmp(v, "a"));
    rui_n64_pad_binds_source(path, guid_a, B_CUP, v, (int)sizeof v);
    assert(!strcmp(v, "righty-"));
    rui_n64_pad_binds_source(path, guid_a, B_STICK_UP, v, (int)sizeof v);
    assert(!strcmp(v, "lefty-"));
    rui_n64_pad_binds_source(path, guid_a, B_Z, v, (int)sizeof v);
    assert(!strcmp(v, "lefttrigger+"));

    /* Rebinding one pad leaves the other alone — the point of the store. */
    rui_n64_pad_binds_set(path, guid_a, B_A, 1 /*button*/, 1 /*SDL b*/, 0);
    rui_n64_pad_binds_source(path, guid_a, B_A, v, (int)sizeof v);
    assert(!strcmp(v, "b"));
    rui_n64_pad_binds_source(path, guid_b, B_A, v, (int)sizeof v);
    assert(!strcmp(v, "a"));

    /* An axis bind carries its direction: half an axis is the bind. */
    rui_n64_pad_binds_set(path, guid_a, B_CUP, 2 /*axis*/, 3 /*RIGHTY*/, +1);
    rui_n64_pad_binds_source(path, guid_a, B_CUP, v, (int)sizeof v);
    assert(!strcmp(v, "righty+"));

    /* It survives a reload: this file is what the game process reads. */
    rui_n64_pad_binds_init(path);
    rui_n64_pad_binds_source(path, guid_a, B_A, v, (int)sizeof v);
    assert(!strcmp(v, "b"));
    rui_n64_pad_binds_source(path, guid_b, B_A, v, (int)sizeof v);
    assert(!strcmp(v, "a"));

    /* Reset returns one pad to defaults without touching the other's edits. */
    rui_n64_pad_binds_set(path, guid_b, B_DUP, 1, 11 /*SDL dpup*/, 0);
    rui_n64_pad_binds_reset(path, guid_a);
    rui_n64_pad_binds_source(path, guid_a, B_A, v, (int)sizeof v);
    assert(!strcmp(v, "a"));
    rui_n64_pad_binds_source(path, guid_b, B_DUP, v, (int)sizeof v);
    assert(!strcmp(v, "dpup"));
}

/* input.ini is resolved beside the bind file the launcher already owns; the
 * game process resolves it the same way, through this same call. */
static void check_path(void) {
    char out[256];
    rui_n64_pad_binds_path("/opt/glover/input.cfg", out, (int)sizeof out);
    assert(!strcmp(out, "/opt/glover/input.ini"));
    rui_n64_pad_binds_path("input.cfg", out, (int)sizeof out);
    assert(!strcmp(out, "input.ini"));
}

int main(int argc, char** argv) {
    char path[512];
    snprintf(path, sizeof path, "%s/n64-pad-binds-test.ini",
             argc > 1 ? argv[1] : ".");

    check_layout();
    check_path();
    check_store(path);
    return 0;
}
