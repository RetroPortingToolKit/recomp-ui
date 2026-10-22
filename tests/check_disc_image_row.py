#!/usr/bin/env python3
"""The disc panel's "Disc image" row is drawn only for a file setup refused.

After setup refuses a disc image (another pressing with the kit's serial, or
the right disc in a form setup cannot read), the panel's headline is "Disc
verification failed" while its three rows (Serial, Region, ISO header) keep
their ticks: each of them is true of that file. A fourth row carries the
cross: "Disc image  Not the image this kit was made from" (Alex, 2026-10-02).

When the row shows is decided in the model and tested there
(launcher_model_disc_refused_by_setup, tests/launcher_setup_refusal_verdict_test.c).
This test keeps the view to that decision and to the accepted words:

  - the row is drawn inside the one condition, and nowhere else;
  - label and value are the accepted words;
  - its mark is a cross in the refused disc's red, not a tick and not amber.

Usage: check_disc_image_row.py <recomp-ui root>
"""
import re
import sys
from pathlib import Path

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
view = (root / "src" / "common" / "backends" / "imgui" / "launcher_imgui.cpp").read_text(encoding="utf-8")

LABEL = 'ui_text("Disc image")'
VALUE = 'ui_text("Not the image this kit was made from")'
failures = []


def check(cond, what):
    if not cond:
        failures.append(what)


check(view.count(LABEL) == 1, "the label must be drawn in exactly one place")
check(view.count(VALUE) == 1, "the value must be drawn in exactly one place")

block = view[view.index("void draw_verdict_block("):]
block = block[: block.index("\n}\n")]
gate = "if (launcher_model_disc_refused_by_setup(m)) {"
check(block.count(gate) == 1, "the row must be gated by the model's decision, once")
if block.count(gate) == 1 and LABEL in block and VALUE in block:
    at = block.index(gate)
    row = block[at:]
    row = row[: row.index("\n        }\n") + 1]
    check(LABEL in row and VALUE in row, "label and value must be inside the gate")
    check(row.index(LABEL) < row.index(VALUE), "the label comes before the value")
    check(re.search(r"state_mark\(false, th, &refused_red\);", row) is not None,
          "the row's mark must be a cross in the refused disc's red")
    check("state_mark(true" not in row, "the row never shows a tick")
    check("PushTextWrapPos" in row[: row.index(VALUE)], "the value must wrap: the dashboard's card is narrow")
    # After the three rows that are true of the file, before the rows that are
    # about something else (the SBI file, online play).
    check(block.index('kv_row("ISO header"') < at < block.index('kv_row("SBI File"'),
          "the row stands after ISO header and before SBI File")
else:
    check(False, "label and value must be in draw_verdict_block")

check("launcher_model_disc_refused_by_setup" in
      (root / "src" / "common" / "launcher_model.h").read_text(encoding="utf-8"),
      "the model must declare launcher_model_disc_refused_by_setup")

if failures:
    for f in failures:
        print("FAIL:", f, file=sys.stderr)
    sys.exit(1)
print("disc image row: ok")
