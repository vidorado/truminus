#!/usr/bin/env python3
"""
gen_fs_ver.py — stamp a content hash of the web assets into data/fs.ver.

The self-OTA flashes the `littlefs` partition only when the web assets
actually changed (see main/p4_ota.cpp, firmware-ota skill). The device needs
a cheap, deterministic way to know "what web do I have" vs "what does this
release carry" without downloading the ~8 MB image:

  - This script hashes every file under data/ (the canonical sources) and
    writes the hex digest to `data/fs.ver`, which is then baked INTO the
    LittleFS image. So a device always knows its own web version by reading
    `/littlefs/fs.ver`, no matter how it was flashed (USB or OTA).
  - The release workflow publishes the same digest as the tiny `littlefs.ver`
    asset. The device compares the two; equal → skip the download entirely.

Run from web_assets_prep AFTER cache_bust.py (so the rewritten ?v= hashes are
part of the content) and gen_gz.py. Excludes:
  - `*.gz` siblings — derived artifacts, would make the hash depend on gzip.
  - `fs.ver` itself — its content is the output, never an input.

Deterministic across machines: files are walked in sorted POSIX-path order and
the path is folded into the digest alongside the bytes, so a rename changes the
hash even if the bytes don't.
"""

import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data"
MARKER = DATA / "fs.ver"


def main() -> int:
    if not DATA.is_dir():
        print(f"[gen_fs_ver] no {DATA} — skipping", file=sys.stderr)
        return 0
    h = hashlib.sha256()
    n = 0
    for f in sorted(DATA.rglob("*"), key=lambda p: p.relative_to(DATA).as_posix()):
        if not f.is_file():
            continue
        if f.suffix.lower() == ".gz" or f == MARKER:
            continue
        rel = f.relative_to(DATA).as_posix()
        h.update(rel.encode("utf-8"))
        h.update(b"\0")
        h.update(f.read_bytes())
        h.update(b"\0")
        n += 1
    digest = h.hexdigest()
    # Only rewrite when the digest changes, so a no-op build doesn't perturb the
    # file mtime (keeps the LittleFS image deterministic / the bin step skipped).
    if MARKER.exists() and MARKER.read_text().strip() == digest:
        print(f"  [gen_fs_ver]  {digest}  (unchanged, {n} files)")
        return 0
    MARKER.write_text(digest + "\n")
    print(f"  [gen_fs_ver]  {digest}  ({n} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
