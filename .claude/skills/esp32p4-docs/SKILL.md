# Skill: ESP32-P4 / ESP-IDF documentation lookup (local RAG)

**Role:** Retrieve authoritative answers from the ESP-IDF documentation and
Kconfig options that ship with the exact IDF version this project builds
against — instead of answering ESP32-P4 / ESP-IDF platform questions from
memory.

**Read and use this skill before answering any question about ESP-IDF APIs,
Kconfig / `sdkconfig` options, ESP32-P4 hardware behaviour, memory/cache tuning,
performance trade-offs, or "does IDF support X".** It exists so config details
buried in Kconfig (e.g. the L2 cache size vs. available-RAM trade-off) are found
by search, not missed.

**Always consult this RAG — not just for direct lookups — whenever:**
- the problem is **complex** or spans several IDF subsystems (memory/heap,
  cache, PSRAM, DMA, SDIO/esp_hosted, TLS, bootloader, partitions…);
- you are **not sure** an option or behaviour exists, or you are working from
  memory rather than a cited source;
- a fix is **proving resistant** / a symptom does not add up — search here for a
  relevant `CONFIG_*` or a documented caveat before concluding;
- another skill (e.g. `pio-idf-p4`) covers the topic but you want to confirm
  nothing newer or adjacent was missed.

The whole point is to be *sure* nothing was overlooked. A quick search here is
cheap; a missed Kconfig knob (like L2 cache) is not. When in doubt, search.

---

## The corpus (already local — no scraping)

Everything lives in the local ESP-IDF checkout, on the same branch used to
build, so the docs always match the firmware:

- **Prose docs:** `~/esp/esp-idf/docs/en/**/*.rst` (~900 files) — guides
  (`api-guides/`), API reference (`api-reference/`), get-started, migration.
- **Kconfig options:** `~/esp/esp-idf/components/**/Kconfig*` (~290 files) —
  where every `CONFIG_*` option, its type, prompt and help text is declared.
  **Most tuning knobs live here, not in the prose.**
- **Precomputed option index:** `config_index.txt` (this skill dir) — all
  ~4250 `CONFIG_*` options flattened to one greppable block each
  (`name / type / prompt / file:line / help`). This is the fast path for
  "what does CONFIG_X do / where is it".

> Path override: the IDF root defaults to `~/esp/esp-idf` (per CLAUDE.md). If it
> moved, pass `IDF=/path` to the scripts below.

## How to search

**Preferred — the Grep tool** (native ripgrep, fastest). Point it at the paths
above. Examples:
- Option meaning/location: search `config_index.txt` for `CONFIG_SPIRAM` or
  `l2.*cache`.
- Prose trade-offs / how-to: search `~/esp/esp-idf/docs/en` with glob `*.rst`.
- Raw declaration + `default`/`depends on`: search `~/esp/esp-idf/components`
  with glob `Kconfig*`.

**Batch one-shot — `./search.sh <pattern>`** greps all three corpora at once
(index → prose → Kconfig) with section headers. Uses plain `grep` so it works
inside scripts where the shell's `rg` function is unavailable. Example:

```bash
.claude/skills/esp32p4-docs/search.sh 'l2.*cache'
```

Always read the cited `file:line` in the actual `.rst` / `Kconfig` for full
context before concluding — the index help text is a summary, not the last word.

## High-value P4 paths (start here for common topics)

| Topic | Path |
|-------|------|
| Speed / cache / RAM trade-offs | `docs/en/api-guides/performance/{speed,ram-usage,size}.rst` |
| PSRAM & flash config | `docs/en/api-guides/{external-ram,flash_psram_config}.rst` |
| Memory types / layout | `docs/en/api-guides/{memory-types,linker-script-generation}.rst` |
| Startup / app flow | `docs/en/api-guides/startup.rst` |
| Fatal errors / core dump | `docs/en/api-guides/{fatal-errors,core_dump}.rst` |
| Cache Kconfig (P4) | `$IDF_PATH/components/esp_system/port/soc/esp32p4/Kconfig.cache` |
| PSRAM Kconfig (P4) | `$IDF_PATH/components/esp_psram/esp32p4/Kconfig.spiram` |
| All `CONFIG_*` at a glance | `config_index.txt` |

## Keeping it in sync (automatic)

The index is a cache of the current checkout, so it must be regenerated whenever
the IDF version changes (`git -C ~/esp/esp-idf pull`, a branch switch, a new
`install.sh`). **This is wired into the CMake configure step** in the root
`CMakeLists.txt`, right after `apply_patches.py`:

```cmake
gen_config_index.py --if-stale --idf $ENV{IDF_PATH}
```

`--if-stale` records the IDF git HEAD (`# IDF-TOKEN:` in the index header) and
regenerates only when it differs — a no-op otherwise, so it costs one
`git rev-parse` per configure. **The next `idf.py build` after updating IDF
refreshes the index automatically.** A moved/missing IDF is a no-op (exit 0), so
the hook never breaks the build.

Manual regeneration, if ever needed:

```bash
.claude/skills/esp32p4-docs/gen_config_index.py            # default ~/esp/esp-idf
.claude/skills/esp32p4-docs/gen_config_index.py --idf /other/esp-idf
```

The `.rst`/`Kconfig` files themselves need no regeneration — they are read live.

## Version note

Corpus tracks the **local** IDF: currently `release-v6.0` (reports 6.0.1). The
online reference the user pinned is
[v6.0.2](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32p4/index.html);
minor-patch text differences are possible, but the **local corpus is
authoritative here because it is what the firmware compiles against**. When IDF
is bumped, re-run `gen_config_index.py` and update this line.

## Files in this skill

- `config_index.txt` — precomputed `CONFIG_*` index (regenerable, committed).
- `gen_config_index.py` — flattens all Kconfig options into the index.
- `search.sh` — one-shot grep across index + prose + Kconfig.
