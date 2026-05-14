"""
cache_bust.py — PlatformIO extra script  (pre:scripts/cache_bust.py)

Runs BEFORE compress_fs.py.  Rewrites data/index.html so every static
asset reference carries a content-derived ?v=<hash> querystring.  The
firmware ignores the querystring and serves the file by its base name,
but browsers cache by the full URL — so a change in any asset forces
clients to fetch the new copy without us touching Cache-Control.

Behaviour
- Hashes every file in data/ except index.html itself, using SHA-1
  truncated to 8 hex chars.  Hashes are deterministic, so the resulting
  index.html only changes in git when a referenced asset really
  changes.
- Updates href="X" / src="X" / src="X?v=OLD" attributes to point to
  X?v=NEW.
- Skips Windows-ADS files (foo:Zone.Identifier) and dotfiles, matching
  the filter in compress_fs.py.
- Idempotent: re-running the script on an already-busted index.html
  produces the same output.
- Writes the file back only if its content actually changed, to avoid
  spurious mtime updates that would invalidate the SCons build cache.
"""

Import("env")  # noqa: F821  (injected by SCons / PlatformIO)

import hashlib
import re
from pathlib import Path

HASH_LEN = 8

data_dir = Path(env.subst("$PROJECT_DATA_DIR"))
index    = data_dir / "index.html"

if not data_dir.is_dir() or not index.is_file():
    pass
else:
    # ── Discover assets ────────────────────────────────────────────────────
    def _skip(p: Path) -> bool:
        name = p.name
        return (":" in name) or name.endswith(".Identifier") or name.startswith(".")

    all_assets = []   # list of Path
    for f in sorted(data_dir.rglob("*")):
        if not f.is_file() or _skip(f):
            continue
        if f.resolve() == index.resolve():
            continue
        all_assets.append(f)

    css_files     = [f for f in all_assets if f.suffix.lower() == ".css"]
    non_css_files = [f for f in all_assets if f.suffix.lower() != ".css"]

    def _hash(b: bytes) -> str:
        return hashlib.sha1(b).hexdigest()[:HASH_LEN]

    hashes = {}   # asset path (posix, relative to data/) -> 8-hex-char hash

    # ── Pass 1: hash non-CSS assets ────────────────────────────────────────
    for f in non_css_files:
        hashes[f.relative_to(data_dir).as_posix()] = _hash(f.read_bytes())

    # ── Pass 2: rewrite url(...) inside CSS, then hash the resulting CSS ──
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
        hashes[f.relative_to(data_dir).as_posix()] = _hash(f.read_bytes())

    # ── Pass 3: rewrite href=/src= attributes in index.html ───────────────
    attr_re = re.compile(
        r'\b(href|src)="([^"?#]+)(\?[^"#]*)?(#[^"]*)?"',
        re.IGNORECASE,
    )

    html = index.read_text(encoding="utf-8")

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
        index.write_text(new_html, encoding="utf-8", newline="\n")
        changed_files.append(index.name)

    if changed_files:
        print(f"  [cache_bust]  updated: {', '.join(changed_files)}")
        for path, h in hashes.items():
            print(f"  [cache_bust]    {path:<32}  v={h}")
    else:
        print(f"  [cache_bust]  all asset references already up-to-date")
