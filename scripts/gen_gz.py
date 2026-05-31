#!/usr/bin/env python3
"""
gen_gz.py — pre-gzip compressible web assets next to their source.

main/webserver.cpp::serveFile() prefers a sibling .gz when present and
serves it with `Content-Encoding: gzip`.  Pre-compressing at build time
keeps the runtime simple (no on-the-fly compression in the firmware) and
shrinks every text asset to ~25-35% of its raw size on the wire.

Run before the LittleFS image is built.  Idempotent: an existing .gz is
skipped when it is newer than its source.
"""

import gzip
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data"

# Extensions worth gzipping.  .woff/.woff2 are already deflate-compressed;
# .ttf is raw and compresses 30-40%.  Images (png/ico/jpg) aren't included
# — they are already entropy-coded.
GZIP_EXTS = {".html", ".htm", ".css", ".js", ".svg", ".json", ".txt", ".ttf"}


def main() -> int:
    if not DATA.is_dir():
        print(f"[gen_gz] no {DATA} — skipping", file=sys.stderr)
        return 0
    total_in = total_out = 0
    for f in sorted(DATA.rglob("*")):
        if not f.is_file() or f.suffix.lower() not in GZIP_EXTS:
            continue
        gz = Path(str(f) + ".gz")
        if gz.exists() and gz.stat().st_mtime >= f.stat().st_mtime:
            total_in  += f.stat().st_size
            total_out += gz.stat().st_size
            continue
        raw = f.read_bytes()
        comp = gzip.compress(raw, compresslevel=9, mtime=0)
        gz.write_bytes(comp)
        pct = 100 * (1 - len(comp) / len(raw)) if raw else 0
        print(f"  [gen_gz]  {f.name:<28}  {len(raw):>7} -> {len(comp):>6} B  ({pct:.0f}%)")
        total_in  += len(raw)
        total_out += len(comp)
    if total_in:
        pct = 100 * (1 - total_out / total_in)
        print(f"  [gen_gz]  total: {total_in} -> {total_out} B  ({pct:.0f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
