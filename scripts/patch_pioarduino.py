import os
import re
from pathlib import Path

Import("env")

def patch_pioarduino_framework():
    """
    The pioarduino platform expects framework-arduinoespressif32@3.3.6,
    but the downloaded package.json may have a different version string.
    This causes platform.get_package_dir() to return None, breaking the build.
    We force the version to 3.3.6 and ensure pioarduino-build.py exists.
    """
    framework_dir = Path(os.path.expanduser("~/.platformio/packages/framework-arduinoespressif32"))
    if not framework_dir.exists():
        return

    # Patch package.json version
    package_json = framework_dir / "package.json"
    if package_json.exists():
        content = package_json.read_text()
        new_content = re.sub(r'"version":\s*"[^"]*"', '"version": "3.3.6"', content)
        if new_content != content:
            package_json.write_text(new_content)
            print("Patched framework-arduinoespressif32/package.json version to 3.3.6")

    # Ensure pioarduino-build.py exists (copy, not symlink, for portability)
    tools_dir = framework_dir / "tools"
    pioarduino_build = tools_dir / "pioarduino-build.py"
    platformio_build = tools_dir / "platformio-build.py"
    if platformio_build.exists() and not pioarduino_build.exists():
        import shutil
        shutil.copy2(platformio_build, pioarduino_build)
        print("Created pioarduino-build.py from platformio-build.py")

def patch_toolchain_path():
    """
    pioarduino installs the RISC-V toolchain with a hashed name (e.g. @src-...)
    or under tools/. PlatformIO then fails to find 'riscv32-esp-elf-g++'.
    We search for the actual toolchain bin directory and prepend it to PATH.
    """
    # Already works? Skip.
    from shutil import which
    if which("riscv32-esp-elf-g++"):
        return

    home = Path(os.path.expanduser("~/.platformio"))
    candidates = []

    # Search in packages (hashed names) and tools
    for base in [home / "packages", home / "tools"]:
        if not base.exists():
            continue
        for sub in base.iterdir():
            if sub.is_dir() and "toolchain-riscv32-esp" in sub.name:
                bin_dir = sub / "bin"
                if bin_dir.exists() and (bin_dir / "riscv32-esp-elf-g++").exists():
                    candidates.append(bin_dir)

    if candidates:
        # Prefer shortest path (non-hashed name if exists)
        candidates.sort(key=lambda p: len(str(p)))
        chosen = str(candidates[0])
        env.PrependENVPath("PATH", chosen)
        print(f"Prepended toolchain bin to PATH: {chosen}")

def patch_smartdisplay_idf5_compat():
    """
    esp32-smartdisplay 2.1.1 was written for ESP-IDF 4.x. On the C5 toolchain
    (pioarduino 55.x = IDF 5.x) two breakages appear:
      1. `esp_lcd_panel_io_spi_config_t.flags.dc_as_cmd_phase` was removed
         from the public struct.
      2. `<driver/spi_common_internal.h>` is no longer a public header.
    Comment out the offending lines so the rest of the library builds.
    The dc_as_cmd_phase flag is always `false` for our boards anyway, so
    dropping the initialiser is a no-op behaviourally.
    """
    libdeps_root = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") / "esp32_smartdisplay" / "src"
    if not libdeps_root.exists():
        return

    for src in libdeps_root.glob("*.c"):
        text = src.read_text()
        orig = text

        # Comment out dc_as_cmd_phase initialisers (line ~65 in each panel/touch file)
        text = re.sub(
            r'^(\s*)\.dc_as_cmd_phase\s*=\s*[^,]+,\s*$',
            r'\1// .dc_as_cmd_phase removed (IDF5 dropped this field)',
            text, flags=re.MULTILINE
        )

        # Strip private references to dc_as_cmd_phase in log_d() format args
        text = re.sub(
            r'\.flags\.dc_as_cmd_phase',
            r'0 /*dc_as_cmd_phase removed*/',
            text
        )

        # In IDF 5.x the internal SPI header moved from driver/ to esp_private/
        # (still exposes spi_bus_get_attr() which smartdisplay uses to detect
        # an already-initialised bus shared with the display).
        text = text.replace(
            "#include <driver/spi_common_internal.h>",
            "#include <esp_private/spi_common_internal.h>"
        )
        # If a previous run of this patch left the touch driver with the
        # public spi_common.h, restore the (now relocated) internal header
        # so spi_bus_get_attr() is declared.
        if "spi_bus_get_attr" in text and "esp_private/spi_common_internal.h" not in text:
            text = text.replace(
                "#include <driver/spi_common.h>",
                "#include <esp_private/spi_common_internal.h>"
            )

        if text != orig:
            src.write_text(text)
            print(f"[smartdisplay] patched {src.name} for IDF5 compatibility")

patch_pioarduino_framework()
patch_toolchain_path()
# esp32-smartdisplay#develop arregla `dc_as_cmd_phase` pero deja el include
# privado `driver/spi_common_internal.h` (que ya no es publico en IDF 5.x).
# Lo sustituimos por el header publico.
patch_smartdisplay_idf5_compat()

# Suppress C++-only warning globally for C++ files (not C)
env.Append(CXXFLAGS=["-Wno-literal-suffix"])
