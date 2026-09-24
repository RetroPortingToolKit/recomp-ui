# Optional experience presets and bundled music choices

Both capabilities are opt-in. Zero-initialized hosts retain their existing
UI. recomp-ui supplies no preset names, recipes, game IDs or default music.

## Presets

Provide all four callbacks at the end of `RecompLauncherCModProvider`:

```c
provider.preset_count = my_preset_count;
provider.preset_get = my_preset_get;
provider.preset_current = my_preset_current;
provider.preset_apply = my_preset_apply;
```

`preset_count` returns 1–64 definitions. `preset_get` fills a zero-initialized
`RecompLauncherCModPreset` with a stable ID, display name and description.
`preset_current` compares host-owned state and the supplied launcher settings,
returning a matching ID or an empty/NULL value for **Custom**. Its returned
string must remain valid for that frame. No active preset is stored separately
or reapplied automatically, so later individual edits remain authoritative.

`preset_apply` receives the selected ID and mutable launcher settings. The
host changes only the options owned by its recipe and necessary conflicts.
Preserve unrelated user choices. Validate availability before making changes;
failure must leave state untouched. Return 1 on success, 0 on failure, with
an explanation through `last_error` where available. The normal provider
commit and host launcher-settings persistence paths still apply.

The generic dropdown appears above the mod feature/package lists. If any
callback is omitted, or count is zero, the picker is absent. Choosing an
entry applies it once. Hosts do not have to implement this capability simply
because they implement mods.

## Bundled MSU sources

Hosts that already enable `msu1_supported` may additionally supply a stable
array of `RecompLauncherCMsuPack` records (`id`, `name`) through GameInfo's
`msu1_packs` / `num_msu1_packs`. The UI lists those names followed by
**Custom...**, which opens a `.msu` file picker. `msu1_pack` stores a bundled
ID; empty means custom. `msu1_dir` retains the custom path. Choosing a source
does not implicitly turn audio on or clear the previous custom selection.

The host owns default selection, ID-to-path resolution, compatibility,
persistence and migration. Unknown persisted IDs should be normalized by the
host. Leave the list absent to keep the existing custom-folder-only UI.
These appended fields require hosts and the UI to be rebuilt together, like
other changes to these shared C structs.

`launcher-presets` CTest covers the absent/incomplete capability, explicit
application, unrelated settings, source selection and custom-path retention.
