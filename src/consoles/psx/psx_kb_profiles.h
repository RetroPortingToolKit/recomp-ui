// consoles/psx/psx_kb_profiles.h - the PlayStation adapter of the named
// keyboard profiles (common/kb_profiles.h).
//
// PSX keeps its own file format (psxrecomp's keybinds.ini, with mouse buttons
// as alternate keys), so a PSX profile file is that text and the adapter hands
// the store psx_binds.h's own profile calls. PSX also keeps the folder its
// first release saved to: psxrecomp/keyboard-profiles of the settings folder,
// or PSXRECOMP_KEYBOARD_PROFILES_DIR. The PS2 profile shares the same keys and
// store, with the folder <settings>/recomp-ui/keyboard-profiles/ps2.

#ifndef RUI_CONSOLE_PSX_KB_PROFILES_H
#define RUI_CONSOLE_PSX_KB_PROFILES_H

#include "common/kb_profiles.h"

#ifdef __cplusplus
extern "C" {
#endif

// The adapter of "psx" or "ps2" (any other id: NULL).
const RuiKbAdapter* rui_psx_kb_adapter(const char* console_id);

#ifdef __cplusplus
}
#endif

#endif // RUI_CONSOLE_PSX_KB_PROFILES_H
