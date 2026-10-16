---
title: SD Card Fonts
nav_order: 4
---

# SD Card Fonts

CrossInk can load extra fonts from the SD card, including fonts that cover other
writing systems (Chinese/Japanese/Korean, Cyrillic, Greek, and more).

- **Every reader** can use `.cpfont` font packs. These are fonts converted for
  CrossInk, with one file per size.
- **X4 Classic, X4 Pro, and Sticky** can also use ordinary static TrueType
  (`.ttf`) and OpenType (`.otf`) font files directly, in any size. The original
  X3 and X4 need `.cpfont` files.

## Available Pre-Built Fonts

You can view pre-built fonts available for download at [Inky](https://inky.crossink.dev/#downloads).

## Converting Custom Fonts with CrossPoint's Font Builder

To make `.cpfont` packs from your own TrueType/OpenType fonts, use CrossPoint's
[Font Builder](https://crosspointreader.com/fonts). On X4 Classic, X4 Pro, and
Sticky, static `.ttf` and `.otf` files do not need conversion.

## Installing Fonts

There are three ways to install fonts:

### Option 1: Manual SD card copy (Fastest)

1.  Download pre-built `.cpfont` files from [Inky](https://inky.crossink.dev/#downloads),
    or use your own static `.ttf` or `.otf` files on an X4 Classic, X4 Pro, or Sticky.
    - If you downloaded a `.zip`, extract it first.
2.  Copy font family folders to one of two locations on your SD card:
    - `/.fonts/` - hidden folder (preferred; keeps the SD card tidy when
      hidden files are not shown on your device)
    - `/fonts/` - visible folder (use this if your computer hides folders that
      start with a dot and you would rather see it in your file manager)

    Both folders are checked when the reader starts and the results are
    combined: a family installed in `/fonts/` shows up even when `/.fonts/` also
    exists, and the other way round. If the same family name appears in both,
    the copy in `/.fonts/` wins and the duplicate in `/fonts/` is ignored. Put
    each font family in its own folder, including `.ttf` and `.otf` families.
    Do not mix `.cpfont` with `.ttf` or `.otf` files in one family folder.

        SD Card Root/
        ├── .fonts/                     <- Hidden folder (preferred)
        │   └── Literata/
        │       ├── Literata_12.cpfont
        │       ├── Literata_14.cpfont
        │       ├── Literata_16.cpfont
        │       └── Literata_18.cpfont
        └── fonts/                      <- Visible folder (equally valid)
            └── Merriweather/
                ├── Merriweather_12.cpfont
                └── ...

    On an X4 Classic, X4 Pro, or Sticky, a `.ttf` or `.otf` family can look like
    `/fonts/MyFont/MyFont-Regular.ttf` alongside its bold, italic, and bold
    italic files.

3.  Insert the SD card and power on your CrossInk device. If it was already on,
    restart it so the new fonts are found.

### Option 2: Download from device

This option downloads pre-built `.cpfont` packs straight to the reader.

1. Go to **Settings > Reader > Font Options > Download Fonts**
2. Connect to Wi-Fi if asked
3. Browse the available font families and select one to download
4. Downloaded fonts appear in **Settings > Reader > Font Options > Font Family**

> [!NOTE]
> To change which font sizes are downloaded, change **Download Font Size Range**
> (in the same **Font Options** menu) _before_ downloading.

### Option 3: Upload via web browser

1. Start **File Transfer** and connect through **Join a Network** or **Create Hotspot**
2. Open the web address shown on the reader (see the [Web Server Guide](./webserver.md))
3. Open the **Fonts** tab
4. Select a folder containing one font family, then upload its `.cpfont` files
   or, on an X4 Classic, X4 Pro, or Sticky, its static `.ttf` or `.otf` files. The
   page only offers `.ttf` and `.otf` uploads on readers that support them.

For the full range of text styles with a `.ttf` or `.otf` family, include regular,
bold, italic, and bold italic files. Variable fonts are not supported. Each file
must be 2 MiB or smaller, and the family must total 6 MiB or less. See
[Scalable Fonts](./scalable-fonts.md) for more about using them.

## Dictionary Fonts

EPUB books can use a different installed SD-card family for dictionary definitions.
Set it for all books in **Settings > Reader > Font Options > Dictionary Font**, or for
one book from the reader menu's font options. If a saved point size is no longer
available, CrossInk chooses the closest size from the dictionary family. When the
reader is low on memory, the dictionary may show in your reader font instead. This
is normal. See [Dictionary](./dictionary.md) for more.

### Generating dictionary font families (Advanced)

Use the dictionary-specific builder to generate the complete family catalog with
the extra coverage used by dictionary definitions:

    python3 -m pip install -r lib/EpdFont/scripts/requirements.txt
    python3 lib/EpdFont/scripts/build-dictionary-fonts.py \
      --output-dir ./generated-dictionary-fonts \
      --clean \
      --jobs 2

The dictionary build includes the `reading` ranges and the built-in ranges, plus
IPA and phonetic-extension characters (`U+0250–U+02FF` and `U+1D00–U+1DBF`) and
combining-mark ranges (`U+1DC0–U+1DFF`, `U+20D0–U+20FF`, and
`U+FE20–U+FE2F`).

The default output is `../crossink-fonts/dictionary-fonts`. Use a separate
`--output-dir` for personal builds, because `--clean` removes the selected output
directory before generating the fonts. The output contains family folders and ZIP
archives; copy a family folder or unzip its archive into `/.fonts/` or `/fonts/`
on the SD card. Use `--only FamilyA,FamilyB` to generate selected families.

## Converting Custom Fonts with Python (Advanced)

This section is for people comfortable with the command line. The steps below
create `.cpfont` packs. X4 Classic, X4 Pro, and Sticky can use static `.ttf` or
`.otf` files directly instead.

### Prerequisites

    pip install freetype-py fonttools

### Single font (one style)

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyFont-Regular.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --style regular \
      --name MyFont \
      --output-dir ./MyFont/

### Multi-style font

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      --regular MyFont-Regular.ttf \
      --bold MyFont-Bold.ttf \
      --italic MyFont-Italic.ttf \
      --bolditalic MyFont-BoldItalic.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --name MyFont \
      --output-dir ./MyFont/

### Available Unicode interval presets

| Preset        | Coverage                                                                                                             |
| ------------- | -------------------------------------------------------------------------------------------------------------------- |
| `ascii`       | U+0020–U+007E (Basic Latin)                                                                                          |
| `latin1`      | U+0080–U+00FF (Latin-1 Supplement)                                                                                   |
| `latin-ext`   | European languages (Latin + Extended-A/B + punctuation + ligatures)                                                  |
| `greek`       | Greek + Extended Greek                                                                                               |
| `cyrillic`    | Cyrillic + Supplement                                                                                                |
| `hebrew`      | Hebrew + Alphabetic Presentation Forms                                                                               |
| `georgian`    | Georgian + Georgian Supplement                                                                                       |
| `armenian`    | Armenian                                                                                                             |
| `ethiopic`    | Ethiopic + Extended                                                                                                  |
| `vietnamese`  | Vietnamese subset (ơ/ư and combining marks)                                                                          |
| `punctuation` | General punctuation (U+2000–U+206F)                                                                                  |
| `cjk`         | CJK Unified Ideographs + Hiragana + Katakana + Fullwidth                                                             |
| `hangul`      | Korean Hangul syllables + Jamo + Compatibility Jamo                                                                  |
| `cherokee`    | Cherokee (historic + supplement block)                                                                               |
| `tifinagh`    | Tifinagh                                                                                                             |
| `symbols`     | Math, currency, arrows, box-drawing, misc symbols, dingbats                                                          |
| `reading`     | Literary fiction coverage: Latin, Greek, Cyrillic, math/symbol blocks, supplemental punctuation, and CJK quote marks |
| `builtin`     | Matches the firmware's built-in font conversion intervals                                                            |

Combine presets with commas: `--intervals latin-ext,greek,cyrillic`

You can also specify arbitrary Unicode ranges directly:
`--intervals latin-ext,(0x2100-0x214F)`

To list all presets with codepoint counts:

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py --list-presets

### Additional options

`--force-autohint`: force FreeType's auto-hinter instead of the font's native hinting (useful when a font's built-in hints produce poor results at small sizes).

Install custom fonts via the web interface or manual SD card copy.
