# SBI input in the launcher

PSX hosts can supply `RecompLauncherCGameInfo.import_sbi` to attach a selected
SBI file to the current disc. The callback validates and stages the companion,
then returns a mounted disc path. It must never return the SBI itself as a disc.
Return zero with an error message on failure; the model preserves the selection.

When this callback exists, the disc button reads **Browse For Disc / SBI**.
The PSX filter accepts `.sbi`, and the model recognises its suffix without regard
to case. Select a disc first. Import errors appear below the browse button.

The host sets `RecompLauncherCDiscVerify.sbi_status` to `RECOMP_SBI_MISSING`,
`RECOMP_SBI_NA`, or `RECOMP_SBI_OK`. The card displays Missing, N/A, or OK under
ISO header. The host owns the meaning of applicability and validation.
Build every launcher consumer with the updated headers because the C structures
have grown. Other console filters remain unchanged.

`tests/launcher_discs_test.c` covers status refresh, uppercase SBI selection,
selection preservation on failure, and selection of the imported CUE on success.
