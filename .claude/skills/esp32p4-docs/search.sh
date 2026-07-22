#!/usr/bin/env bash
# One-shot search across the local ESP-IDF P4 doc corpus: the precomputed
# Kconfig option index, the prose guides/reference (.rst), and the raw Kconfig
# files. Case-insensitive extended-regex; pass a grep -E pattern.
#
#   ./search.sh 'l2.*cache'
#   ./search.sh psram              # everything mentioning PSRAM
#   IDF=/other/esp-idf ./search.sh cache
#
# Uses plain `grep` so it runs regardless of ripgrep availability (the login
# shell's `rg` is a Claude Code function, absent inside scripts). When *you*
# search interactively, the Grep tool / rg is faster — this is the portable
# batch path.
set -euo pipefail

PAT="${1:-}"
if [ -z "$PAT" ]; then
  echo "usage: $0 <grep-extended-regex>" >&2
  exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IDF="${IDF:-$HOME/esp/esp-idf}"
INDEX="$HERE/config_index.txt"

echo "### CONFIG_ options (index)"
if [ -f "$INDEX" ]; then
  # Show the matching option's whole block (name + type/prompt/file/help).
  grep -i -E -B1 -A4 "$PAT" "$INDEX" || echo "  (no option matches)"
else
  echo "  (config_index.txt missing — run gen_config_index.py)"
fi

echo
echo "### Prose docs (docs/en/**.rst)"
if [ -d "$IDF/docs/en" ]; then
  grep -R -i -n -E --include='*.rst' "$PAT" "$IDF/docs/en" \
    | head -80 || echo "  (no prose matches)"
else
  echo "  ($IDF/docs/en missing — wrong IDF path?)"
fi

echo
echo "### Raw Kconfig (components/**/Kconfig*)"
grep -R -i -n -E --include='Kconfig*' "$PAT" "$IDF/components" \
  | head -60 || echo "  (no Kconfig matches)"
