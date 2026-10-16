---
title: Languages
nav_order: 11
---

# Languages

CrossInk ships with English built in. Other menu languages come as small text files that you copy to your SD card and then pick on the reader. You can also edit these files to fix a translation or make your own. The same download also contains hyphenation packs, which let CrossInk split long words at the end of a line for more languages.

## Install or edit a language

1. Download the language ZIP for your firmware version from the [releases page](https://github.com/uxjulia/CrossInk/releases). Unzip it and copy its `.crosspoint/languages` folder to the top (root) folder of your SD card. You only need to keep the language files you want. Folders starting with a dot may be hidden in your file manager, so turn on "show hidden files" if you cannot see it.
2. On the reader, open **Settings > System > Device > Language** and select the language. The reader shows a loading screen, prepares the language, saves your choice, and restarts to the Home screen.
3. If you edit a language file later, select that language again to apply your changes. Editing the file alone does nothing, because the reader uses its own prepared copy. Keep the file on the SD card in case you want to reapply or reinstall it.

Good to know:

- Anything missing from a translation shows in English.
- A file with a mistake in it is rejected and your current language stays in use.
- Two files with the same language code are both disabled until you rename or remove one.
- When you update the firmware, your installed language keeps working. New or changed menu text shows in English until you reapply the file.
- Translations change menu text only. They do not add fonts for new alphabets, new keyboards, or new writing styles.
- Reapplying the same compiled language and metadata keeps the current generation without rewriting flash or restarting. Comments and formatting do not trigger an update; changing the order of compiled translation records does.

> [!NOTE]
> Advanced: the default firmware builds in English only (`custom_i18n_builtin_langs = en`). Custom builds can embed a comma-separated list of languages, or `all`. Installed languages are stored once in the reader's internal flash, so opening menus, reading, and waking never read translation files from the SD card. The prepared copy stays available if you remove the SD file, and it takes priority over a built-in translation of the same code. A matching SD file in the picker reinstalls that language even if it is also built in. Without a valid prepared copy, a built-in translation is used, otherwise English. The version label inside a YAML file is informational and not an exact-version lock. When upgrading from firmware that had built-in translations, your saved language code is kept. On an English-only build you see English until you install the SD file. The device log explains any failed check.

## Download on the reader

Builds configured with a download endpoint provide **Settings > System >
Device > Download Languages** and online installation in **Hyphenation Packs**.
Without a configured endpoint, online requests display **Downloads unavailable**.

Download Languages lists every language the firmware supports, each marked
**Built in** (English), **Current** (the language in use), **Installed** (a copy
is already on the reader or SD card), or **Install**. Selecting **Install**
restarts into the Wi-Fi screen, downloads the verified language, and activates
it with no further choices. Selecting an installed language offers **Activate**,
**Update**, and **Cancel**; the current language offers **Update** and **Cancel**.
**Update** downloads again the same way. Activation restarts to Home; your
reading position is kept, and you reopen the book normally. Cancelling the Wi-Fi
screen returns Home and changes nothing.

After a firmware update, the one-time **What's New** screen offers **Update** when
the active language pack is older than the firmware, so a single press downloads
the matching pack. The same screen is available under **Settings > System >
Release Notes**.

In Hyphenation Packs, packs that are not installed show **Install**. Selecting
one installs it right away: a pack already on SD is activated directly, otherwise
the reader restarts into the Wi-Fi screen, downloads the pack, and activates it
with no further choices. If your SD copy was edited, you are asked once before it
is replaced. Installed packs show **Installed** and offer **Update**, **Delete**,
and **Cancel**. Update goes straight to the Wi-Fi download and reinstall. Built-in
English remains visible without a strike-through and does nothing when selected.

A downloaded source is saved on SD before it is activated. If the SD file was
edited or is not an official copy, you are asked before it is replaced; the old
contents are discarded after successful publication. Cancel is selected first. Conflicting same-code files or case-only filename
collisions must be resolved before downloading. No permanent backup is promised.

Online translations must match the firmware's source and string contract. Updating
or downgrading firmware may make a catalog incompatible; manual SD files retain
their existing English-fallback behavior. Menu language and the language selected
from EPUB metadata remain independent.

## EPUB hyphenation packs

Hyphenation splits long words across lines. English is always built in. For other languages, install a pack:

1. Copy the `.crosspoint/hyphenation` folder from the language ZIP to the root of your SD card.
2. Open **Settings > Reader > Page Layout > Hyphenation Packs**.
3. Select the language marked **Activate**, then confirm **Activate**. The reader restarts to Home. Reopen your book afterward.

Choose **Delete** to remove an installed pack (this also restarts). The files on the SD card stay there so you can reinstall later. Editing or deleting a file on the SD card alone changes nothing until you activate it. To replace an installed pack from SD without Wi-Fi, delete the active pack first, then activate its retained SD source.

The initial packs cover French, German, Russian, Spanish, Italian, Polish, Portuguese, Swedish, and Ukrainian. A book uses the pack for its own language, whatever your menu language is. If a book needs a pack you have not installed, the reader offers once per reading session to open the manager. You can cancel and keep reading. Hyphens already in the text still work, and the **Hyphenation** setting still turns automatic hyphenation on or off.

Books are laid out again automatically after a pack changes, so you do not need to clear the cache. Your bookmarks and reading position are kept.

> [!NOTE]
> Advanced: files must be named like `hyph-de.cphyph`, with the same language code inside. The manager accepts upstream CPHY v1 files for these nine languages with the existing 2/2 word-boundary rules. Unsupported scripts, flags, versions, truncated files, and checksum mismatches are rejected. A pack cannot supply font glyphs, keyboards, or handling for a new script. Installed packs are read straight from internal flash, with no whole-dictionary RAM allocation and no SD access per word. Hyphenation uses the first `0x340000` bytes of the `spiffs` partition, split into two `0x1a0000` banks. The final two 64 KiB UI-language slots are untouched. An update copies retained packs into the inactive bank, verifies checksums, then publishes a commit marker. The running copy stays in use until restart, and boot can fall back to the older valid bank after an interruption. Release packaging includes all nine packs with checksums and the original pattern sources and license notices. To build just the hyphenation archive: `python3 scripts/package_hyphenation.py --output /tmp/crossink-hyphenation.zip`. Keep `assets/hyphenation/patterns` and its notices matched to the generated tables when updating pattern data.

## Create or edit a translation

To make your own language, copy `english_template.yaml` (included in the language ZIP; `english-template.yaml` is an older name for the same file), give the copy a name ending in lowercase `.yaml`, and change its language code and native name. You may delete entries you do not want to translate. A new language does not need a firmware update. Browser-based translation editors such as Inky can also open and export these files.

```yaml
_language_code: "MY-LANGUAGE"
_language_name: "My language"
_direction: "ltr"
_keyboard: "EN"

# Keep the STR_ names; edit the quoted text.
STR_SETTINGS_TITLE: "My settings"
STR_TIME_EXAMPLE: "Unknown keys are ignored"
```

- Metadata belongs before the first `STR_` entry. `_language_code` and
  `_language_name` are required. Codes are at most 31 ASCII letters, digits,
  hyphens, or underscores, start with a letter, and are normalized to uppercase.
  `EN` is reserved for the built-in English language. Native names are at most
  95 UTF-8 bytes. Keep names on one line.
- `_direction` is `ltr` or `rtl`. Without it, Arabic (`AR`) and Hebrew (`HE`)
  retain their existing RTL controls; other identities default to LTR.
- `_keyboard` selects an existing language's keyboard default. Unknown codes
  fall back to English. Explicitly enabled keyboard layouts still take precedence.
  Existing legacy codes such as `PT`, `PT2`, and `SI` retain their meanings.
- Use UTF-8 and double-quoted strings. Supported escapes are `\"`, `\\`, `\n`,
  and `\t`. Blank lines, whole-line comments, and comments after a closing quote
  are supported. YAML nesting, lists, multiline blocks, and Unicode escapes are
  not supported; type the Unicode characters directly.
- Missing or empty translations use English. Unknown `STR_` keys are ignored.
  For strings containing format placeholders, preserve their argument types and
  order, including length modifiers such as `%zu`. Unsafe/mismatched formats
  fall back to English for that entry. Positional arguments and `%n` are not
  supported. Do not translate placeholder syntax.
- Limits (a file that goes over them is rejected): 256 KiB source file, 2,047 bytes per source line, 127-byte key names,
  256 unknown/metadata keys, and a 64 KiB compiled cache including its metadata.
  The picker scans at most 64 YAML files, with filenames below 127 bytes.
  Codes, names, and filenames share a 2 KiB catalog budget (all 27 starter files
  fit together). If the catalog is too large, keep fewer files on the SD card;
  English and the active cached language remain available.
- Font glyph coverage and available keyboard layouts are unchanged. A new
  translation file cannot add missing font glyphs, shaping rules, or keyboards.

## Filename fallback fonts (X4 Classic, X4 Pro, Sticky)

If your book titles or filenames use an alphabet the built-in font does not have (for example Chinese), they can show as blank boxes. You can add a font to fill in those characters.

Open **Settings > System > Device > Filename Fallback Font**, directly below
Language, to choose it.
The built-in UI font continues to draw characters it supports, including Latin
characters inside mixed-script titles. Menu labels keep their built-in fonts.
Select **None** to disable filename fallback. This option is available on the
X4 Classic, X4 Pro, and Sticky, but not on the X3 or X4.

Copy static TrueType or OpenType CFF `.ttf` or `.otf` files into a family folder such as
`/.crosspoint/languages/fonts/Noto Sans SC/`. The folder name is the picker label.
Font metadata identifies the regular and bold faces; filenames are unrestricted.
A regular face is required, bold is optional, and missing bold glyphs use regular.
Italic faces and bitmap `.cpfont` packs are not offered. Each family must contain
at most one regular and one bold face. Variable fonts and font collections are
unsupported. Keep reading fonts in `/fonts` or `/.fonts`.

Each supported filename font can be up to **32 MiB**. The same face supplies the 8, 10 and
12 point UI sizes. Compact faces can reside in PSRAM; larger faces stream from
SD with bounded read and glyph caches. First use and newly visible characters
can take longer than cached redraws. The UI font is independent of the reader
font, but shares the bounded font-rendering workspace. Missing or unreadable
families recover to the built-in UI without changing the saved choice. Reopen
the picker and select the family again after changing its files on a computer.
A firmware restart also reloads the selected family.

## Advanced and maintainer details

The sections below are for people who build firmware, publish releases, or build translation tools. You do not need them to use a language.

### Flash storage and recovery

The existing `spiffs` data region is separate from the two firmware application
slots. CrossInk uses its final 128 KiB as two 64 KiB language slots. It does not
mount a filesystem there, resize partitions, or overwrite an OTA/rollback image.

CrossInk owns this firmware data partition. Applying an SD language initializes
its selected cache slot automatically, even if another firmware left files or
filesystem metadata there. Existing SPIFFS files are not preserved as a usable
filesystem; back them up beforehand if you need them. Only the selected 64 KiB
slot is erased and written during each installation, with the other slot left
intact. No manufacturer-specific file or layout detection is needed.

Booting, waking, and selecting English do not initialize flash storage. Removing
SD cache folders does not clear internal flash. Normal OTA and SD firmware
updates leave this region alone; a complete flash erase removes the installed
language. The partition table, both application slots, boot metadata, NVS,
coredump storage, and SD card are outside the language cache write range.

Installation writes the unselected slot and publishes it only after readback and
checksum validation. The current slot stays mapped until restart. Settings use
synced temporary files with backup recovery, so an interrupted selection can
return to the previous language. As with other SD operations, physical card or
filesystem damage can still require recovery from a backup.

### Maintaining firmware and releases

The generator reads `english.yaml`, `languages.json`, and only the YAML files
selected by `custom_i18n_builtin_langs`. `en` is the default; `en,es,fr` embeds
those three languages, and `all` embeds all registered starter languages. Invalid
codes, duplicate selected identities, or missing selected sources fail the build.
Missing translations and unsafe printf contracts fall back to English. Unselected
starter files are not validated or embedded by an English-only build.
`languages.json` freezes legacy enum/code/name compatibility; it is not the SD
language catalog. Adding an English key does not require changes to community
translations. Prefer `_FORMAT` for new printf-format keys; existing exceptions
are listed in `scripts/gen_i18n.py` so their argument contracts are checked.
Do not repurpose an existing `STR_` key for an unrelated meaning.

Release and release-candidate workflows attach a language ZIP. To create one:

```sh
python3 scripts/package_languages.py --version 1.5.2 --output /tmp/crossink-languages.zip
```

Community files remain starter assets and may be incomplete. Missing keys do not
block firmware builds. The release template identifies the corresponding
English source, and the ZIP manifest records checksums.

#### Automatic template for translation editors

`scripts/generate_language_template.py` generates `english_template.yaml` and
`language-template.json` from the same `english.yaml` and `gen_i18n.py` rules used
by firmware. No second string catalog is maintained. Existing `STR_` IDs and
English source order are preserved, including IDs currently unused by firmware.
The JSON's `active` flag uses the same source scan as firmware key stripping.
Only active nonempty translations that differ from English consume cache records.
Empty translations mean English fallback; whitespace-only translations are literal
text in the SD parser.

Editors should load the **single JSON document** at
`https://raw.githubusercontent.com/uxjulia/CrossInk/main/docs/languages/template/language-template.json`.
This public generated source supports browser cross-origin fetching. It contains the
English entries, printf argument signatures, parser limits, legacy keyboard
identities, and exact source provenance together, so two requests cannot mix
releases. `schemaVersion: 1` describes this editor contract. `format` names the
firmware parser format. `sourceCommit` identifies the source snapshot, and
`firmwareVersion` is informational rather than a device version lock.
`englishSourceSha256` hashes the exact English source bytes; `keysetSha256` hashes
all ordered entry keys joined by LF **with a trailing LF**. The companion YAML's
exact SHA-256 is recorded in `template.sha256`. All hashes use UTF-8 bytes.

Stable release builds generate the catalog, template and editor starter assets.
They attach the catalog, template and language ZIP to the draft release and copy
the same editor assets beside the existing Pages release catalog. The site asset sync copies `docs/languages/template` to
`/languages/template`, also available through the CrossInk website. The JSON
reports `status: "release-build"`: the workflow
publishes the Pages catalog before a human publishes its draft GitHub Release,
so this status deliberately does not claim that a release has been published.
Release candidates attach their own assets but do not replace the stable Pages
copy.

For each available non-English starter source, a language identity in the JSON
has an optional `translation` descriptor with `filename`, `sha256`, and
`omittedKeys`. Paths have the form `translations/<sha256>.yaml`, relative to the
catalog. Inky loads and checksum-validates the selected language file, checks
its language identity and template provenance, then opens those translations
for local editing. The catalog remains fixed during editing. Template changes
cannot redirect an old catalog to different translation text: filenames are
content-addressed and stable publication adds new files while preserving older
hashes. The ZIP also includes these editor starter files.

Editor starters are generated from the existing community YAML sources. They
contain only current canonical keys and preserve nonempty translations exactly,
including English-identical and whitespace-only values. Obsolete keys, empty
values and unsafe printf contracts are omitted for English fallback.
`omittedKeys` counts entries removed from the **source starter**, including
obsolete IDs; it is not a count of missing canonical translations. Direction
and keyboard defaults retain the source language's existing behavior. Each file
has firmware version, source commit, English-source and keyset hashes alongside
language metadata. All normalized files must pass device UTF-8, scalar, line,
source and compiled-cache limits before generation succeeds.

Public fetching becomes available only after this local plumbing is
integrated and the release/Pages workflows run; an older public catalog may
contain a different keyset from local development firmware.

For an exact local firmware snapshot, generate an offline fixture instead:

```sh
python3 scripts/generate_language_template.py --version local-test-languages \
  --source-commit "$(git rev-parse HEAD)" --output-dir /tmp/crossink-template
```

The default `status: "local-fixture"` distinguishes this output from live release
builds. Inky can load this JSON locally, display its source/version, and export a
translation without sending translation text to any server. Keep the chosen
contract while editing; explicitly load another version before migrating keys.
Metadata must remain ahead of strings. Inky export adds the firmware version,
source commit, English-source hash and keyset hash as informational metadata; unsupported
`STR_` entries should be rejected by the editor even though firmware ignores them.
All device scalar, UTF-8, format, source, line and cache limits still apply.
Translations do not add CJK glyphs, font coverage, keyboards or shaping support.

### Validation and timing

The installer uses less than 5 KiB of parser scratch. Catalog and parser heap
allocations are fallible and together stay below 8 KiB; the catalog is released
before installation. The normal lookup index uses two internal-RAM bytes per key.

The host suite exercises the production parser, cache, generated schema, flash
adapter, and I18n API. It covers restart/apply behavior, all starter languages,
missing SD sources, fallback, and stable string pointers:

```sh
cmake -G 'Unix Makefiles' -S test/language_cache -B /tmp/crossink-language-tests
cmake --build /tmp/crossink-language-tests -j2
ctest --test-dir /tmp/crossink-language-tests --output-on-failure
```

After building the simulator, exercise the real settings migration and picker:

```sh
python3 scripts/test_sd_language_boot.py --installer /tmp/crossink-language-tests/I18nIntegrationTool --ui
```

For hardware timing, build with `CROSSINK_LANGUAGE_BENCHMARK` and package with
`--benchmark`. Install **English benchmark**. Compare the same Settings/reader
menu interactions with built-in English, mapped flash, and the diagnostic PSRAM
backend on S3 (add `CROSSINK_LANGUAGE_BACKEND_PSRAM` for that comparison build).
Keep `CROSSINK_LANGUAGE_BENCHMARK` enabled so the fixture retains identical English
strings in its cache. Diagnostic allocation/timing is excluded from normal firmware.
Record warm and cold runs, internal free heap/largest block, and installation
and cached startup times separately. Rendering logs distinguish CPU preparation
from time spent in display calls. Target added p95 CPU rendering below 5 ms and
cached wake overhead below 20 ms. Host timings cannot establish these targets
on an ESP32 or a physical e-ink panel.

Test X3/X4, Sticky, and X4 Pro: change/reapply a language, remove its SD source,
wake repeatedly, exercise RTL controls, then perform OTA and SD firmware updates.
Check that the cached selection survives and both firmware-slot behaviors remain
intact. Interrupt installation only on a backed-up test device. No EPUB cache
reset is needed for this feature.

## Maintainer: configure S3 downloads

The asset publisher targets the dedicated `crossink-languages` bucket in
`us-east-1`. Its production prefix is `v1/`; manual test bundles use a separate
`testing/<firmware-commit>/` prefix and include their exact endpoint.

For a local firmware build, set `CROSSINK_ASSET_BASE_URL` to the bundle's HTTPS
base URL (with the final slash) before running the appropriate MCU-specific
PlatformIO wrapper. This explicitly compiles the endpoint into the binary; it
does not upload files or modify the bucket. Existing manual SD installation is
unaffected. Clear any older `ASSET_DOWNLOAD_BASE_URL` override before using this
variable. Build from the commit recorded by the bundle so the online translation
source and keyset match.

For example, in the dedicated C3 build worktree with matching source:

```sh
CROSSINK_ASSET_BASE_URL="https://crossink-languages.s3.us-east-1.amazonaws.com/testing/FULL_COMMIT/" \
  /Users/home/.local/bin/codex-pio-c3 run -e default -j1
```

The `Publish language download assets` workflow runs after a release is published,
not when a draft is created. The reusable `scripts/release_assets.py` and detailed
manual-upload/Actions setup instructions live in the shared asset repository's
`docs/s3-language-releases.md`. Configure `LANGUAGE_ASSETS_TOOLS_REF`,
`LANGUAGE_ASSETS_REVIEW_FILE`, `LANGUAGE_ASSETS_AWS_ROLE_ARN`, and (after endpoint
qualification) `CROSSINK_ASSET_BASE_URL` in the firmware repository. No AWS or
GitHub configuration is changed by committing the workflow.
