"""
compress_fs.py — PlatformIO extra script  (pre:scripts/compress_fs.py)

Runs at *configuration* time (before any build targets are set up) so
that when PlatformIO's ESP32 platform scripts create the buildfs target
they pick up the compressed directory instead of data/.

What it does
------------
1. Reads every file from $PROJECT_DATA_DIR  (data/).
2. Gzip-compresses them (level 9) into
       .pio/build/<env>/gz_data/<file>.gz
3. Replaces PROJECT_DATA_DIR with that folder so mklittlefs
   builds the image from the .gz files.

Why this works on the browser side
-----------------------------------
ESPAsyncWebServer (mathieucarbou fork) checks for <name>.gz in LittleFS
when serving static files.  If found, and the browser sent
Accept-Encoding: gzip (all modern browsers do), it replies with the
compressed body and adds:
    Content-Encoding: gzip
    Content-Type: <mime-type-of-original>
The browser decompresses transparently — no server code changes needed.
"""

Import("env")  # noqa: F821  (injected by SCons / PlatformIO)

import gzip
import shutil
from pathlib import Path

# ── Resolve paths ──────────────────────────────────────────────────────────
src = Path(env.subst("$PROJECT_DATA_DIR"))   # normally  <project>/data
dst = Path(env.subst("$BUILD_DIR")) / "gz_data"

if not src.is_dir():
    # data/ doesn't exist for this env — nothing to do.
    pass
else:
    # ── (Re)create output directory ────────────────────────────────────────
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)

    # ── Compress every file ────────────────────────────────────────────────
    total_in = total_out = 0
    for f in sorted(src.rglob("*")):
        if not f.is_file():
            continue
        rel  = f.relative_to(src)
        out  = dst / (str(rel) + ".gz")
        out.parent.mkdir(parents=True, exist_ok=True)
        raw  = f.read_bytes()
        gz   = gzip.compress(raw, compresslevel=9)
        out.write_bytes(gz)
        total_in  += len(raw)
        total_out += len(gz)
        pct = 100 * (1 - len(gz) / len(raw)) if raw else 0
        print(f"  [compress_fs]  {str(rel):<42}  {len(raw):>7} → {len(gz):>6} B  ({pct:.0f}%)")

    if total_in:
        pct_t = 100 * (1 - total_out / total_in)
        print(f"  [compress_fs]  Total  {total_in:>7} → {total_out:>6} B  ({pct_t:.0f}% saved)")

    # ── Redirect the FS build to the compressed folder ─────────────────────
    # This must happen HERE (config time), not inside AddPreAction, because
    # the ESP32 platform bakes PROJECT_DATA_DIR into the mklittlefs command
    # string during target setup — too early for a pre-action callback.
    env.Replace(PROJECT_DATA_DIR=str(dst))
    print(f"  [compress_fs]  FS image will be built from {dst}")
