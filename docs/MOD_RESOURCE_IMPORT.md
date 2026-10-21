# Background owner-resource imports

An expensive resource conversion can use the launcher's existing background
setup job instead of blocking the file-picker callback. This is a source-paired
C ABI addition, advertised by `RECOMP_LAUNCHER_HAS_MOD_RESOURCE_IMPORT`.

Set `RecompLauncherCModResource.import_on_select` and provide
`RecompLauncherCModProvider.feature_resource_import`. The callback receives the
package/feature/resource IDs, selected resource path, and a snapshot of the
selected primary game image. It also receives a progress sink and error buffer.

The callback runs on the worker thread. Return 1 after adopting a complete,
validated resource installation; return 0 and an actionable error on failure.
Keep the previous resource selection intact until all replacement resources
have passed validation. Do not render UI or mutate the primary image. Progress
fractions are 0..1, or negative for indeterminate work.

The launcher shows the setup progress modal, blocks other provider mutations,
and keeps the model alive while the job runs. Imports also poll and display
progress for cartridge titles without a first-run setup wizard.
Successful import stages changes
for the ordinary PLAY commit; it does not mark game setup complete or replace
the ROM/disc selection. Normal resources keep `import_on_select=0`. Clear still
uses `feature_resource_set_path` with an empty path, so the provider controls
whether imported resources remain installed.

ROM-free regression: `recomp-ui-mod-import-test` exercises the real worker and
poll path, success/failure, progress, overlap rejection and image preservation.
For graphical QA, `LNG_SCRIPT` also supports `view:mods`, `mod-select:<index>`,
`resource-import:<resource-id>:<path>` and `wait-ready`. These select inputs and
invoke the same worker; they do not fabricate an import result.
