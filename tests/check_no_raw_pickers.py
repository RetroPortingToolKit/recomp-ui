#!/usr/bin/env python3
"""Fail if a UI backend calls the blocking picker API directly.

WHY THIS TEST EXISTS
--------------------
launcher_files.h has two layers. `launcher_try_pick_*` is tri-state and
reports -1 for "this host has no usable file picker at all". The blocking
`launcher_pick_*` wrappers collapse that -1 into `false`, indistinguishable
from "the user pressed Cancel".

A UI that calls the blocking wrapper therefore CANNOT offer the built-in
browser, and on a host with no working zenity/kdialog its Browse button
silently does nothing. That is not hypothetical: it shipped. 14 of 17 picker
call sites in launcher_imgui.cpp were written that way, and Linux AppImage
players hit dead buttons on the memory-card, mod-install, shader, ROM-patch
and first-run-setup rows.

The fix was to route every picker through ui_pick(), which owns the
native -> built-in fallback. This test keeps it that way: the shape only
stays universal if a regression is loud.

If you are here because this test failed: do not add your file to the
allowlist. Use ui_pick() / ui_pick_file() / ui_pick_folder() /
ui_pick_save_file() instead.
"""

import os
import re
import sys

# Directories whose sources are user-facing UI and must use ui_pick().
UI_ROOTS = [os.path.join("src", "common", "backends")]

# The blocking wrappers. Calling any of these from a UI backend is the defect.
BANNED = [
    "launcher_pick_file",
    "launcher_pick_folder",
    "launcher_pick_save_file",
    "launcher_pick_rom",
]

# The one legitimate tri-state consumer: ui_pick() itself dispatches to these.
ALLOWED_TRISTATE = [
    "launcher_try_pick_file",
    "launcher_try_pick_folder",
    "launcher_try_pick_save_file",
]

SOURCE_EXT = (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp")

# A call, not a mention: the name followed by '(' and not preceded by an
# identifier character (so launcher_try_pick_file does not match
# launcher_pick_file).
CALL_RE = re.compile(
    r"(?<![A-Za-z0-9_])(" + "|".join(re.escape(n) for n in BANNED) + r")\s*\("
)


def strip_comments(text):
    """Blank out // and /* */ comments so prose about the API is not a hit."""
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        elif text[i] == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(text[i:j])
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    findings = []
    scanned = 0

    for ui_root in UI_ROOTS:
        base = os.path.join(root, ui_root)
        if not os.path.isdir(base):
            print("no such directory: %s" % base, file=sys.stderr)
            return 2
        for dirpath, _dirnames, filenames in os.walk(base):
            for name in sorted(filenames):
                if not name.endswith(SOURCE_EXT):
                    continue
                path = os.path.join(dirpath, name)
                with open(path, encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
                scanned += 1
                code = strip_comments(text)
                for match in CALL_RE.finditer(code):
                    line = code.count("\n", 0, match.start()) + 1
                    findings.append((path, line, match.group(1)))

    if not scanned:
        print("scanned no files - is the source root right?", file=sys.stderr)
        return 2

    if findings:
        print("FAIL: blocking picker API called from a UI backend", file=sys.stderr)
        print("", file=sys.stderr)
        for path, line, fn in findings:
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            print("  %s:%d: %s(" % (rel, line, fn), file=sys.stderr)
        print("", file=sys.stderr)
        print(
            "These collapse the tri-state -1 ('no usable native picker') into\n"
            "false ('user cancelled'), so the built-in browser fallback can\n"
            "never run and the button silently does nothing on hosts without\n"
            "a working zenity/kdialog.\n"
            "\n"
            "Use ui_pick() / ui_pick_file() / ui_pick_folder() /\n"
            "ui_pick_save_file() in launcher_imgui.cpp instead. They take a\n"
            "completion callback and handle the fallback for you.\n"
            "(%s stay available for ui_pick's own dispatch.)"
            % ", ".join(ALLOWED_TRISTATE),
            file=sys.stderr,
        )
        return 1

    print("ok: %d UI source files, no blocking picker calls" % scanned)
    return 0


if __name__ == "__main__":
    sys.exit(main())
