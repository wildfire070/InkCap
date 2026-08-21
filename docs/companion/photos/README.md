# Device photos

Real photos of the companion running on hardware, used by the top-level README.

| File | Shot | Status |
| --- | --- | --- |
| `home-screen.jpg` | Home screen with Sophocles and a speech bubble | ✅ |
| `character-vellum.jpg` | Same screen with Vellum, for contrast | ✅ |
| `neglected.jpg` | A Peckish or Neglected companion | ⬜ still wanted |
| `settings-picker.jpg` | The character picker in Settings | ⬜ still wanted |

The two missing shots have marked placeholders in the top-level README. Each sits next to a
commented-out `<img ...>` line: uncomment it and delete the placeholder blockquote above.

Earning a Neglected shot takes three days of not reading, so that one needs patience or a
temporarily lowered threshold in `lib/Companion/CompanionMood.h`.

## Before committing a photo

These are published on a public repository, so:

- **Strip the metadata.** Phone photos embed GPS coordinates. Saving through Pillow without
  passing `exif=` drops everything; the two files here were processed that way and carry no
  metadata at all.
- **Crop to the device** and resize to around 900px wide. The existing files are ~105 KB each;
  a raw phone photo is 1–5 MB and has no business in a firmware repository.

## Taking them

Landscape or portrait both work — portrait suits the X3's shape. Straight-on beats angled, and
a lamp *behind* you rather than overhead avoids the sheen e-ink picks up.
