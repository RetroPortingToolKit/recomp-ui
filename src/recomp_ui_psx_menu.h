/* recomp_ui_psx_menu.h -- recomp-ui as a psxrecomp in-game menu provider.
 *
 * psxrecomp owns the provider contract (runtime/include/psx_menu_host.h):
 * the runtime keeps every policy (pause, live settings, mod changes,
 * disc/tray, netplay session start and exit) and offers it to any provider
 * through PsxMenuHost. recomp-ui implements PsxMenuProvider with its shared
 * runtime-UI model (recomp_runtime_ui.h) and ImGui presentation:
 *
 *   - Display: fullscreen; Graphics: render thread (restart-only: applied
 *     by a quick relaunch when the menu closes); System: pause in menu,
 *     resume, return to launcher, quit. (Texture, FMV and linear filtering
 *     are psxrecomp's Image Filtering mod now, not menu rows.)
 *     Disc: selected disc and Insert disc and resume (multi-disc titles,
 *     through the runtime's tray policy).
 *   - Opens with F10 or Ctrl+L, or Select + L3 on a controller; arrows,
 *     Enter/Space, Esc/Backspace, D-pad, Cross/Circle navigate.
 *
 * psxrecomp uses this provider when the title registers none (recomp-ui is
 * the default). Chosen that way (PsxMenuHost.default_selection) it keeps the
 * title's default presentation: it attaches only for multi-disc titles,
 * which is where recomp-ui's game menu appears today.
 *
 * Built only inside a psxrecomp title build (the runtime's include path has
 * psx_menu_host.h); declared here so the runtime can find it with
 * __has_include. */
#ifndef RECOMP_UI_PSX_MENU_H
#define RECOMP_UI_PSX_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PsxMenuProvider;
const struct PsxMenuProvider *recomp_ui_psx_menu_provider(void);

/* For hosts and tests without SDL events: one RecompRuntimeUiInput press
 * (RECOMP_RUNTIME_UI_INPUT_TOGGLE opens/closes). Main thread. */
void recomp_ui_psx_menu_input(int recomp_runtime_ui_input);

/* Controls page: a full-screen, game-styled button config (Keyboard /
 * Controller chooser, then one row per action in the title's order). Each
 * action names the DualShock input the game reads for it; the page rebinds
 * player 1's keyboard key (keybinds.ini) and the first connected gamepad's
 * button (input.ini [mapping.<guid>]) for that input, swapping on a clash.
 * Start's key leaves a page; leaving the chooser closes the page. */
typedef struct RecompUiPsxControlAction {
    const char *label;   /* row label; '\n' splits it over two lines */
    const char *button;  /* keybinds.ini name: "square", "cross", "r2", ... */
} RecompUiPsxControlAction;

/* Optional: the title's own art for the page, in its native pixels (the page
 * is 320x240). An RGBA atlas plus where each piece sits in it. Without a skin
 * the page draws Lato in the PS-menu style. */
typedef struct RecompUiPsxRect { uint16_t x, y, w, h; } RecompUiPsxRect;

typedef struct RecompUiPsxFont {
    RecompUiPsxRect glyph[96];  /* ASCII 32..127; w == 0: not in the font */
    uint8_t advance;            /* pen step per character; 0: glyph width + 1 (space 4) */
    uint8_t line_height;        /* visible height of a capital, from the cell top */
} RecompUiPsxFont;

typedef struct RecompUiPsxBlit { RecompUiPsxRect src; int16_t x, y; } RecompUiPsxBlit;

typedef struct RecompUiPsxControlsSkin {
    const uint32_t *rgba;          /* atlas, R G B A bytes in memory; A 0 = clear */
    int width, height;
    RecompUiPsxFont title, item, item_selected, small;
    RecompUiPsxRect face[4];       /* cross, circle, triangle, square */
    RecompUiPsxRect shoulder[4];   /* L1, L2, R1, R2 */
    RecompUiPsxRect ellipsis;      /* the "..." before a prompt verb */
    RecompUiPsxRect cursor[4];     /* selector animation frames */
    int8_t cursor_dx[4];           /* per-frame x nudge */
    int cursor_frames, cursor_vblanks;
    RecompUiPsxBlit backdrop[32];
    int backdrop_count;
} RecompUiPsxControlsSkin;

/* Copied (atlas included); NULL clears it. Any thread. */
void recomp_ui_psx_controls_set_skin(const RecompUiPsxControlsSkin *skin);

/* Interaction cues for the title's own menu sounds, oldest first. */
enum {
    RECOMP_UI_PSX_CUE_NONE = 0,
    RECOMP_UI_PSX_CUE_MOVE,      /* highlight moved */
    RECOMP_UI_PSX_CUE_ACCEPT,    /* page entered */
    RECOMP_UI_PSX_CUE_BACK,      /* page or screen left */
    RECOMP_UI_PSX_CUE_BOUND,     /* a key or button was assigned */
    RECOMP_UI_PSX_CUE_WARN       /* hotkey warning */
};
int  recomp_ui_psx_controls_take_cue(void);

/* Copied; call before opening. At most 12 actions. */
void recomp_ui_psx_controls_set_actions(const RecompUiPsxControlAction *actions, int count);
/* Open the page. The game keeps running underneath (its own screen code
 * owns what happens meanwhile); input goes to the page. 0 when the menu is
 * not attached or no actions were set. Any thread. */
int  recomp_ui_psx_controls_open(void);
int  recomp_ui_psx_controls_is_open(void);

#ifdef __cplusplus
}
#endif
#endif
