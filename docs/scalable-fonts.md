# Scalable fonts on S3

ESP32-S3 devices use embedded static TrueType outlines for Bitter and Lexend
Deca, with regular, bold, italic, and bold-italic faces. Inter UI fonts remain
bitmaps. C3 builds retain the original bitmap reader fonts and `.cpfont` support.
EPUB parsing, pagination, and reindexing after layout changes are unchanged.

## Installing fonts

Copy static `.ttf` files to `/fonts` or a family folder inside it. The hidden
`/.fonts` root also works and takes precedence. Restart or refresh the font list
after changing files outside the font uploader. Choose the family in the existing
font picker; no desktop conversion, pre-generated sizes, or glyph indexing is
required. TTFs inside a family folder use that folder name in the picker, like `.cpfont`
families. Loose TTFs directly in either font root use their internal family name.
Style metadata still identifies regular, bold, italic, and bold-italic faces.
If you selected a folder-based TTF under its previous metadata name, select it
once again under its folder name after updating.
The uploader accepts TTF on supported devices and validates the completed file.

All existing `.cpfont` installs remain supported. A bitmap family takes precedence
if it shares a name with a TTF family; duplicate TTF styles are logged and skipped.
A lone style is usable as the regular fallback. Supported custom sizes are 8–22 pt
in the existing selector. Each TTF is limited to 2 MiB; a custom family has a total 6 MiB file-data budget, and
family names must fit the existing 63-byte persisted field. Larger files, variable
fonts, collections, and OTF/CFF files are not supported in this first version.
Unsupported files are skipped with a `TTF` diagnostic; upload errors are reported
in the browser. Temporary read/allocation failures preserve the saved selection. Family loading checks
the total before reading face data and requires at least 1 MiB of free PSRAM
headroom for other reader work at its allocation checks, including size-descriptor
costs. This is a headroom check, not a separately allocated reserve. Families use streaming when their complete bytes cannot fit. If even the
streaming workspaces cannot preserve headroom, loading fails with a memory
diagnostic; a total above the file-data budget has a separate diagnostic.

## Memory, rendering, and persistence

FreeInkFont lives in the SDK and exposes FreeType-native pixel metrics plus raw
8-bit coverage, including black/white coverage for monochrome output. Its per-face rendering API supports no hinting,
native TrueType hinting (interpreter 35 or 40), automatic hinting, light hinting,
monochrome output, outline weight, slant, and auto-hinter stem darkening. CrossInk
compiles these optional SDK modules. Select an installed TTF, then open
`Reader > Font Options > TTF Rendering` in global settings or `TTF Rendering`
from the in-reader Font menu to tune it. The editor is hidden for built-in fonts
and `.cpfont` bitmap families because those controls cannot affect them.

`HalScalableFont` owns storage, 150-DPI reader-size conversion, style mapping,
2-bit coverage conversion, glyph caching, and EpdFont adaptation. CrossInk persists
one profile per custom TTF family and applies it to every face in that family.
Changing a profile reopens the resident family, and the profile is included in
the font identity so affected EPUB layouts rebuild. Gamma, 2-bit thresholds,
letter/word/line spacing, kerning multipliers, and ligature choices stay in
CrossInk because they are renderer/layout policy rather than reusable outline
rasterization controls. The fixed 150-DPI conversion matches CrossInk's existing
`.cpfont` sizes, so the same point-size setting has comparable visual dimensions
for bitmap and scalable families on every device. The SDK accepts fractional
pixel sizes in FreeType's native 26.6 units and does not assume a display density.
The shared runtime reserves up to 1280 KiB for bounded FreeType parsing and scratch
(with 1024/768 KiB fallback arenas),
plus 512 KiB for glyph pixels and bounded 512-entry pixel, metric, and kerning
lookup tables. Reusing metrics avoids rerunning FreeType hinting for every
occurrence of the same character during EPUB layout. The
rasterizers' grayscale and monochrome 16 KiB scratch pools are allocated once
inside the FreeType workspace, rather than on the render-task stack. The
auto-hinter also keeps its reusable
glyph record and first-use Latin/CJK metric scratch out of that task stack. Each
open face uses bounded size/metric descriptors in PSRAM. Embedded font bytes stay
in flash. Custom faces are loaded in regular/bold/italic/bold-italic order and use PSRAM
individually when they fit, preserving 1 MiB of reader headroom plus descriptors
and streaming buffers for pending faces. Remaining faces stream from one open SD
handle each through four 1 KiB read windows. Reader faces also retain up to 1 MiB
of font-file prefix in PSRAM when the reader reserve and pending faces still fit;
the prefix is filled during the existing content-hash scan and is optional.
Per-face admission keeps the regular face resident
when the entire family cannot fit. Bytes and faces are shared by all selected sizes.
Selection validates a regular glyph; other styles initialize their auto-hinting
only when used. Streamed GSUB copies are capped at 48 KiB and released after
ligature resolution; streamed GPOS copies remain disabled. Metadata discovery
reads through a bounded SDK stream into its separate parser workspace, avoiding
another full-file copy while a large reader family is resident. Size changes
within a family reuse faces and caches. Family changes, dictionary replacement,
and network handoff release custom file data through SdCardFontManager.

Font mutation and measurement borrow the ActivityManager render mutex when
already held, or acquire it for their scope. Width queries do not render glyphs.
Rasterized glyphs are reused across draws and
AA passes until eviction, using the default `.cpfont` grayscale thresholds.
Automatic hinting remains CrossInk's default; physical e-ink
acceptance must check stem weight and small-size readability. There is no extra
framebuffer.

Custom cache identity includes all selected font bytes, style order, size,
rendering profile, and residency mode (streaming omits GPOS kerning). Built-in identity includes outline asset content and backend
revision. Old layout caches therefore rebuild when affected, without
changing binary section-cache layouts or deleting bookmarks/clippings. A failed
built-in allocation uses the UI recovery font and its separate cache identity.

Dictionary definitions reuse the active reader family when it matches. A
different TTF dictionary family skips the full-file content hash and checksum
scan. Faces up to 256 KiB remain resident when existing heap reserves allow it,
preserving sequential reads for small fonts (at most 1 MiB across four styles).
Larger faces stream only the tables and glyphs needed for the definition.
Its identity is unique to that open and must never enter persistent book
layouts. Returning to the reader reloads its normal content-based identity,
including when the reader requests the same family and size. Only one custom
family remains active. For streamed faces, existing typography rules apply (no GPOS
kerning and a bounded GSUB copy); file I/O, parser, and regular-glyph probe
failures still trigger fallback, but there is no whole-file checksum warning
for a temporary dictionary load.

## Rebuilding assets

Run `lib/EpdFont/scripts/build-scalable-fonts.py --source <source-font-directory>
--output lib/EpdFont/scalableFonts` with fonttools installed. It subsets all current
bitmap codepoint ranges, appends missing fallback outlines at the correct em
scale, retains layout tables, and verifies coverage. Source families are Bitter,
LexendDeca, ChareInk7, NotoSymbols, and NotoSansCJKsc. Derived TTF files and their
licenses are checked in. `scripts/build_scalable_font_assets.py` embeds these as
an ignored generated header during builds; never edit that header.
