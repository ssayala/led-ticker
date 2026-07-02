#!/usr/bin/env python3
"""Generate the panel font (firmware/src/bold_font.h) from the MD_MAX72XX built-in.

Two per-glyph rules turn the stock table into the panel face:

1. Bold alphanumerics only. Letters and digits are emboldened by one pixel: we
   append a column and set new[i] = col[i] | col[i-1] (classic shift-and-OR).
   Vertical strokes become 2px thick while internal gaps stay open, so it reads
   as a clean bold rather than a blob. Non-alphanumerics — space, $, :, ., %, /,
   punctuation — are copied at stock weight, so numbers/letters look bold while
   symbols stay light (and narrower).
2. Fold lowercase onto uppercase. Each a-z code emits its A-Z glyph, so the
   panel renders all-caps whatever case the input is — an LED ticker reads
   better in caps, and lowercase bold looked muddy on 8 rows.

Emboldening grows a letter/digit's width by 1 (5->6), which is why the firmware
drops the steady sign cutoff (STATUS_STATIC_MAX_CHARS) from 5 to 4 — 5 wide bold
chars no longer fit the 32-column panel, so 5+ char signs scroll (scrolling has
no width limit). The steady clock ("H:MM") and timer ("MM:SS") still fit because
the colon is a regular-weight symbol and stays narrow. The firmware sets this
face once at init and uses it for everything.

Blank cells and space carry no set pixels, so they fall through to the
stock-weight copy and spacing stays tight.

Source is the library's stock table (MD_MAX72xx_font.cpp, USE_NEW_FONT block). Run
after `pio run` has fetched deps. The output header is committed; regenerate only
when the library font changes.

Usage:
  python3 tools/gen_bold_font.py [path/to/MD_MAX72xx_font.cpp]
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_FONT = REPO / "firmware/.pio/libdeps/esp32-s3/MD_MAX72XX/src/MD_MAX72xx_font.cpp"
OUT = REPO / "firmware/src/bold_font.h"

HEADER_LEN = 7  # 'F', version=2, firstHi, firstLo, lastHi, lastLo, height


def extract_bytes(src: str) -> list[int]:
    """Flatten the USE_NEW_FONT _sysfont[] initializer into a list of byte values."""
    m = re.search(r"_sysfont\[\]\s*=\s*\{", src)
    if not m:
        sys.exit("could not find _sysfont[] initializer")
    body = src[m.end():]
    # Keep only the USE_NEW_FONT arm (drops the #else legacy table).
    body = body.split("#else", 1)[0].split("#endif", 1)[0]
    body = body.split("#if USE_NEW_FONT", 1)[-1]

    vals: list[int] = []
    for raw in body.splitlines():
        line = raw.split("//", 1)[0]  # strip trailing comment
        for tok in line.split(","):
            tok = tok.strip()
            if not tok:
                continue
            if tok in ("{", "}", "};"):
                continue
            cm = re.fullmatch(r"'(.)'", tok)  # char literal, e.g. 'F'
            if cm:
                vals.append(ord(cm.group(1)))
            elif re.fullmatch(r"\d+", tok):
                vals.append(int(tok))
            else:
                sys.exit(f"unexpected token in font table: {tok!r}")
    return vals


# Hand-drawn overrides for glyphs the mechanical embolden mangles. Letters: A's
# apex squares into a flat-top box; M/W's chevron, N's diagonal and Q's tail blob
# up or merge with the ring in a narrow cell (M/W keep thin center arms so the
# V/peak stays open — thickening fills a solid bar). Symbols: the up/down arrows'
# open barbs fill into a solid blob when emboldened, so they get sharp bold
# triangles by hand. M/W and the arrows are 7 cols, others 4-6. Values are the
# column list, bit 0 = top row. Applied after a-z folding, so lowercase picks the
# letter shapes up too.
GLYPH_OVERRIDES = {
    "A": [0x7c, 0x7e, 0x13, 0x13, 0x7e, 0x7c],  # pointed apex
    "M": [0x7f, 0x7f, 0x02, 0x0c, 0x02, 0x7f, 0x7f],  # V to row 3
    "W": [0x7f, 0x7f, 0x20, 0x18, 0x20, 0x7f, 0x7f],  # peak to row 3
    "N": [0x7f, 0x7f, 0x06, 0x18, 0x7f, 0x7f],
    "Q": [0x3e, 0x7f, 0x41, 0x41, 0xff, 0xbe],
    "\x18": [0x08, 0x0c, 0x7e, 0x7f, 0x7e, 0x0c, 0x08],  # up arrow (sharp point, fat stem)
    "\x19": [0x08, 0x18, 0x3f, 0x7f, 0x3f, 0x18, 0x08],  # down arrow
}

# Symbols kept at stock (light) weight: their fine interior detail — the $ S and
# the degree ring — reads cleaner thin than emboldened, and a light glyph beside
# bold digits looks intentional. These skip the embolden step entirely.
KEEP_STOCK = frozenset("$\xb0")


def _embolden_cols(cols: list[int]) -> list[int]:
    """new[k] = cols[k] | cols[k-1], for k in 0..width (cols[-1]=cols[width]=0),
    yielding width+1 columns: a 2px-thick stroke that keeps internal gaps."""
    width = len(cols)
    return [((cols[k] if k < width else 0) | (cols[k - 1] if k > 0 else 0)) & 0xFF
            for k in range(width + 1)]


def embolden(vals: list[int]) -> list[int]:
    """Build the panel face from the stock table:

    - Embolden every glyph with content (letters, digits and symbols) by one
      shift-and-OR column for 2px-thick strokes. Exceptions: blank cells and
      space stay untouched (so spacing doesn't grow), the arrows are replaced by
      hand-drawn bold shapes (GLYPH_OVERRIDES), and $ and degree stay at stock
      weight (KEEP_STOCK) because their fine detail reads better thin.
    - Fold a-z onto the A-Z shapes: each lowercase code emits its uppercase
      glyph, so the panel physically renders all-caps whatever the input case.
      (An LED ticker reads better in caps; lowercase bold looked muddy.)
    """
    first = (vals[2] << 8) | vals[3]  # v2 header: 'F',2, firstHi,firstLo, lastHi,lastLo, height
    # Parse every glyph into a list indexed by (code - first); each entry is its
    # column list. We need the whole table up front so a-z can reach A-Z's cols.
    glyphs: list[list[int]] = []
    i = HEADER_LEN
    n = len(vals)
    while i < n:
        width = vals[i]
        i += 1
        glyphs.append(vals[i:i + width])
        i += width

    out = vals[:HEADER_LEN]
    for idx, cols in enumerate(glyphs):
        ch = chr(first + idx)
        tgt = chr(first + idx - 0x20) if "a" <= ch <= "z" else ch  # folded target
        src = cols
        if "a" <= ch <= "z":  # fold lowercase onto the uppercase glyph
            up = (first + idx - 0x20) - first
            if 0 <= up < len(glyphs):
                src = glyphs[up]
        if tgt in GLYPH_OVERRIDES:  # hand-drawn glyph beats the mechanical embolden
            g = list(GLYPH_OVERRIDES[tgt])
        elif tgt in KEEP_STOCK:  # leave at stock weight — fine detail reads better thin
            g = list(src)
        elif src and any(src):  # embolden every glyph with content — symbols included
            g = _embolden_cols(src)
        else:  # blank cells / space: unchanged, so spacing doesn't grow
            g = list(src)
        out.append(len(g))
        out.extend(g)
    return out


def render(vals: list[int]) -> str:
    lines = []
    for k in range(0, len(vals), 16):
        chunk = ", ".join(f"0x{b:02x}" for b in vals[k:k + 16])
        lines.append("  " + chunk + ",")
    body = "\n".join(lines)
    return f"""// Generated by tools/gen_bold_font.py — do not edit by hand.
// Panel font derived from the MD_MAX72XX built-in:
//   - Every glyph with content (letters, digits and symbols) is emboldened by
//     one shift-and-OR column for 2px-thick strokes; the arrows are hand-drawn
//     bold, and $ and degree stay at stock (light) weight. Bold glyphs are 1px
//     wider than stock, which is why STATUS_STATIC_MAX_CHARS is 4 (five wide
//     bold chars overflow the 32-col panel).
//   - a-z fold onto the A-Z glyphs, so the panel always renders all-caps.
// This is the panel's only font — set once at init and used for everything
// (ambient, signs, clock, timer, setup). See tools/gen_bold_font.py for why.
#pragma once
#include <MD_MAX72xx.h>

// const keeps this in flash-mapped .rodata on ESP32 (a non-const PROGMEM array
// would land in RAM). setFont() wants a non-const pointer but only ever reads
// the table (pgm_read_byte), so callers pass it via a const_cast.
const MD_MAX72XX::fontType_t BOLD_FONT[] PROGMEM = {{
{body}
}};
"""


def main() -> None:
    font_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_FONT
    if not font_path.exists():
        sys.exit(f"font source not found: {font_path}\nrun `pio run -d firmware` first to fetch deps")
    vals = extract_bytes(font_path.read_text())
    if vals[:2] != [ord("F"), 2]:
        sys.exit(f"unexpected font header {vals[:HEADER_LEN]} — expected v2 ('F',2,...)")
    bold = embolden(vals)
    OUT.write_text(render(bold))
    print(f"wrote {OUT} ({len(bold)} bytes, from {font_path})")


if __name__ == "__main__":
    main()
