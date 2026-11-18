# PlayStation 2 host profile

The PS2 host profile preserves the SDL2 fallback and other console profiles.
The ps2llr consumer selects the shared SDL3 backend at build time.

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
consumers. It calls the host on the real owner thread before draw (phase0, when
no preparation worker owns the provider) and
after GL rendering/before presentation (phase1). The host may service queued
model actions and capture only on request; model/platform pointers cannot be
retained or used from another thread. The ps2llr thin DLL owns its bounded RGBA
readback/free API. Existing in-session/Resume and preserve-SDL APIs remain used.

Preserve all bundled component licenses/notices.
