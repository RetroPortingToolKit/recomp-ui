# Modern launcher module — initial major2 conversion

This private candidate combines modern upstream UI with preserved PS1 mouse,
GunCon, neGcon and replay controls. A title moving from historical major1
needs one title rebuild. Historical products and the v1 witnesses under
[tools/abi-history/v1](../tools/abi-history/v1/) remain unchanged.

The C contract is [recomp_launcher_module.h](../src/recomp_launcher_module.h).
Its only exported symbol is `recomp_launcher_query_v2`. Sized records check
major2/minor0, architecture, byte order, calling convention, capabilities,
actual record layouts and the shared SDL3 library. A mismatch refuses before
callbacks or settings edits. The Windows basename is `recomp-launcher-v2.dll`;
Linux uses `librecomp-launcher-v2.so`, macOS `librecomp-launcher-v2.dylib`.
Only Windows has been exercised for this candidate.

The DLL owns the blocking dashboard and the existing PS1 in-game menu
provider, including its private model and ImGui renderer. The host owns guest
state, policy, callbacks, settings storage, disc control, network session and
start-list scanning. The provider uses the core's existing C `PsxMenuHost`,
`PsxMenuProvider` and `PsxHostVideoSettings` records. Their complete members
participate in the checked layout fingerprint. Build the module with
`RECOMP_UI_PSXRECOMP_ROOT` naming the matching core so these records and the
provider are present. Missing provider capability refuses a caller that needs it.

Host and DLL use one shared SDL3 provider: version, loaded path, SHA256 and
SDL function addresses must match. The host passes absolute product/save/asset
roots without changing process environment or working directory. Typed start
records copy filenames and thumbnails into caller-owned buffers. The module
returns power-on, save-slot or replay choices; the host retains explicit CLI
precedence. It never sets `PSX_LOAD_SLOT` or `PSX_REPLAY_FILE`.

`recomp_add_launcher_module` builds the DLL against an exact shared SDL target
and requires a64-character SHA256 source/build identity.
`recomp_target_launcher_module_host` adds only the loader, callback/scanner
adapter and non-launcher host helpers to a title. It stages an already-built
module, assets and SDL; it never compiles the dashboard. These helpers live in
[recomp_ui.cmake](../recomp_ui.cmake). Core consumers opt in with
`PSX_LAUNCHER_MODULE=ON`, `PSX_STATIC_RUNTIME=OFF` and
`RECOMP_LAUNCHER_MODULE_FILE` pointing to that prebuilt platform module.
Static SDL or an absent/wrongly named module refuses configuration.

After the initial conversion, a compatible UI update replaces only the DLL
while the title is closed. No self-updater is provided. Close/unload only after
blocking calls have returned, workers have joined and providers have detached.
Worker termination failure stops the host before borrowed contexts are freed;
that failure can lose unsaved edits. It is never a successful launcher result.

The frozen public header inventory binds641 declarations across28 records and
642 members. The module boundary currently has90 members, plus the typed PS1
menu witnesses. The compile-derived hashes use actual `sizeof`, alignment and
offsets. FNV values identify layouts; SHA256 binds artifacts and source inputs.
Regenerate the public and boundary witnesses from the repository root:

```powershell
& python tools/generate_launcher_v2_witness.py src/recomp_launcher.h tools/launcher_v2_contract.tsv src/common/launcher_module_v2_records_witness.inc --boundary-header src/recomp_launcher_module.h --boundary-output src/common/launcher_module_v2_boundary_witness.inc
```

The host-only `recomp_launcher_module_inspect` checks the real DLL and PS1 menu
table without opening a window or invoking game callbacks. The core exposes
it through `--launcher-module-check`. This can check a replaced DLL from the
actual title executable, but does not prove rendered menus, settings edits,
controller interaction or gameplay. One-file update evidence also requires
unchanged title/generated hashes and zero title compiler invocations.

Current focused evidence covers five UI/model/contract tests and25 real loader
checks, including wrong versions/layouts/SDL providers and the real PS1 menu
table. Consumer gameplay, actual UI lifecycle, setup/netplay and other platform
qualification are separate measurements; these checks do not imply fleet
acceptance or compatibility with historical products.
