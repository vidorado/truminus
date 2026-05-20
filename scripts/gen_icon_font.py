"""
Generate main/font_icons.ttf — a merged subset of FontAwesome 6 Free Solid
and FontAwesome 6 Brands containing only the glyphs used by the TruMinus UI.

Sources (FA 6.7.2, included in repo):
  scripts/fonts/fa6-free-solid-900.otf   — solid icons
  scripts/fonts/fa6-brands-400.ttf       — brand icons (bluetooth)
  scripts/fonts/fa6-regular-400.ttf      — regular icons (currently unused)

Usage:
    python3 scripts/gen_icon_font.py

Requires fonttools:
    pip install fonttools brotli   # brotli only needed for woff2 output
"""

import sys
import tempfile
from pathlib import Path

try:
    from fontTools import subset as ss
    from fontTools.ttLib import TTFont
    from fontTools.merge import Merger
except ImportError:
    sys.exit("fonttools not found — run: pip install fonttools")

ROOT       = Path(__file__).parent.parent
SOLID_SRC  = ROOT / "scripts" / "fonts" / "fa6-free-solid-900.ttf"  # TTF (not OTF) for merge compat
BRANDS_SRC = ROOT / "scripts" / "fonts" / "fa6-brands-400.ttf"
DST        = ROOT / "main" / "font_icons.ttf"

# Glyphs from FA6 Free Solid — keep in sync with #define FA_* in p4display.cpp
# and p4settings.cpp.
SOLID_GLYPHS = {
    # p4display.cpp
    0xF002: "magnifying-glass  FA_SEARCH    (scan buttons)",
    0xE3AF: "house-chimney     FA_HOUSE_CHIM",
    0xF013: "cog               FA_COG",
    0xF043: "tint/water-drop   FA_TINT",
    0xF053: "chevron-left      FA_CHEVRON_L",
    0xF054: "chevron-right     FA_CHEVRON_R",
    0xF0D7: "caret-up          FA_CARET_U   (setpoint / fan-level)",
    0xF0D8: "caret-down        FA_CARET_D   (setpoint / fan-level)",
    0xF074: "shuffle/random    FA_RANDOM",
    0xF0E7: "bolt/lightning    FA_BOLT",
    0xF1EB: "wifi              FA_WIFI",
    0xF2C9: "thermometer-half  FA_THERM_HALF",
    0xF7E4: "fire-flame-curved FA_FIRE",
    # p4settings.cpp — settings menu
    0xF0E0: "envelope          FA_ENVELOPE  (MQTT)",
    0xF185: "sun               FA_SUN       (Solar/BLE)",
    0xF108: "display/desktop   FA_DISPLAY   (Display settings)",
    0xF0AC: "globe             FA_GLOBE     (Language)",
    0xF0C2: "cloud             FA_CLOUD     (WSS reverse-tunnel status)",
    # p4settings.cpp — password toggle / navigation
    0xF06E: "eye               FA_EYE",
    0xF070: "eye-slash         FA_EYE_SLASH",
    0xF2F6: "right-to-bracket  FA_SIGN_IN",
    # LVGL built-in symbols used by lv_keyboard (LV_SYMBOL_*)
    # LV_SYMBOL_NEW_LINE (U+F8A2) is not in FA6 Free; it is aliased below to U+F2F6
    # so the Enter key renders as the right-to-bracket arrow (user request).
    0xF00C: "check             LV_SYMBOL_OK        (keyboard confirm / tick)",
    0xF00D: "xmark             LV_SYMBOL_CLOSE     (keyboard close / X)",
    0xF55A: "delete-left       LV_SYMBOL_BACKSPACE (keyboard backspace)",
    0xF11C: "keyboard          LV_SYMBOL_KEYBOARD  (mode-switch button in all kb maps)",
}

# Glyphs from FA6 Brands — bluetooth is brand-only, not in Solid.
BRANDS_GLYPHS = {
    0xF293: "bluetooth         FA_BLUETOOTH (BLE status)",
}

ALL_GLYPHS = {**SOLID_GLYPHS, **BRANDS_GLYPHS}
# Virtual alias verified separately (not in source fonts, added via cmap patch).
ALIAS_GLYPHS = {0xF8A2: "enter arrow (aliased from U+F2F6)  LV_SYMBOL_NEW_LINE"}


def subset_font(src: Path, codepoints: list, label: str) -> str:
    opts = ss.Options()
    opts.layout_features = []
    opts.name_IDs        = [1, 2, 4, 5]
    opts.recalc_bounds   = True

    font      = ss.load_font(str(src), opts)
    subsetter = ss.Subsetter(opts)
    subsetter.populate(unicodes=codepoints)
    subsetter.subset(font)

    tmp = tempfile.mktemp(suffix=f"_{label}.ttf")
    ss.save_font(font, tmp, opts)
    return tmp


def main():
    for src in (SOLID_SRC, BRANDS_SRC):
        if not src.exists():
            sys.exit(f"Source font not found: {src}")

    solid_tmp  = subset_font(SOLID_SRC,  list(SOLID_GLYPHS.keys()),  "solid")
    brands_tmp = subset_font(BRANDS_SRC, list(BRANDS_GLYPHS.keys()), "brands")

    DST.parent.mkdir(parents=True, exist_ok=True)
    merger = Merger()
    merged = merger.merge([solid_tmp, brands_tmp])
    merged.save(str(DST))

    # Alias U+F8A2 (LV_SYMBOL_NEW_LINE) → same glyph as U+F2F6 (right-to-bracket).
    # FA6 has no glyph at F8A2; this makes the keyboard Enter key render as the
    # right-to-bracket arrow while LVGL's internal NEW_LINE logic still works.
    out = TTFont(str(DST))
    sign_in_glyph = out.getBestCmap().get(0xF2F6)
    if sign_in_glyph:
        for subtable in out['cmap'].tables:
            if hasattr(subtable, 'cmap'):
                subtable.cmap[0xF8A2] = sign_in_glyph
        out.save(str(DST))

    # Verify
    out  = TTFont(str(DST))
    cmap = out.getBestCmap()
    print(f"Written: {DST}  ({DST.stat().st_size} bytes)")
    print()
    ok = True
    for cp, desc in sorted(ALL_GLYPHS.items()):
        found = cp in cmap
        src   = "brands" if cp in BRANDS_GLYPHS else "solid"
        print(f"  U+{cp:04X} [{src:6s}]  {'✓' if found else '✗ MISSING'}  {desc}")
        if not found:
            ok = False
    for cp, desc in sorted(ALIAS_GLYPHS.items()):
        found = cp in cmap
        print(f"  U+{cp:04X} [alias ]  {'✓' if found else '✗ MISSING'}  {desc}")
        if not found:
            ok = False
    if not ok:
        sys.exit("\nSome glyphs are missing from the source fonts.")
    print("\nAll glyphs present.")


if __name__ == "__main__":
    main()
