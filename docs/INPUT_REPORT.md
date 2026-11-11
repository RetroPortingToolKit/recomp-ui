# Controller reports

The SDL2 and SDL3 launchers can capture a bounded controller report before
Play, including without a selected ROM. Set `RECOMP_INPUT_REPORT` to the
destination `.jsonl` file before opening the launcher. The default capture
lasts ten seconds; `RECOMP_INPUT_REPORT_MS` selects 100 to 30000 milliseconds.
Connect the controller first, then press and release buttons, move both sticks,
and use the D-pad while the launcher stays open. Closing the launcher completes
the report early. The launcher does not start a game for this capture.

Each report contains raw joystick enumeration (including devices without an
SDL gamepad mapping), names, GUIDs, vendor/product IDs, mapping text, axis/button/
hat counts and values, selected player devices and binding values, profile file
paths, and a short event stream. Event capture is capped at 4096 entries. The
report stays local; review it before sharing it.

`recomp-ui-input-report <output.jsonl> [duration-ms]` performs the same device
and event capture without a launcher, ROM, or window. Standalone reports mark
player assignments unavailable; use the launcher capture to inspect its actual
assignments and bindings. The reusable C interface is
`src/common/launcher_input_report.h`.
