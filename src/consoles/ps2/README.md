# PlayStation 2 host profile

This private integration branch is based on shared recomp-ui commit
`c90796f2d0fb198b2972a2d97f81ffa58b33d7df`. It preserves the SDL2 fallback and
all other console profiles while selecting SDL3 for the ps2llr consumer.

The PS2 profile reuses the shared DualShock vocabulary and named keyboard bind
store, exposes two physical-instance player sources, and hides unsupported pad
remapping. Native PS2 cards are host-owned: SAVE_NONE intentionally prevents the
PS1 card formatter/inspector from appearing. Presentation enhancements are
host-backed Mods; no legacy supersampling cap or HLE/LLE toggle is exposed.

The navy/fog/tower background is original procedural UI geometry. It uses no
BIOS screenshots/assets and does not copy the PSP wave styling. The PSP host
provider pattern was reviewed at PSPLLR commit
`f45bf1dee3a8cac012ad9063002a93ebe58c66b6`,
`tools/pspref/psp_launcher/launcher.cpp` (MIT).

`recomp_launcher_set_host_frame_hook` is additive and dormant for existing
consumers. It calls the host on the real owner thread before draw (phase0) and
after GL rendering/before presentation (phase1). The host may service queued
model actions and capture only on request; model/platform pointers cannot be
retained or used from another thread. The ps2llr thin DLL owns its bounded RGBA
readback/free API. Existing in-session/Resume and preserve-SDL APIs remain used.

Syntax-only C/C++ checks pass. No DLL link, launched UI, TCP screenshot/action,
physical-controller, saved-choice, density1..8 or gameplay qualification has been
performed for this integration. Preserve all bundled component licenses/notices.
