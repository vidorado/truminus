#!/usr/bin/env python3
"""
gen_fa_subset.py — build data/fa-solid.woff + data/fa-brands.woff from the
FontAwesome 6 source fonts in scripts/fonts/.

Run after adding or removing icons to keep the woffs tiny.  The two output
files together carry only the glyphs the web UI references, so the network
cost stays in the low single-digit KB.

Glyph list mirrors what the LCD shows in main/p4display.cpp (FA_* macros).
"""

import sys
from pathlib import Path

from fontTools.subset import Subsetter
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parent.parent
FONTS_DIR = ROOT / "scripts" / "fonts"
OUT_DIR   = ROOT / "data"

# Codepoints used in the web UI.  Keep in sync with the FA_* macros in
# main/p4display.cpp.
SOLID = [
    0xF013,  # cog/gear (settings button)
    0xF015,  # home
    0xF043,  # tint (water boiler indicator)
    0xF054,  # chevron-right (outdoor-temp arrow fallback)
    0xF06D,  # fire (heating indicator)
    0xF074,  # random/shuffle (LIN status)
    0xF0C2,  # cloud (WSS tunnel status)
    0xF0E7,  # bolt (boiler boost)
    0xF2C9,  # thermometer-half
    0xF2F6,  # right-to-bracket (outdoor-temp arrow on P4 LCD)
    0xF7E4,  # fire-flame-curved (heating indicator on P4 LCD)
    0xE3AF,  # house-chimney (room indicator on P4 LCD)
    0xE55F,  # plug-circle-bolt (AC mains connected in INVERSOR panel)
    0xF177,  # arrow-left-long  (BAT discharging in INVERSOR panel)
    0xF178,  # arrow-right-long (BAT charging in INVERSOR panel)
    0xF2DC,  # snowflake (A/C cool and eco — CLIMATIZACIÓN panel)
]
BRANDS = [
    0xF293,  # bluetooth-b
]


def _subset(src: Path, dst: Path, codepoints: list[int]) -> None:
    font = TTFont(str(src))
    ss = Subsetter()
    ss.populate(unicodes=codepoints)
    ss.subset(font)
    font.flavor = "woff"
    font.save(str(dst))
    print(f"  {dst.name}: {dst.stat().st_size:>5} B  ({len(codepoints)} glyphs)")


def main() -> int:
    if not FONTS_DIR.is_dir():
        print(f"[gen_fa_subset] missing {FONTS_DIR}", file=sys.stderr)
        return 1
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    _subset(FONTS_DIR / "fa6-free-solid-900.otf", OUT_DIR / "fa-solid.woff",  SOLID)
    _subset(FONTS_DIR / "fa6-brands-400.ttf",     OUT_DIR / "fa-brands.woff", BRANDS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
