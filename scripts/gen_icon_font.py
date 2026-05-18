"""
Generate main/font_icons.ttf — a minimal subset of FontAwesome 6 Free Solid
containing only the glyphs used by the TruMinus firmware UI.

Source: scripts/fonts/fa6-free-solid-900.otf  (FA 6.7.2, included in repo)

Usage:
    python3 scripts/gen_icon_font.py

Requires fonttools:
    pip install fonttools brotli   # brotli only needed for woff2 output
"""

import sys
from pathlib import Path

try:
    from fontTools import subset as ss
    from fontTools.ttLib import TTFont
except ImportError:
    sys.exit("fonttools not found — run: pip install fonttools")

ROOT = Path(__file__).parent.parent
SRC  = ROOT / "scripts" / "fonts" / "fa6-free-solid-900.otf"
DST  = ROOT / "main" / "font_icons.ttf"

# Codepoints used in p4display.cpp — keep in sync with #define FA_* macros.
GLYPHS = {
    0xE3AF: "house-chimney     FA_HOUSE_CHIM",
    0xF013: "cog               FA_COG",
    0xF043: "tint/water-drop   FA_TINT",
    0xF053: "chevron-left      FA_CHEVRON_L",
    0xF054: "chevron-right     FA_CHEVRON_R",
    0xF074: "shuffle/random    FA_RANDOM",
    0xF0E7: "bolt/lightning    FA_BOLT",
    0xF1EB: "wifi              FA_WIFI",
    0xF2C9: "thermometer-half  FA_THERM_HALF",
    0xF7E4: "fire-flame-curved FA_FIRE",
}

def main():
    if not SRC.exists():
        sys.exit(f"Source font not found: {SRC}\n"
                 "Download FA6 Free desktop package and place the OTF at that path.")

    options = ss.Options()
    options.layout_features = []     # drop GSUB/GPOS — not needed for icon display
    options.name_IDs = [1, 2, 4, 5] # keep family/version names
    options.recalc_bounds = True

    font = ss.load_font(str(SRC), options)
    subsetter = ss.Subsetter(options)
    subsetter.populate(unicodes=list(GLYPHS.keys()))
    subsetter.subset(font)

    DST.parent.mkdir(parents=True, exist_ok=True)
    ss.save_font(font, str(DST), options)

    # Verify
    out = TTFont(str(DST))
    cmap = out.getBestCmap()
    print(f"Written: {DST}  ({DST.stat().st_size} bytes)")
    print()
    ok = True
    for cp, desc in sorted(GLYPHS.items()):
        found = cp in cmap
        print(f"  U+{cp:04X}  {'✓' if found else '✗ MISSING'}  {desc}")
        if not found:
            ok = False
    if not ok:
        sys.exit("\nSome glyphs are missing from the source font.")
    print("\nAll glyphs present.")

if __name__ == "__main__":
    main()
