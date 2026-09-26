# TTF loading comparison, 2026-09-22

## Scope and evidence

Compared CrossInk `feat/scalable-ttf-streaming` at `67601607d` with the freshly
fetched CrossPoint `upstream/feat-ttf` at `44e1f906d`. CrossInk's pinned SDK is
`f52efeaa96440012cb2e1ad13a72ccec9b60c0c8`; that commit was available locally but
not from the configured SDK remote. No SDK changes are needed for this fix.

The local four-style Merriweather Sans family (522,604 bytes) and ChareInk7
family (5,309,504 bytes) both rendered successfully in the production adapter,
in resident and forced-streaming modes, with eight built-in faces still alive.
They did not report checksum mismatches. This does not establish that the files
on the reporting device are identical, or reproduce its intermittent failure.

## History

- `aba74b3bd`: added shared metrics/kerning caches and restored 150-DPI sizing,
  matching the bitmap converter. Keep these useful changes.
- `b1f3da104`: increased the bounded FreeType workspace and added diagnostics.
  Keep the bounded allocator and its recoverable failure paths.
- `4272c8252`: added streaming, with a full-file hash in 256-byte reads and an
  all-or-nothing family residency decision. A large family could force regular
  text to stream even when several individual faces would fit in PSRAM.
- `a14dd9ef3`: checked a regular glyph before accepting a font selection.
- `a1b5b7487`: expanded that check to every style. This initializes unused
  auto-hinters and reads their outlines for a regular-only preview; a failure in
  an unused style also rejects the entire family.
- `38139357f`: fixed multi-style catalog counting. Retain this fix.
- `67601607d`: batched hashing and added one 4 KiB stream window. It reduced
  read calls but increased read amplification as FreeType switched tables.

## CrossPoint comparison and selected adaptations

CrossPoint's `SdCardFontSystem::openTtfSource()` chooses resident/streamed storage
per source. `TtfEpdFont::load()` initializes regular first and defers the other
faces; GSUB copies have a 48 KiB limit and streamed GPOS copies are disabled.
Its glyph caches fill on demand. Recent history also includes extra-weight
filename filtering (`35cac585d`), packed advance tables (`621556ca7`), dynamic
kerning (`ab8822f6d`), and auto-hinting (`39cdd6152`, `afabf73b1`).

This patch adopts per-face residency, regular-first loading, the 48 KiB GSUB
copy budget, and regular-only selection probing. CrossInk still opens all file
sources to establish content identities and style-specific line metrics; this
is not a wholesale port of CrossPoint's lazy-face adapter. Other styles defer
expensive glyph/auto-hinter preparation until used.

It also divides the existing 4 KiB per-face stream buffer into four 1 KiB windows
and retries the exact requested read when read-ahead fails. Successful read-ahead
remains byte-for-byte equivalent to resident reads. Larger requests bypass the
windows. No new pixel arena or framebuffer is allocated.

Preserve one MiB of reader headroom plus descriptors and stream buffers for
pending faces. The regular face is considered first regardless of directory
order. Each remaining face independently falls back to streaming if it cannot
fit or its allocation fails. The shared FreeType arena is unchanged.

Resident and streamed layout identities now differ: streaming skips GPOS and
can change kerning. Previously identical font IDs could reuse layout generated
with different kerning. Adapter revision 8 invalidates affected old identities;
no binary cache format, bookmarks, or clipping format changes are needed.

A full-file content hash remains intentional: same-size in-place replacements
must invalidate cached layouts. Removing it without an equally reliable
replacement would trade startup speed for stale text layout. CrossPoint's
variable-font synthesis, broader file formats, extra-weight filtering, packed
word storage, and allocator/eviction design are separate work; they were not
copied into this focused loading fix. CrossInk retains its existing static-TTF
scope, render profiles, missing-style fallback, size reuse, and C3 capability gate.

## Instrumented I/O comparison

The test opens four styles alongside eight built-in faces and measures/renders
ASCII characters at 12, 16, 22, and 12 points. Counts include full-file hashing.
The historical adapter was compiled against the same pinned SDK. These are host
file-I/O counts, not SD latency or device-speed measurements. Host durations are
not comparable to e-ink refresh or MCU execution time.

| Font / adapter | Read calls | Bytes read |
| --- | ---: | ---: |
| ChareInk, before `67601607d` | 31,042 | 6,548,386 |
| ChareInk, `67601607d` | 6,620 | 27,276,504 |
| ChareInk, revised streamed path | 2,176 | 6,407,400 |
| ChareInk, resident path | 4 | 5,309,504 |
| Merriweather Sans, `67601607d` streamed | 6,287 | 25,427,024 |
| Merriweather Sans, revised streamed path | 971 | 1,416,084 |
| Merriweather Sans, resident path | 4 | 522,604 |

Resident and streamed glyph metrics and packed pixels are compared in the new
adapter test. The test also covers mixed residency, low/fragmented heap
snapshots, stable render-option identities, short-read cleanup, subsequent
reload, and recovery after an injected failed read-ahead. Snapshots test policy,
not actual ESP32 heap fragmentation.

## Validation performed

- `cmake -S test/scalable_fonts -B <build> -G 'Unix Makefiles'`, build, and
  `ctest --test-dir <build> --output-on-failure`: nine tests passed, including
  ASan/UBSan adapter/FreeType coverage and the native stack-frame compile gate.
- The adapter executable also passed with all four local Merriweather Sans and
  ChareInk7 files, comparing resident/streamed output.
- `pio run -e x4-pro-simulator -j1`: passed against the pinned SDK.
- X4 Pro simulator smoke with each family: passed size changes, dictionary
  restoration, oversized file/family rejection, replacement/deletion/reload,
  and reader controls. Simulator dependency: `fcf8e974c7144ea67ae4713d1d964e57ae61425c`.
- Formatting and `git diff --check`: passed.
- Independent `code-reviewer` review: no remaining P1-P3 findings. One existing
  missing-glyph/checksum warning was restored after review; the final simulator
  build and both font-family smoke checks were rerun.

This branch does not contain `config/device-resource-profiles.json` or
`scripts/device_resource_profiles.py`, so calibrated/stress resource-profile
smoke was unavailable. No ESP32 firmware build, flash, hardware timing, or task
watermark measurement is claimed. The simulator's reported heap is not an S3
resource measurement.

## Device verification

On the affected S3 reader, use the same SD card, book, point size, and font files.
Select Merriweather Sans and ChareInk7 from the font picker, then open passages
with regular, bold, italic, bold-italic, and accented text. Change sizes repeatedly,
open/close the dictionary, switch font families, sleep/resume, and return from
network transfers. Confirm that selection remains active and text uses the
chosen font. Capture any `TTF` error with its stage, FreeType code, file path,
stream-read flag, and arena request.

Measure cold selection and warm page composition separately from panel refresh.
For ChareInk, inspect `Resident font` / `Streaming font` logs to confirm that
regular gets residency first when memory permits. Record internal/PSRAM free
and largest blocks and task stack watermarks. Normal identity changes rebuild
affected layout automatically; clear only the affected book cache for a deliberate
cold-layout comparison. Device reliability and speed remain to be confirmed.
