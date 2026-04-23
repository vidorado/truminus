"""
compress_fs.py — PlatformIO extra script

Gzip-compresses every file in data/ into a temporary directory
(.pio/build/<env>/gz_data/) and redirects the LittleFS image build
to use that directory instead of data/.

ESPAsyncWebServer (mathieucarbou fork) automatically detects .gz files
in LittleFS and serves them with:
    Content-Encoding: gzip
    Content-Type: <mime-type-of-original>
when the browser sends Accept-Encoding: gzip (all modern browsers do).
No server-side code changes are needed.

Result: the flash image contains only .gz files; the browser receives
compressed content transparently.  Typical saving: 60-75 % per file.

Add to platformio.ini [env] section:
    extra_scripts = pre:scripts/compress_fs.py
"""

Import("env")  # noqa: F821  (SCons injects this)

import gzip
import shutil
from pathlib import Path


def compress_data(source, target, env):  # noqa: ARG001
    src = Path(env.subst("$PROJECT_DATA_DIR"))
    dst = Path(env.subst("$BUILD_DIR")) / "gz_data"

    if not src.is_dir():
        print(f"[compress_fs] data dir not found: {src} — skipping")
        return

    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)

    total_in = total_out = 0
    for f in sorted(src.rglob("*")):
        if not f.is_file():
            continue
        rel  = f.relative_to(src)
        out  = dst / (str(rel) + ".gz")
        out.parent.mkdir(parents=True, exist_ok=True)
        raw  = f.read_bytes()
        data = gzip.compress(raw, compresslevel=9)
        out.write_bytes(data)
        total_in  += len(raw)
        total_out += len(data)
        pct = 100 * (1 - len(data) / len(raw)) if raw else 0
        print(f"  [compress_fs]  {rel:<40}  {len(raw):>6} → {len(data):>5} B  ({pct:.0f} % smaller)")

    pct_total = 100 * (1 - total_out / total_in) if total_in else 0
    print(f"  [compress_fs]  Total: {total_in} B → {total_out} B  ({pct_total:.0f} % saved)")

    # Redirect mklittlefs / mkspiffs to the compressed directory.
    # PROJECT_DATA_DIR is expanded at action-execution time (SCons lazy eval),
    # so replacing it here, before mklittlefs runs, takes effect correctly.
    env["PROJECT_DATA_DIR"] = str(dst)
    print(f"  [compress_fs]  FS image will be built from {dst}")


env.AddPreAction("buildfs", compress_data)
