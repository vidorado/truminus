"""
PlatformIO extra_scripts: delegate build/clean/upload to idf.py.

PlatformIO handles cmake configuration (IDE integration, IntelliSense) while
idf.py handles the actual build and flash. This is required because
PlatformIO's espidf builder generates linker scripts that are incompatible
with IDF 6.0 / ESP32-P4 (missing section mappings for non-contiguous regions).

PlatformIO's cmake+ninja does run for components and bootloader, but the final
app link is handled by idf.py via the AddPreAction on firmware.elf.
"""

import os
import subprocess
import sys
from pathlib import Path
from SCons.Script import AlwaysBuild, Default, DefaultEnvironment, COMMAND_LINE_TARGETS

env = DefaultEnvironment()

PROJECT_DIR = env.subst("$PROJECT_DIR")
IDF_PY = str(Path.home() / "esp" / "esp-idf" / "tools" / "idf.py")
IDF_ENV_SH = str(Path.home() / "esp" / "esp-idf" / "export.sh")
UPLOAD_PORT = env.subst("$UPLOAD_PORT") or "/dev/ttyACM0"


def _idf_env() -> dict:
    """Return IDF build environment.

    PlatformIO's platform.py resets IDF_PATH to "" in os.environ, which
    prevents export.sh from detecting it correctly.  We bypass this by:
    1. Restoring IDF_PATH before sourcing export.sh.
    2. Using env -0 (NUL-separated) to handle multi-line variable values.
    """
    idf_root = str(Path(IDF_PY).parent.parent)  # .../esp-idf/tools/idf.py → .../esp-idf
    idf_tools = str(Path.home() / ".espressif")   # where IDF installed tools/venv
    # Pre-set both vars so export.sh / activate.py find the correct virtualenv.
    # PlatformIO's platform.py resets IDF_PATH="" and IDF_TOOLS_PATH=~/.platformio,
    # which breaks activate.py's virtualenv lookup.
    pre = f'export IDF_PATH="{idf_root}"; export IDF_TOOLS_PATH="{idf_tools}"; '
    cmd = f'bash -c \'{pre}. {IDF_ENV_SH} >/dev/null 2>&1 && env -0\''
    try:
        raw = subprocess.check_output(cmd, shell=True)
    except subprocess.CalledProcessError:
        merged = os.environ.copy()
        merged["IDF_PATH"] = idf_root
        merged["IDF_TOOLS_PATH"] = idf_tools
        return merged
    merged = os.environ.copy()
    merged["IDF_PATH"] = idf_root
    merged["IDF_TOOLS_PATH"] = idf_tools
    for entry in raw.split(b"\x00"):
        entry_str = entry.decode(errors="replace")
        if "=" in entry_str:
            k, _, v = entry_str.partition("=")
            merged[k] = v
    return merged


def _run(args: list, **kwargs) -> None:
    idf_env = _idf_env()
    # Prefer the IDF virtualenv Python from PATH; fall back to absolute script path.
    idf_python = idf_env.get(
        "IDF_PYTHON_ENV_PATH",
        str(Path.home() / ".espressif" / "python_env" / "idf6.0_py3.12_env"),
    )
    python_bin = str(Path(idf_python) / "bin" / "python")
    result = subprocess.run(
        [python_bin, IDF_PY] + args,
        cwd=PROJECT_DIR,
        env=idf_env,
        **kwargs,
    )
    if result.returncode != 0:
        sys.exit(result.returncode)


def build_idf(source, target, env):
    print("--- idf.py build ---")
    _run(["build"])


def clean_idf(source, target, env):
    print("--- idf.py fullclean ---")
    _run(["fullclean"])


def upload_idf(source, target, env):
    port = env.subst("$UPLOAD_PORT") or UPLOAD_PORT
    protocol = env.subst("$UPLOAD_PROTOCOL")
    if protocol == "espota":
        ota_host = env.subst("$UPLOAD_PORT") or "truminus.local"
        print(f"--- idf.py app-flash (OTA {ota_host}) ---")
        _run(["-p", ota_host, "app-flash"])
    else:
        print(f"--- idf.py flash ({port}) ---")
        _run(["-p", port, "flash"])


# Remove PlatformIO's own source-compile and link builders.
# The SCons build (firmware.elf) is handled entirely by idf.py.
#
# UPLOADCMD must be a callable Action (or a non-empty string); SCons rejects
# None with "$UPLOADCMD value None cannot be used to create an Action" when
# `pio run -t upload` tries to construct the upload action.  Pointing it at
# our upload_idf function makes the regular PlatformIO upload path delegate
# to idf.py too, matching the explicit `upload` alias below.
env.Replace(UPLOADCMD=upload_idf)
env.Replace(BUILDERS={})

# Run idf.py build as a pre-action before PlatformIO tries to link firmware.elf.
# This effectively makes idf.py the builder for the main app.
env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", build_idf)

# Named targets for pio run / pio run -t upload / pio run -t clean
build_target  = env.AlwaysBuild(env.Alias("build",  None, build_idf))
upload_target = env.AlwaysBuild(env.Alias("upload", None, upload_idf))
clean_target  = env.AlwaysBuild(env.Alias("clean",  None, clean_idf))

# pio run (no explicit target) triggers the build
if not COMMAND_LINE_TARGETS or "build" in COMMAND_LINE_TARGETS:
    Default(build_target)
