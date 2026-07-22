#!/usr/bin/env python3
"""Flatten every Kconfig option in the local ESP-IDF tree into one greppable
index, so a keyword search finds the option, its help text and its source file
without walking the nested Kconfig hierarchy by hand.

The default IDF location matches CLAUDE.md (~/esp/esp-idf); override with
--idf. Output is a block-per-option text file (see FORMAT below), ordered by
config name. Re-run after switching IDF versions.

`--if-stale` regenerates only when the IDF version token (git HEAD, else the
version.cmake string) differs from the one recorded in the existing index, so
it is cheap to wire into the CMake configure step. See the esp32p4-docs skill.

Usage:
    ./gen_config_index.py [--idf ~/esp/esp-idf] [-o config_index.txt] [--if-stale]
"""
import argparse
import os
import re
import subprocess
import sys

# One block per option, blank-line separated. Every field is on its own line so
# `rg -B1 -A6 CONFIG_CACHE_L2` lands on the whole block.
#
#   CONFIG_<NAME>
#     type:   bool|int|hex|string|tristate
#     prompt: <menuconfig prompt or (none)>
#     file:   <idf-relative path>:<line>
#     help:   <help text, newlines collapsed to spaces>

# `choice <NAME>` blocks carry the group-level prompt/help (e.g. the L2 cache
# size choice); named choices often share their symbol with a derived config,
# and the richer of the two wins in `out`. Anonymous choices have no symbol to
# key on, so they are skipped — their member configs stay individually indexed.
CONFIG_RE = re.compile(r"^(\s*)(menuconfig|config|choice)\s+([A-Za-z0-9_]+)\s*$")
TYPE_RE = re.compile(
    r'^\s*(bool|int|hex|string|tristate)\b\s*(?:"([^"]*)")?'
)
PROMPT_RE = re.compile(r'^\s*prompt\s+"([^"]*)"')
HELP_RE = re.compile(r"^(\s*)help\b")


# Marker line the index carries so `--if-stale` can tell whether the recorded
# IDF matches the current one without reparsing 290 Kconfig files.
TOKEN_PREFIX = "# IDF-TOKEN: "


def idf_token(idf: str) -> str:
    """A string that changes whenever the IDF version changes: the git HEAD if
    IDF is a checkout, else the version.cmake contents (release tarball)."""
    try:
        head = subprocess.run(
            ["git", "-C", idf, "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=10,
        )
        if head.returncode == 0 and head.stdout.strip():
            return "git:" + head.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    vfile = os.path.join(idf, "tools", "cmake", "version.cmake")
    try:
        with open(vfile, "r", errors="replace") as fh:
            return "ver:" + re.sub(r"\s+", "", fh.read())
    except OSError:
        return "unknown"


def recorded_token(index_path: str) -> str:
    try:
        with open(index_path, "r", errors="replace") as fh:
            for _ in range(6):  # token lives in the header
                line = fh.readline()
                if not line:
                    break
                if line.startswith(TOKEN_PREFIX):
                    return line[len(TOKEN_PREFIX):].strip()
    except OSError:
        pass
    return ""


def indent(line: str) -> int:
    return len(line) - len(line.lstrip(" \t"))


def parse_file(path: str, rel: str, out: dict) -> None:
    with open(path, "r", errors="replace") as fh:
        lines = fh.readlines()

    i = 0
    n = len(lines)
    while i < n:
        m = CONFIG_RE.match(lines[i].rstrip("\n"))
        if not m:
            i += 1
            continue
        base = len(m.group(1))
        name = m.group(3)
        lineno = i + 1
        ctype = ""
        prompt = ""
        help_parts = []
        j = i + 1
        while j < n:
            raw = lines[j].rstrip("\n")
            if raw.strip() == "":
                j += 1
                continue
            # Any nested config/menuconfig/choice (a choice's members sit at a
            # deeper indent) ends this body and is handled on its own turn, so
            # members stay individually indexed while the choice keeps its help.
            if CONFIG_RE.match(raw):
                break
            if indent(raw) <= base and raw.strip() and not raw.lstrip().startswith("#"):
                # dedented to a sibling keyword (endmenu, endchoice, source, ...)
                break
            tm = TYPE_RE.match(raw)
            if tm and not ctype:
                ctype = tm.group(1)
                if tm.group(2) and not prompt:
                    prompt = tm.group(2)
            pm = PROMPT_RE.match(raw)
            if pm and not prompt:
                prompt = pm.group(1)
            hm = HELP_RE.match(raw)
            if hm:
                help_indent = indent(raw)
                j += 1
                while j < n:
                    hraw = lines[j].rstrip("\n")
                    if hraw.strip() == "":
                        help_parts.append("")
                        j += 1
                        continue
                    if indent(hraw) <= help_indent:
                        break
                    help_parts.append(hraw.strip())
                    j += 1
                continue
            j += 1

        help_text = " ".join(p for p in help_parts).strip()
        help_text = re.sub(r"\s+", " ", help_text)
        # Last declaration wins is wrong for Kconfig (options accumulate across
        # files); keep the first entry that carries a prompt or help, else any.
        key = "CONFIG_" + name
        richness = (1 if prompt else 0) + (1 if help_text else 0) + (1 if ctype else 0)
        prev = out.get(key)
        if prev is None or richness > prev[0]:
            out[key] = (richness, ctype, prompt, f"{rel}:{lineno}", help_text)
        i = j


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--idf", default=os.path.expanduser("~/esp/esp-idf"))
    ap.add_argument("-o", "--out", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "config_index.txt"))
    ap.add_argument("--if-stale", action="store_true",
                    help="regenerate only if the IDF version token changed; "
                         "otherwise no-op (cheap for build-time hooks)")
    args = ap.parse_args()

    idf = os.path.abspath(os.path.expanduser(args.idf))
    comp = os.path.join(idf, "components")
    if not os.path.isdir(comp):
        # A build-time --if-stale hook must not fail the build if IDF moved.
        msg = f"error: {comp} not found (wrong --idf?)"
        if args.if_stale:
            print(msg + " — skipping index refresh", file=sys.stderr)
            return 0
        print(msg, file=sys.stderr)
        return 1

    token = idf_token(idf)
    if args.if_stale and recorded_token(args.out) == token and token != "unknown":
        print(f"config index up to date ({token})")
        return 0

    out: dict = {}
    files = 0
    for root, _dirs, names in os.walk(comp):
        for name in names:
            if name == "Kconfig" or name.startswith("Kconfig."):
                path = os.path.join(root, name)
                rel = os.path.relpath(path, idf)
                parse_file(path, rel, out)
                files += 1

    with open(args.out, "w") as fh:
        fh.write(f"# ESP-IDF Kconfig option index — {len(out)} options "
                 f"from {files} Kconfig files under {os.path.relpath(comp, idf)}/\n")
        fh.write(f"# IDF: {idf}\n")
        fh.write(f"{TOKEN_PREFIX}{token}\n")
        fh.write("# Regenerate with gen_config_index.py (--if-stale for the "
                 "CMake configure hook) after switching IDF version.\n\n")
        for key in sorted(out):
            _r, ctype, prompt, loc, help_text = out[key]
            fh.write(f"{key}\n")
            fh.write(f"  type:   {ctype or '(unset)'}\n")
            fh.write(f"  prompt: {prompt or '(none)'}\n")
            fh.write(f"  file:   {loc}\n")
            fh.write(f"  help:   {help_text or '(none)'}\n\n")

    print(f"{len(out)} options from {files} Kconfig files -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
