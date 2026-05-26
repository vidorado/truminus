#!/usr/bin/env python3
"""
Auto-apply patches to managed_components and PlatformIO platform files.

Called from CMakeLists.txt at cmake configure time (see CMakeLists.txt).
Each patch is idempotent: checks if already applied before modifying.

To add a new patch:
  1. Add a PATCHES entry below with old/new strings.
  2. Optionally add a .patch file in patches/ for documentation.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def apply(target: Path, old: str, new: str, description: str) -> None:
    """Replace old with new in target file. No-op if already applied."""
    if not target.exists():
        return
    content = target.read_text(encoding="utf-8", errors="replace")
    if new in content:
        return  # already patched
    if old not in content:
        print(f"[patches] WARNING: '{description}' — expected text not found in {target.name}, skipping")
        return
    target.write_text(content.replace(old, new, 1), encoding="utf-8")
    print(f"[patches] Applied '{description}' to {target}")


PATCHES = [
    # ── ESP-IDF component: esp_lcd_st7701 ─────────────────────────────────────
    # IDF 6.0 renamed color_space → rgb_ele_order in esp_lcd_panel_dev_config_t.
    # Patch: patches/esp_lcd_st7701_idf6.patch
    {
        "target": ROOT / "managed_components" / "espressif__esp_lcd_st7701" / "esp_lcd_st7701_mipi.c",
        "old":    "panel_dev_config->color_space",
        "new":    "panel_dev_config->rgb_ele_order",
        "desc":   "esp_lcd_st7701: color_space→rgb_ele_order for IDF 6.0",
    },

    # ── ESP-IDF component: jc4880_bsp (csvke/esp32_p4_jc4880p433c_bsp) ────────
    # IDF master split MIPI DSI panel config `pixel_format` into separate
    # `in_color_format` / `out_color_format` fields (renamed enum to FourCC
    # LCD_COLOR_FMT_*), and removed the `.flags.use_dma2d` field (DMA2D is
    # picked up automatically now).  Two separate substitutions because the
    # changes are in different lines of the same struct literal.
    # Patch: patches/jc4880_bsp_idf6.patch
    {
        "target": ROOT / "managed_components" / "jc4880_bsp" / "src" / "bsp_display.c",
        "old":    ".pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,",
        "new":    (
            "// IDF master: pixel_format split into in_/out_color_format,\n"
            "        // and the enum was renamed to LCD_COLOR_FMT_RGB565 (FourCC).\n"
            "        .in_color_format  = LCD_COLOR_FMT_RGB565,\n"
            "        .out_color_format = LCD_COLOR_FMT_RGB565,"
        ),
        "desc":   "jc4880_bsp: split pixel_format into in_/out_color_format for IDF 6.0",
    },
    {
        "target": ROOT / "managed_components" / "jc4880_bsp" / "src" / "bsp_display.c",
        "old":    ".flags = { .use_dma2d = true },",
        "new":    (
            "// IDF master removed use_dma2d (DMA2D is now used automatically when\n"
            "        // available); only disable_lp remains in the flags struct.\n"
            "        .flags = { .disable_lp = false },"
        ),
        "desc":   "jc4880_bsp: drop use_dma2d flag for IDF 6.0",
    },

    # ── PlatformIO espressif32 platform: bootloader linker-script includes ────
    # PlatformIO's espidf.py builds CFLAGS for the bootloader linker-script
    # preprocessor but omits the parent ld/ directory, so bootloader.sections.
    # common.ld (which lives one level up from esp32p4/) is not found.
    # Patch: patches/platformio_espidf_bootloader_includes.patch
    {
        "target": Path.home() / ".platformio" / "platforms" / "espressif32"
                  / "builder" / "frameworks" / "espidf.py",
        "old": (
            '    bootloader_extra_includes = [\n'
            '        str(Path(FRAMEWORK_DIR) / "components" / "bootloader" / "subproject" / "main" / "ld" / idf_variant)\n'
            '    ]'
        ),
        "new": (
            '    bootloader_extra_includes = [\n'
            '        str(Path(FRAMEWORK_DIR) / "components" / "bootloader" / "subproject" / "main" / "ld" / idf_variant),\n'
            '        str(Path(FRAMEWORK_DIR) / "components" / "bootloader" / "subproject" / "main" / "ld"),\n'
            '    ]'
        ),
        "desc":   "PlatformIO espidf.py: add main/ld/ to bootloader linker-script include paths",
    },
    # ── ESP-IDF component: esp_lvgl_port — GT911 I2C read crash ────────────
    # A transient I2C error (noise, bus contention) makes
    # esp_lcd_touch_read_data() return ESP_ERR_INVALID_RESPONSE.
    # The stock code wraps it in ESP_ERROR_CHECK which aborts the firmware.
    # Replace with a graceful return (report no-touch, retry next cycle).
    # Patch: patches/esp_lvgl_port_touch_graceful.patch
    {
        "target": ROOT / "managed_components" / "espressif__esp_lvgl_port"
                  / "src" / "lvgl9" / "esp_lvgl_port_touch.c",
        "old": (
            "    /* Read data from touch controller into memory */\n"
            "    ESP_ERROR_CHECK(esp_lcd_touch_read_data(touch_ctx->handle));\n"
            "\n"
            "    /* Read data from touch controller */\n"
            "    ESP_ERROR_CHECK(esp_lcd_touch_get_data(touch_ctx->handle, touch_data, &touch_cnt, CONFIG_ESP_LCD_TOUCH_MAX_POINTS));"
        ),
        "new": (
            "    /* Read data from touch controller into memory */\n"
            "    if (esp_lcd_touch_read_data(touch_ctx->handle) != ESP_OK) {\n"
            "        data->state = LV_INDEV_STATE_RELEASED;\n"
            "        return;\n"
            "    }\n"
            "\n"
            "    /* Read data from touch controller */\n"
            "    if (esp_lcd_touch_get_data(touch_ctx->handle, touch_data, &touch_cnt, CONFIG_ESP_LCD_TOUCH_MAX_POINTS) != ESP_OK) {\n"
            "        data->state = LV_INDEV_STATE_RELEASED;\n"
            "        return;\n"
            "    }"
        ),
        "desc": "esp_lvgl_port: graceful I2C error handling in touchpad read",
    },

    # Same patch for the versioned copy of the platform (if present).
    {
        "target": Path.home() / ".platformio" / "platforms"
                  / "espressif32@src-3ace0f14b41a59a9fa78289abea8a5b4"
                  / "builder" / "frameworks" / "espidf.py",
        "old": (
            '    bootloader_extra_includes = [\n'
            '        str(Path(FRAMEWORK_DIR) / "components" / "bootloader" / "subproject" / "main" / "ld" / idf_variant)\n'
            '    ]'
        ),
        "new": (
            '    bootloader_extra_includes = [\n'
            '        str(Path(FRAMEWORK_DIR) / "components" / "bootloader" / "subproject" / "main" / "ld" / idf_variant),\n'
            '        str(Path(FRAMEWORK_DIR) / "components" / "bootloader" / "subproject" / "main" / "ld"),\n'
            '    ]'
        ),
        "desc":   "PlatformIO espidf.py (versioned): add main/ld/ to bootloader linker-script include paths",
    },
]


def main():
    for p in PATCHES:
        apply(p["target"], p["old"], p["new"], p["desc"])


if __name__ == "__main__":
    main()
