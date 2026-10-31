#!/usr/bin/env python3
"""The launcher offers OpenBIOS only in a build that holds it (PS1B-420).

Every PlayStation kit's setup window called the BIOS optional and offered
"Use OpenBIOS" in five places. A build that holds no BIOS of its own (every
pin H kit) needs the player's image; the button cleared the BIOS they had
chosen and switched to nothing. Alex decided the words for such a build on
2026-10-02 (A16).

Which kind of build it is, is decided in the model and tested there
(launcher_model_bundled_bios_offered, tests/launcher_bios_step_test.c). This
test keeps the view to that decision and to the accepted words:

  - every "Use OpenBIOS" button is inside a condition on the model's answer;
  - the heading and the line of step 1 have both forms, chosen by it;
  - the picker's title names no file (it named SCPH1001.BIN for every kit);
  - Generate waits for a BIOS, with the accepted tooltip.

Usage: check_bios_step_words.py <recomp-ui root>
"""
import re
import sys
from pathlib import Path

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
view = (root / "src" / "common" / "backends" / "imgui" / "launcher_imgui.cpp").read_text(encoding="utf-8")
failures = []


def check(cond, what):
    if not cond:
        failures.append(what)


# The five buttons. Each must stand inside a block opened by a condition on
# the model's answer, with nothing but that block's own lines in between.
GATES = ("launcher_model_bundled_bios_offered(m)", "offers_bundled", "psx_bundled")
buttons = [m.start() for m in re.finditer(r'ImGui::Button\((?:ui_text\()?"Use OpenBIOS', view)]
check(len(buttons) == 5, f'"Use OpenBIOS" buttons: expected 5, found {len(buttons)}')
for at in buttons:
    before = view[max(0, at - 700):at]
    opened = [before.rfind("if (" + g) for g in GATES] + [before.rfind("|| (" + g) for g in GATES]
    # psx_bundled gates the Settings card through "if (psx_bundled || ...) {" and "if (is_psx) {".
    gate = max(opened)
    line = view.count("\n", 0, at) + 1
    check(gate >= 0, f'the "Use OpenBIOS" button at line {line} is not inside a condition on the build holding a BIOS')
    if gate >= 0:
        between = before[gate:]
        check(between.count("{") > between.count("}"),
              f'the "Use OpenBIOS" button at line {line} stands after its condition has closed')

# Step 1: both forms of the heading and of the line.
check(re.search(r'\(offers_bundled \? "1\. PlayStation BIOS \(optional\)"\s*: "1\. PlayStation BIOS"\)', view) is not None,
      "the heading must be chosen by offers_bundled")
check(re.search(r'offers_bundled\s*\? "Optional \\u2014 OpenBIOS is used unless you browse for a "\s*'
                r'"retail dump \(exactly 512 KB\)\."\s*'
                r': "This build needs a PlayStation BIOS image \(exactly 512 KB\)\. "\s*'
                r'"Select your own dump\."', view) is not None,
      "the line of step 1 must have both forms, the accepted words for a build with no BIOS of its own")
check(re.search(r"const bool offers_bundled = \(plat == SETUP_PLAT_PSX\) &&\s*launcher_model_bundled_bios_offered\(m\);",
                view) is not None, "offers_bundled must be the model's answer")
check('offers_bundled ? "OpenBIOS" : "(none selected)"' in view,
      "an empty row must read (none selected) where no OpenBIOS is held")

# The picker's title.
check("SCPH1001.BIN)" not in view, "the BIOS picker's title must not name SCPH1001.BIN: 37 kits need another image")
check('"Select PlayStation BIOS"' in view, 'the BIOS picker\'s title must be "Select PlayStation BIOS"')

# The "BIOS not in this build" box: the OpenBIOS clause only where it is held.
clause = "use Use OpenBIOS instead."
check(view.count(clause) == 1, "the box's OpenBIOS clause must stand once")
if view.count(clause) == 1:
    at = view.index(clause)
    check("else if (launcher_model_bundled_bios_offered(m))" in view[at - 500:at],
          "the box's OpenBIOS clause must be inside the condition")
    after = view[at:at + 500]
    check('"with your current disc and toolchain.");' in after, "the box must have a form without the clause")

# Generate waits for a BIOS.
check("const bool needs_bios = launcher_model_setup_bios_blocks_generate(m);" in view,
      "Generate must ask the model whether it has to wait for a BIOS")
check(re.search(r'strcmp\(m->rom_size, "--"\) != 0 && !needs_bios;', view) is not None,
      "a missing BIOS must grey Generate")
check(re.search(r'if \(needs_bios\)\s*ImGui::SetTooltip\("Select a PlayStation BIOS first"\);', view) is not None,
      "the greyed Generate must say that a BIOS is missing")

# Six more sentences offered OpenBIOS. Each keeps its full form for a build
# that takes an empty BIOS, and has a form without the clause (or the
# launcher's own label) for one that does not. Adjacent string literals are
# joined first, so that a sentence reads as the player reads it.
flat = re.sub(r'"\s*"', "", view)
PAIRS = [
    # (what it is, the full form, the form for a build with no BIOS of its own)
    ("the Settings row with nothing chosen",
     '? "OpenBIOS" : "(none selected)"));', None),
    ("the setup note when the chosen BIOS still has to be built",
     "(you need a disc image for that), or switch back to OpenBIOS to play now.\"",
     "(you need a disc image for that).\""),
    ("the setup note on a build that is already generated",
     "Select a PlayStation BIOS (or keep OpenBIOS) and a Redump-style .cue with sibling .bin tracks so %s can launch.",
     "Select a PlayStation BIOS and a Redump-style .cue with sibling .bin tracks so %s can launch."),
    ("the tooltip of a greyed Confirm, a BIOS that is prepared",
     '"Prepare this BIOS first, or switch to OpenBIOS"', '"Prepare this BIOS first"'),
    ("the tooltip of a greyed Confirm, a BIOS that is generated",
     '"Generate & rebuild with this BIOS first, or switch to OpenBIOS"',
     '"Generate & rebuild with this BIOS first"'),
    ("the box shown at PLAY",
     "rebuild with it (same disc and toolchain), switch to OpenBIOS to Play without rebuilding, "
     "or cancel and keep the current selection.",
     "rebuild with it (same disc and toolchain), or cancel and keep the current selection."),
    ("the tooltip of a greyed PLAY",
     '"Select a valid BIOS first (or Use OpenBIOS when this build allows it)."',
     '"Select a valid BIOS first."'),
]
for what, full, short in PAIRS:
    check(flat.count(full) == 1, f"{what}: the full form must stand once (found {flat.count(full)})")
    if flat.count(full) != 1:
        continue
    at = flat.index(full)
    check("launcher_model_bundled_bios_offered(m)" in flat[max(0, at - 420):at + len(full)],
          f"{what}: the full form must be inside the condition on the build holding a BIOS")
    if short is not None:
        check(flat.count(short) == 1, f"{what}: the short form must stand once (found {flat.count(short)})")
        if flat.count(short) == 1:
            check(at < flat.index(short) < at + 900,
                  f"{what}: the short form must be the other branch of the same choice")

# Two labels are left as they were: their boxes open only with a chosen file.
for label in ('"Selected: OpenBIOS"', '"Current selection: OpenBIOS"'):
    check(view.count(label) == 1, f"{label} must be left in place")
    if view.count(label) == 1:
        check("Not reached:" in view[view.index(label) - 400:view.index(label)],
              f"{label} must carry the note that says why it cannot be reached")

# Not checked here: the online-play notices that name OpenBIOS. They say which
# BIOS a match uses, not what this build offers.

if failures:
    for f in failures:
        print("FAIL:", f, file=sys.stderr)
    sys.exit(1)
print("bios step words: ok")
