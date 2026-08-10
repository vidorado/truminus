# Skill: LVGL fonts in TruMinus (Tiny TTF + FontAwesome 6)

**Role:** Specialist on font loading, glyph availability and icon management
for the TruMinus 800×480 LCD UI.

---

## 1. How fonts work at runtime

Fonts are **embedded into the firmware binary** via `EMBED_FILES` in
`main/CMakeLists.txt`. At boot, `load_fonts()` in `p4display.cpp` calls
`lv_tiny_ttf_create_data()` on each embedded TTF, creating `lv_font_t*` objects
that LVGL uses for on-demand glyph rasterisation. Any glyph present in the TTF
file is available — no pre-compilation step needed.

Embedded files:
| Variable | File | Purpose |
|----------|------|---------|
| `font_regular_ttf_*` | `main/assets/fonts/font_regular.ttf` | Body text — Latin + accents |
| `font_bold_ttf_*`    | `main/assets/fonts/font_bold.ttf`    | Section titles |
| `font_icons_ttf_*`   | `main/assets/fonts/font_icons.ttf`   | FA6 Free Solid icon subset |

---

## 2. Fallback chain

Every regular/bold font has the icon font set as `->fallback`:

```cpp
s_font_22->fallback = s_font_icons22;
s_font_24->fallback = s_font_icons24;
// etc.
```

This means a single label can mix Latin text and FA icons without switching
fonts — LVGL walks the fallback chain per glyph.

---

## 3. FA icon font — codepoints and defines

`main/assets/fonts/font_icons.ttf` is a **subset** of FA6 Free Solid 6.7.2.
The source OTF is committed at `scripts/fonts/fa6-free-solid-900.otf`.

Current codepoints in `#define FA_*` macros (`p4display.cpp`):

| Macro | UTF-8 | Unicode | FA name |
|-------|-------|---------|---------|
| `FA_TINT`       | `\xEF\x81\x83` | U+F043 | water-drop / tint |
| `FA_FIRE`       | `\xEF\x9F\xA4` | U+F7E4 | fire-flame-curved |
| `FA_HOUSE_CHIM` | `\xEE\x8E\xAF` | U+E3AF | house-chimney |
| `FA_THERM_HALF` | `\xEF\x8B\x89` | U+F2C9 | thermometer-half |
| `FA_CHEVRON_L`  | `\xEF\x81\x93` | U+F053 | chevron-left |
| `FA_CHEVRON_R`  | `\xEF\x81\x94` | U+F054 | chevron-right |
| `FA_WIFI`       | `\xEF\x87\xAB` | U+F1EB | wifi |
| `FA_RANDOM`     | `\xEF\x81\xB4` | U+F074 | shuffle / random |
| `FA_COG`        | `\xEF\x80\x93` | U+F013 | cog / gear |
| `FA_BOLT`       | `\xEF\x83\xA7` | U+F0E7 | bolt / lightning |

**UTF-8 encoding formula** for any FA codepoint in range U+F000–U+FFFF (3 bytes):
```
byte1 = 0xEF                          (1110_1111)
byte2 = 0x80 | ((cp >> 6) & 0x3F)
byte3 = 0x80 | (cp & 0x3F)
```
Example: U+F054 → byte2 = 0x80|(0xF054>>6 & 0x3F) = 0x81, byte3 = 0x80|(0x54 & 0x3F) = 0x94 → `\xEF\x81\x94`.

For U+E000–U+EFFF range (e.g. U+E3AF): same formula applies.

---

## 4. Adding a new icon glyph

1. Find the codepoint on fontawesome.com (search → "Free" filter → click icon → check "Solid").
2. Add the codepoint to `GLYPHS` dict in `scripts/gen_icon_font.py`.
3. Regenerate the font:
   ```bash
   python3 scripts/gen_icon_font.py
   ```
   This reads from `scripts/fonts/fa6-free-solid-900.otf` and overwrites
   `main/assets/fonts/font_icons.ttf`. The script prints ✓/✗ for each glyph.
4. Add a `#define FA_NEWICON "\xNN\xNN\xNN"` in `p4display.cpp`.
5. Recompile and flash.

---

## 5. Regular/bold font — character ranges

`font_regular.ttf` and `font_bold.ttf` are full Latin-extended TTFs; Tiny TTF
renders any glyph they contain on demand. Confirmed ranges that matter:

- `32–127` — ASCII (always present)
- `160–382` — Latin Extended-A/B including `°` (U+00B0=176), accented chars
- `8211–8212` — en-dash, em-dash
- `8722` — mathematical minus sign (U+2212)

**Do NOT use `LV_SYMBOL_PLUS` / `LV_SYMBOL_MINUS`** in labels whose font is
set to our Tiny TTF fonts. Those are LVGL private-use glyphs (U+E7F3/E7F4)
that only exist in LVGL's built-in Montserrat font. Use `"+"` and `"-"` (plain
ASCII) instead.

---

## 6. LVGL keyboard — required glyphs in the icon font

`lv_keyboard` uses LVGL private-use codepoints for its built-in buttons.
**All of these must be present in `font_icons.ttf`** (the fallback for every font),
otherwise the key shows a blank box or a wrong glyph and the corruption persists.

| LVGL symbol | Unicode | FA6 name | Notes |
|-------------|---------|----------|-------|
| `LV_SYMBOL_OK`        | U+F00C | check            | keyboard confirm (all maps) |
| `LV_SYMBOL_CLOSE`     | U+F00D | xmark            | uppercase map only |
| `LV_SYMBOL_BACKSPACE` | U+F55A | delete-left      | all maps |
| `LV_SYMBOL_KEYBOARD`  | U+F11C | keyboard         | mode-switch btn in lower/num/spec maps |
| `LV_SYMBOL_LEFT`      | U+F053 | chevron-left     | all maps |
| `LV_SYMBOL_RIGHT`     | U+F054 | chevron-right    | all maps |
| `LV_SYMBOL_NEW_LINE`  | U+F8A2 | *(not in FA6)*   | Enter key — see alias below |

### LV_SYMBOL_NEW_LINE alias

FA6 Free has no glyph at U+F8A2. The cmap alias in `gen_icon_font.py` maps
U+F8A2 → same glyph as U+F2F6 (right-to-bracket / FA_SIGN_IN) so the Enter key
renders as an arrow while LVGL's internal NEW_LINE logic still works:

```python
out = TTFont(str(DST))
sign_in_glyph = out.getBestCmap().get(0xF2F6)
if sign_in_glyph:
    for subtable in out['cmap'].tables:
        if hasattr(subtable, 'cmap'):
            subtable.cmap[0xF8A2] = sign_in_glyph
    out.save(str(DST))
```

Always run `python3 scripts/gen_icon_font.py` after adding any keyboard-symbol
codepoint to regenerate `main/assets/fonts/font_icons.ttf`.

---

## 7. Custom keyboard map — Latin special mode

When the UI language is Spanish, `build_keyboard()` in `p4settings.cpp` replaces
the default special map with `s_kb_map_spec` which adds ñ, á é í ó ú ü, ¡, ¿
on row 2.

**C++ pitfall:** OR-combining `lv_buttonmatrix_ctrl_t` flags produces `int` which
C++ won't implicitly convert back. Use explicit cast macros:

```cpp
#define BMC(x) static_cast<lv_buttonmatrix_ctrl_t>(x)
#define KB_BTN(w)  BMC(LV_BUTTONMATRIX_CTRL_POPOVER | (w))
#define KB_CTRL(w) BMC(LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_CHECKED | (w))
#define KB_CHK(w)  BMC(LV_BUTTONMATRIX_CTRL_CHECKED | (w))
```

The button count in `s_kb_map_spec` (40) must match the ctrl array length exactly.
The enum for the special mode is `LV_KEYBOARD_MODE_SPECIAL` (not
`LV_KEYBOARD_MODE_TEXT_SPECIAL`) in LVGL 9.5.

---

## 8. EEZ Studio integration

The EEZ project (`ui/truminus_ui.eez-project`) is used for **visual layout
design only** — not for code generation. The workflow:

1. User makes pixel-level adjustments in EEZ Studio.
2. User shows the result; Claude reads the project file and mirrors the
   coordinate/size changes into `p4display.cpp`.

EEZ font config uses `embeddedFontFile` (base64 TTF, for editor rendering)
and `lvglRanges` in `"start-end"` format (e.g. `"8211-8212"`). The
`source.filePath` is `"../main/assets/fonts/font_regular.ttf"` (relative to `ui/`).

**Do not rely on EEZ-generated C code** — it is incompatible with the
Tiny TTF approach used in the firmware.
