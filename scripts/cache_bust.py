#!/usr/bin/env python3
"""
cache_bust.py — standalone asset cache-busting for data/index.html.

Adds ?v=<SHA-1 8-char hash> querystrings to every href=/src= and CSS url()
that references a sibling asset in data/. The firmware ignores the query,
but browsers cache by full URL, so a change in any asset forces a refetch.

Deterministic: re-running on the same inputs produces the same output (no
spurious mtime updates).  Run before compress_fs.py from the top-level
CMakeLists.txt.
"""

import hashlib
import re
import sys
from pathlib import Path

HASH_LEN     = 8
PROJECT_ROOT = Path(__file__).resolve().parent.parent
DATA_DIR     = PROJECT_ROOT / "data"
INDEX        = DATA_DIR / "index.html"


def _skip(p: Path) -> bool:
    name = p.name
    return (":" in name) or name.endswith(".Identifier") or name.startswith(".")


def _hash(b: bytes) -> str:
    return hashlib.sha1(b).hexdigest()[:HASH_LEN]


def main() -> int:
    if not DATA_DIR.is_dir() or not INDEX.is_file():
        print(f"[cache_bust] no {INDEX} — skipping", file=sys.stderr)
        return 0

    all_assets = []
    for f in sorted(DATA_DIR.rglob("*")):
        if not f.is_file() or _skip(f):
            continue
        if f.resolve() == INDEX.resolve():
            continue
        all_assets.append(f)

    css_files     = [f for f in all_assets if f.suffix.lower() == ".css"]
    non_css_files = [f for f in all_assets if f.suffix.lower() != ".css"]

    hashes = {}   # path (posix, relative to data/) -> 8-hex-char hash

    # Pass 1: hash non-CSS assets
    for f in non_css_files:
        hashes[f.relative_to(DATA_DIR).as_posix()] = _hash(f.read_bytes())

    # Pass 2: rewrite url(...) inside CSS, then re-hash the resulting CSS.
    # url() accepts: url(X), url('X'), url("X"), with optional ?query.
    # data: URIs are left alone.
    css_url_re = re.compile(
        r'\burl\(\s*(["\']?)(?!data:)([^"\')?#]+)(\?[^"\')#]*)?(#[^"\')]*)?\1\s*\)',
        re.IGNORECASE,
    )

    def _css_sub(m: re.Match) -> str:
        quote = m.group(1)
        path  = m.group(2)
        frag  = m.group(4) or ""
        h = hashes.get(path)
        if h is None:
            return m.group(0)
        return f'url({quote}{path}?v={h}{frag}{quote})'

    changed_files = []
    for f in css_files:
        text = f.read_text(encoding="utf-8")
        new_text = css_url_re.sub(_css_sub, text)
        if new_text != text:
            f.write_text(new_text, encoding="utf-8", newline="\n")
            changed_files.append(f.name)
        hashes[f.relative_to(DATA_DIR).as_posix()] = _hash(f.read_bytes())

    # Pass 3: rewrite href=/src= in index.html
    attr_re = re.compile(
        r'\b(href|src)="([^"?#]+)(\?[^"#]*)?(#[^"]*)?"',
        re.IGNORECASE,
    )

    html = INDEX.read_text(encoding="utf-8")

    def _html_sub(m: re.Match) -> str:
        attr  = m.group(1)
        path  = m.group(2)
        frag  = m.group(4) or ""
        h = hashes.get(path)
        if h is None:
            return m.group(0)
        return f'{attr}="{path}?v={h}{frag}"'

    new_html = attr_re.sub(_html_sub, html)
    if new_html != html:
        INDEX.write_text(new_html, encoding="utf-8", newline="\n")
        changed_files.append(INDEX.name)

    if changed_files:
        print(f"  [cache_bust]  updated: {', '.join(changed_files)}")
    else:
        print(f"  [cache_bust]  all asset references already up-to-date")
    return 0


if __name__ == "__main__":
    sys.exit(main())
