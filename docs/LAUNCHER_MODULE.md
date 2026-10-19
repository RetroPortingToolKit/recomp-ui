# Launcher module pilot (PS1B-482)

This opt-in pilot puts the blocking PSX pre-boot and return/rematch dashboard
in one platform shared library. The title keeps its in-game overlay, callbacks,
network session and shared non-launcher helpers. The initial conversion needs
a title rebuild. A later compatible UI change replaces the module through the
existing kit update route, after the title exits. There is no self-updater.

The public contract is `src/recomp_launcher_module.h`. The only module export
is `recomp_launcher_query_v1`. Its sized records negotiate major 1/minor 0,
architecture, byte order, C calling convention, full record/callback layout
identifiers and capabilities. Settings keep the frozen existing bytes. Mods,
netplay and start-list providers have their own size/version wrappers. Their
contexts and all borrowed strings/arrays stay valid until the blocking call
ends. Workers join before return. Each side frees only its own allocations.

Query and Run check the actual shared SDL3 path, file hash, version and symbol
identity on both sides. SDL2, static SDL and two SDL instances refuse the
module route. No SDL handle crosses the interface. Query/load mismatches stop
boot with a diagnostic; they never become legacy UNAVAILABLE=2. Explicit
headless and skip-launcher starts still bypass the loader.

The host passes absolute product, save and asset roots. Its scanner owns the
existing PSX state/replay formats and supplies typed records and thumbnail
copies. The module returns power-on, save-slot or replay choices in caller
buffers. It does not set PSX_LOAD_SLOT or PSX_REPLAY_FILE. The host retains
existing explicit command-line/environment inputs and their precedence.

`recomp_add_launcher_module` builds the module once against the exact shared
SDL3 target, with a 64-character SHA-256 source/build receipt identity.
`recomp_target_launcher_module_host` adds the loader, callback/scanner adapter
and in-game UI helpers to a title and stages an already-built module plus
assets and the same shared SDL3 provider. It does not compile the launcher UI.
Both helpers live in `recomp_ui.cmake`. The legacy integration path remains
the default. The standalone option is RECOMP_UI_BUILD_LAUNCHER_MODULE=ON.
Build that module with RECOMP_UI_ENABLE_MODS=ON if the title supplies a mod
provider. A module that lacks this capability refuses that title's request.

Framework consumers opt in with PSX_LAUNCHER_MODULE=ON,
PSX_STATIC_RUNTIME=OFF and RECOMP_LAUNCHER_MODULE_FILE pointing to the actual
prebuilt platform module. Configuration refuses an absent artifact or an
unverified/static SDL target. Runtime receipts must separately bind framework,
launcher/module, SDL and assets, architecture and toolchain. Windows stages a
DLL; Linux and macOS stage a shared library with relative dependency paths.
The Windows artifact is `recomp-launcher-v1.dll`, including under MinGW.
Linux uses `librecomp-launcher-v1.so`; macOS uses
`librecomp-launcher-v1.dylib`. Staging refuses a different basename.
The Windows SDL identity collector declares DLL imports explicitly so its
addresses come from its actual imports rather than caller-local jump stubs.
Platform dependency floors, signing, setup/multi-program packaging and safe
replacement/rollback still require actual artifact and player-path tests.

The accepted 590-member inventory and witness generator produce compile
expressions for every legacy structure/member and the v1 boundary. FNV values
are layout identifiers; SHA-256 binds artifacts. The authored C/C++ contract
fixture tests layout agreement and refusal preservation. It cannot prove real
SDL/device identity, callback families, setup/netplay, settings parity or
one-file update behavior. All eight named acceptance measurements remain open
until their actual evidence passes on the required platforms, including a
native macOS arm64 host. Source or unit success is not fleet approval.

The inventory ships at `tools/launcher_v1_contract.tsv`. Regenerate both witness
files from the repository root with the frozen public header:

```powershell
& python tools/generate_launcher_v1_witness.py src/recomp_launcher.h tools/launcher_v1_contract.tsv src/common/launcher_module_legacy_witness.inc --boundary-header src/recomp_launcher_module.h --boundary-output src/common/launcher_module_boundary_witness.inc
```

The generator checks that header's SHA-256 before writing, after normalizing
the LF or CRLF line-ending encoding of the accepted Git blob.
The module joins prepare workers while their stack model is alive, before
settings readback and platform teardown, and joins cover workers before Run
returns. These lifetime changes still need actual native callback/unload tests.
If a worker wait fails, the module prints a diagnostic and stops the host with
failure status before cleanup can release a context whose worker may survive.
That failure path bypasses normal destructors; it can lose unsaved edits and
still needs native fault tests. It never returns a successful launcher result.
