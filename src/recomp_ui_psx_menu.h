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

#ifdef __cplusplus
extern "C" {
#endif

struct PsxMenuProvider;
const struct PsxMenuProvider *recomp_ui_psx_menu_provider(void);

/* For hosts and tests without SDL events: one RecompRuntimeUiInput press
 * (RECOMP_RUNTIME_UI_INPUT_TOGGLE opens/closes). Main thread. */
void recomp_ui_psx_menu_input(int recomp_runtime_ui_input);

#ifdef __cplusplus
}
#endif
#endif
