#!/usr/bin/env python3
"""Build assets/common/img/flags.png: every flag Noto Color Emoji ships, in a
26x26 grid indexed by the two letters of the region code (row = first letter,
column = second, 'A' = 0). Cells for codes that are not flags stay transparent.

Why a sheet: Windows' Segoe UI Emoji has no flag glyphs at all (a regional-
indicator pair renders as two boxed letters), and a platform with no emoji
provider has nothing. The launcher takes flags from this sheet on every
platform, so a country reads the same everywhere.

Source: Noto Color Emoji (SIL Open Font License 1.1) -- see flags.LICENSE.txt.
Requires fontTools and Pillow with libraqm.
"""
import sys
from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

FONT = sys.argv[1] if len(sys.argv) > 1 else '/usr/share/fonts/noto/NotoColorEmoji.ttf'
OUT = sys.argv[2] if len(sys.argv) > 2 else 'assets/common/img/flags.png'
CELL_W, CELL_H = 51, 48          # Noto's 136x128 strike at 3/8: taller than any
                                 # size the launcher draws a flag at (2x HiDPI body)
STRIKE = 109                      # the only size the CBDT font renders at

tt = TTFont(FONT)
cmap = tt.getBestCmap()
ri = {cmap[cp]: chr(ord('A') + cp - 0x1F1E6) for cp in range(0x1F1E6, 0x1F200) if cp in cmap}
flags = set()
for lk in tt['GSUB'].table.LookupList.Lookup:
    for st in lk.SubTable:
        if st.LookupType == 7:
            st = st.ExtSubTable
        if st.LookupType != 4:
            continue
        for first, ligs in st.ligatures.items():
            if first not in ri:
                continue
            for lig in ligs:
                if len(lig.Component) == 1 and lig.Component[0] in ri:
                    flags.add(ri[first] + ri[lig.Component[0]])

font = ImageFont.truetype(FONT, STRIKE, layout_engine=ImageFont.Layout.RAQM)
sheet = Image.new('RGBA', (26 * CELL_W, 26 * CELL_H), (0, 0, 0, 0))
for code in sorted(flags):
    text = ''.join(chr(0x1F1E6 + ord(c) - ord('A')) for c in code)
    cell = Image.new('RGBA', (136, 128), (0, 0, 0, 0))
    ImageDraw.Draw(cell).text((0, 0), text, font=font, embedded_color=True)
    if not cell.getbbox():
        print('blank:', code, file=sys.stderr)
        continue
    small = cell.resize((CELL_W, CELL_H), Image.LANCZOS)
    sheet.paste(small, ((ord(code[1]) - ord('A')) * CELL_W, (ord(code[0]) - ord('A')) * CELL_H))
# A 256-colour palette is a third of the RGBA size. Only libimagequant keeps
# the anti-aliased edges' alpha intact (Pillow's octree quantizer flattens it).
sheet = sheet.quantize(256, method=Image.Quantize.LIBIMAGEQUANT)
sheet.save(OUT, optimize=True)
print(f'{len(flags)} flags -> {OUT}')
