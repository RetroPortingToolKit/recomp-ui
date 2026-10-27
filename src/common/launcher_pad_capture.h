#ifndef LAUNCHER_PAD_CAPTURE_H
#define LAUNCHER_PAD_CAPTURE_H

#include "launcher_model.h"
#include "launcher_input.h"
#include <string.h>

/* Resolve the selected physical controller before accepting a bind or polling
 * its release state. SDL2 instance 0 is valid; the persisted instance stores
 * id+1 so that a disconnected/GUID-only selection remains distinguishable.
 * Return 1 for a live selection, -1 for an unavailable concrete selection,
 * and 0 for a generic source that has no selected controller. */
static inline int launcher_pad_capture_resolve(const LauncherModel* m,
        const LauncherPad* pads, int count, uint32_t* id) {
    if (!m || !id || m->cfg_player < 0 || m->cfg_player >= LNG_MAX_PLAYERS)
        return -1;
    const int player = m->cfg_player;
    const char* guid = m->s.player_gamepad_guid[player];
    const uint32_t instance = m->s.player_gamepad_instance[player];
    const uint32_t raw = m->player_pad_id[player];
    if (m->s.player_src[player] != 2) return -1;
    if (!instance && !raw && !guid[0]) return 0;
    if (!pads || count <= 0) return -1;

    /* Prefer the assigned unit when several controllers have the same GUID. */
    for (int i = 0; i < count; ++i) {
        if ((instance ? pads[i].id + 1 == instance : raw && pads[i].id == raw) &&
            (!guid[0] || !strcmp(pads[i].guid, guid))) {
            *id = pads[i].id;
            return 1;
        }
    }
    /* A restored profile/reconnected controller can have a new SDL ID. Do
     * not steal an identical controller already assigned to another seat. */
    if (guid[0]) for (int i = 0; i < count; ++i) {
        if (pads[i].guid[0] && !strcmp(pads[i].guid, guid) &&
            !launcher_model_gamepad_instance_claimed(m, player, pads[i].id)) {
            *id = pads[i].id;
            return 1;
        }
    }
    return -1;
}
#endif
