"""Emit actual sizeof/alignof/offsetof witnesses for the frozen public header.

The accepted declaration inventory binds member names and callback signatures.
Compilation supplies layout values for the actual target, never guessed offsets.
This tool uses only the standard library and refuses drift from its source hash.
"""
import argparse
import csv
import hashlib
import re
from pathlib import Path

# Initial modern ABI major2 header. Historical major1 witnesses are unchanged. The
# scope's worktree hash is a CRLF rendering of this same source, not Git bytes.
PUBLIC_HEADER_SHA256 = "2f7d0a1d17ec2491f5e8f7dc1a850e118219569bf444ce6d41bfa4784a6448cf"
PUBLIC_INVENTORY_SHA256 = "af1a8d3c9671477e1b318566fba370ecfadca062555476bb7839136d78321ea3"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("header", type=Path)
    parser.add_argument("inventory", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--boundary-header", type=Path)
    parser.add_argument("--boundary-output", type=Path)
    args = parser.parse_args()
    # Windows checkouts can encode the accepted Git blob with CRLF. Normalize
    # that encoding alone; all declaration bytes still bind the frozen hash.
    if hashlib.sha256(args.header.read_bytes().replace(b"\r\n", b"\n")).hexdigest() != PUBLIC_HEADER_SHA256:
        raise SystemExit("Frozen public header changed; review a new major/layout first.")
    if hashlib.sha256(args.inventory.read_bytes().replace(b"\r\n", b"\n")).hexdigest() != PUBLIC_INVENTORY_SHA256:
        raise SystemExit("Frozen public declaration inventory changed; review its source mapping first.")
    with args.inventory.open(encoding="utf-8", newline="") as handle:
        rows = list(csv.DictReader(handle, delimiter="\t"))
    if len(rows) != 641 or len({row["struct"] for row in rows}) != 28:
        raise SystemExit("The accepted 641-declaration/28-structure inventory changed.")
    lines = ["/* Generated from the frozen public declaration inventory. Do not edit. */"]
    prior = None
    seen = set()
    for row in rows:
        record = row["struct"]
        declaration = " ".join(row["declaration"].split())
        callback = re.search(r"\(\s*\*\s*(\w+)\s*\)", declaration)
        if callback:
            members = [callback.group(1)]
        else:
            # Multiple declarators share their leading type, e.g. thumb_w, thumb_h.
            members = [re.search(r"(\w+)\s*(?:\[[^]]*\]\s*)*;?$", part.strip()).group(1)
                       for part in declaration.rstrip(";").split(",")]
        if record != prior:
            lines.append(f"RL_WITNESS_STRUCT({record})")
            prior = record
        escaped = declaration.replace("\\", "\\\\").replace('"', '\\"')
        for member in members:
            if (record, member) in seen:
                raise SystemExit(f"Duplicate witness member: {record}.{member}")
            seen.add((record, member))
            lines.append(f'RL_WITNESS_FIELD({record}, {member}, "{escaped}")')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"declarations={len(rows)} structures=28 member_witnesses={len(seen)}")
    if args.boundary_header or args.boundary_output:
        if not args.boundary_header or not args.boundary_output:
            raise SystemExit("Both boundary paths are required.")
        text = re.sub(r"/\*.*?\*/|//[^\n]*", "", args.boundary_header.read_text(encoding="utf-8"), flags=re.S)
        boundary = ["/* Generated from recomp_launcher_module.h. Do not edit. */"]
        total = 0
        for match in re.finditer(r"typedef struct (\w+)\s*\{(.*?)\}\s*\1\s*;", text, re.S):
            record, body = match.groups()
            boundary.append(f"RL_WITNESS_STRUCT({record})")
            for declaration in body.split(";"):
                declaration = " ".join(declaration.split())
                if not declaration:
                    continue
                callback = re.search(r"\([^)]*\*\s*(\w+)\s*\)", declaration)
                member = callback.group(1) if callback else re.search(r"(\w+)\s*(?:\[[^]]*\]\s*)*$", declaration).group(1)
                escaped = declaration.replace("\\", "\\\\").replace('"', '\\"')
                boundary.append(f'RL_WITNESS_FIELD({record}, {member}, "{escaped};")')
                total += 1
        # Callback typedefs also freeze parameter/return signatures. Their
        # parameter types cannot be inferred from a pointer-sized member.
        for header in (args.header, args.boundary_header):
            clean = re.sub(r"/\*.*?\*/|//[^\n]*", "", header.read_text(encoding="utf-8"), flags=re.S)
            for match in re.finditer(r"typedef\s+\w+\s*\([^;]*\*[^;]+;", clean):
                signature = " ".join(match.group(0).split()).replace('"', '\\"')
                boundary.append(f'RL_WITNESS_SIGNATURE("{signature}")')
        args.boundary_output.parent.mkdir(parents=True, exist_ok=True)
        args.boundary_output.write_text("\n".join(boundary) + "\n", encoding="utf-8", newline="\n")
        print(f"boundary_member_witnesses={total}")


if __name__ == "__main__":
    main()

