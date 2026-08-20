# CrossPoint Reader — Companion

A reading companion for [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader), the
open-source e-reader firmware. Pick a small pixel character; it lives on your home screen and its
mood depends on how much you actually read.

![The five companions, all thriving](docs/companion/roster-thriving.png)

Everything CrossPoint already does is untouched. The companion is **off by default** and adds
around **16 KB of flash and 80 bytes of RAM** when you turn it on.

> ### ⚠️ Device support
>
> **Developed and tested only on the Xteink X3.** That is the only device this has actually run on.
>
> | Device | Status |
> | --- | --- |
> | **Xteink X3** | ✅ Developed and tested here |
> | **Xteink X4** | ⚠️ Untested. Should run — same chip, and the release binary includes it — but the X4 has **no RTC**, so streaks and day-based decay are unavailable (see below) |
> | **Xteink X4 Pro** | ❌ **Do not flash the release binary** — the X4 Pro is ESP32-**S3** and X3/X4 are ESP32-**C3**, so it will not boot. Needs its own `x4pro` build, which has **not been verified to compile** here (see [Building from source](#building-from-source)) |
> | Anything else | Untested |
>
> **Back up your current firmware before flashing.** Instructions are in
> [Install](#install) below. It takes two minutes and means you can always get back to exactly
> what you have now.

<p align="center">
  <img src="docs/companion/photos/home-screen.jpg" width="380"
       alt="An Xteink X3 showing the home screen, with Sophocles the pixel fox and a speech bubble reading 'Fine. This is fine. I am a fine boy.'">
</p>

<p align="center">
  <em>Sophocles on an Xteink X3, twelve minutes short of Thriving.</em>
</p>

---

## What it does

Read regularly and your companion thrives. Ignore it and it sulks, wilts, or powers down.

- **Five characters** to choose from, each with their own artwork and voice
- **Four moods**, driven by how much you read and how many days you skip
- **180 written lines** — the character comments on how you are doing, and the line changes
  every time you land on the home screen
- **It paces around** while you move the menu cursor, and turns to face the way it is walking
- **A streak counter**, and a nudge telling you how far you are from the top mood today

---

## The companions

Left to right in each strip: **Thriving · Content · Peckish · Neglected**.

### Sophocles — the fox
House Telemanus's sigil fox from *Red Rising*. Aristocratic, judgemental, and permanently
convinced you are hiding jellybeans.

![Sophocles in four moods](docs/companion/sophocles.png)

> *"You smell faintly of jellybeans. Approved."*
> *"I have seen dead men read more than this."*

### Vellum — the ghost
A page-spirit that only stays solid while somebody is still reading. When neglected it literally
fades out — the same artwork drawn at a quarter density instead of a solid fill.

![Vellum in four moods](docs/companion/vellum.png)

> *"I am not haunting you. I am supervising."*
> *"Bloody hell, I can see through my own hands."*

### Octavo — the robot
A page-counting unit that runs on finished chapters instead of batteries. Swears exclusively in
error codes.

![Octavo in four moods](docs/companion/octavo.png)

> *"SIGNAL STRONG. READING QUOTA EXCEEDED."*
> *"FATAL: give a shit exception. Core dumped."*

### Lumen — the moth
A paper moth that navigates by the light of whatever you are reading. Wings spread when the
reading is good, shut tight when it is not.

![Lumen in four moods](docs/companion/lumen.png)

> *"Drunk on lumens. Absolutely hammered."*
> *"I have started eating the curtains. Your fault."*

### Sprig — the sprout
A potted thing that grows a leaf per finished chapter. Passive-aggressive about drought.

![Sprig in four moods](docs/companion/sprig.png)

> *"I am the best damn plant on this device."*
> *"I am ninety percent stick at this point."*

<p align="center">
  <img src="docs/companion/photos/character-vellum.jpg" width="380"
       alt="The same home screen with Vellum the pixel ghost, saying 'A quiet day. I approve of quiet.'">
</p>

<p align="center">
  <em>Same screen, different tenant. Switching character takes two button presses.</em>
</p>

---

## How moods work

| Mood | How you get there |
| --- | --- |
| **Thriving** | 25+ credited minutes today |
| **Content** | 2+ minutes today, **or** you read yesterday |
| **Peckish** | Exactly two quiet days — you skipped one full day |
| **Neglected** | Three or more quiet days |

Two details matter:

**"Credited" minutes are real reading.** Time only accrues while a page turned within the last
five minutes. A reader left open on one page earns nothing. Page-flipping without reading accrues
time but still has to clear a real threshold, so it will not jump you a tier.

**Days are your days.** The rollover uses your device's clock in your own timezone, so a
late-evening session does not land on tomorrow and break a streak.

There is no death state. However long you neglect it, one good session brings it straight back.

### Devices without a clock

Day counting needs a real-time clock. The **X3** and **X4 Pro** have one; the **X4 does not**.

Without a clock, elapsed days are genuinely unknowable, so rather than guess, day-based decay
switches off entirely: the mood reflects only the current reading session and never drops below
Content. In practice that means **no streaks, and no Peckish or Neglected** — you get Content and
Thriving.

The clock also needs setting once, over Wi-Fi, before day counting works. Until then the same
fallback applies.

<!-- A photo of a Peckish or Neglected companion would sit well here.
     Earning one takes three quiet days. See docs/companion/photos/README.md -->

---

## Install

The release binary is for **X3 and X4 only** (both ESP32-C3). **X4 Pro owners must build from
source** — a C3 binary will not boot on its S3 chip.

### Step 1 — back up your current firmware (please do this)

Flashing replaces what is on the device. A full backup lets you return to exactly your current
setup, Wi-Fi credentials and settings included, rather than merely a clean reinstall.

```bash
pip install esptool
esptool --chip esp32c3 --port /dev/cu.usbmodemXXXX --baud 921600 \
  read-flash 0 0x1000000 crosspoint-backup.bin
```

Find your port with `ls /dev/cu.*` on macOS, or `dmesg | grep tty` on Linux. The file should come
out at exactly 16,777,216 bytes. Keep a copy somewhere other than the machine you flash from.

Restore any time with:

```bash
esptool --chip esp32c3 --port /dev/cu.usbmodemXXXX --baud 921600 \
  write-flash 0 crosspoint-backup.bin
```

If you skip the backup you can still recover — the web flasher below will put an official
CrossPoint build back on — but you will lose your settings and saved networks.

### Step 2 — flash

**No toolchain needed:**

1. Download `firmware.bin` from the [latest release](https://github.com/JoshuaMillerCode/crosspoint-reader-companion/releases/latest)
2. Connect your device by USB-C and wake it
3. Go to [crosspointreader.com/#flash-tools](https://crosspointreader.com/#flash-tools)
4. Pick your device (**X3** or **X4**), choose **Custom .bin**, and upload the file

**Or from a terminal:**

```bash
esptool --chip esp32c3 --port /dev/cu.usbmodemXXXX --baud 921600 \
  write-flash 0x10000 firmware.bin
```

Your books and reading progress live on the SD card and are not touched by flashing.

> **Note on locked devices.** Some units bought from third-party sellers ship with USB flashing
> locked. If the browser's serial picker cannot see your device, check
> [CrossPoint's notes on the Xteink Unlocker](https://github.com/crosspoint-reader/crosspoint-reader#usb-locked-devices-xteink-unlocker)
> before going further.

---

## Turning it on

**Settings → Companion** to enable, then **Character** to choose who lives on your home screen.
**Show on Home** controls whether it is drawn.

Nothing changes about the reader until you enable it.

<!-- A photo of the character picker would sit well here.
     See docs/companion/photos/README.md -->

---

## Building from source

```bash
git clone --recursive https://github.com/JoshuaMillerCode/crosspoint-reader-companion
cd crosspoint-reader-companion
pio run -e default            # build
pio run -e default -t upload  # build and flash
```

Pick the environment for your device:

| Device | Environment | Chip |
| --- | --- | --- |
| X3 / X4 | `default` or `gh_release` | ESP32-C3 |
| **X4 Pro** | **`x4pro`** | ESP32-**S3** |

```bash
pio run -e x4pro -t upload    # X4 Pro
```

The chip differs, so the binaries are not interchangeable.

**On the X4 Pro specifically:** the `x4pro` environment has not been built successfully here. The
attempt failed while ESP-IDF configured its S3 toolchain, before reaching any companion code, so
whether this compiles for S3 is simply unknown rather than known-broken. Nothing in the companion
is chip-specific — no assembly, no C3 registers, and all layout comes from runtime screen
dimensions — so there is reason to expect it works. But nobody has demonstrated it.

If you have an X4 Pro and get it building, please open an issue. That, and a photo, is all it
would take to move it into the tested column.

Needs [pioarduino](https://github.com/pioarduino/pioarduino) and Python 3.8+. The `--recursive`
matters: the FreeInk SDK is a submodule. If you forget it, run
`git submodule update --init --recursive`.

Host-side unit tests, no hardware required:

```bash
cmake -S test -B build/test && cmake --build build/test
ctest --test-dir build/test
```

---

## Making your own character

Each companion is a single plain-text file in [`src/companion/sprites/`](src/companion/sprites/)
holding both its artwork and its dialogue. No binary assets, no image editor.

```
name: Sophocles
kind: fox

[thriving]
..............##..........##......
.............#ww#........#ww#.....
...  30 rows of 34 characters  ...

[quotes.thriving]
Tail up. Very pleased with your page count.
You smell faintly of jellybeans. Approved.
```

| Character | Meaning |
| --- | --- |
| `.` | transparent |
| `#` | ink — outline, eyes, nose |
| `w` | paper — chest, muzzle, inner ears, tail tip |
| `o` | body fill, drawn as a 50% checkerboard |
| `d` | faded fill, drawn at 25% — used for ghosts and dead batteries |

Add a `.grid` file, list it in [`order.txt`](src/companion/sprites/order.txt), and rebuild. It
appears in the settings picker automatically. The build step packs the art into flash and will
**fail the build** with a `file:line` error if a row is the wrong width or a line is too long for
the speech bubble.

Because the panel is 1-bit, there is no colour: the dither densities are what give the artwork
its shading, and they are resolved at build time so the render path stays trivial.

---

## How it is built

- **Sprites** are 34×30 hand-placed pixels, packed 1 bit per pixel. All twenty poses total
  3,000 bytes.
- **Mood logic** lives in [`lib/Companion/`](lib/Companion/) as pure functions with no Arduino,
  FreeRTOS, or HAL dependencies, so the whole decay and crediting policy is covered by host unit
  tests before it reaches a device.
- **The walk cycle is driven by screen redraws, not a timer.** On e-ink a timer would keep the
  panel refreshing, block the low-power idle, and accumulate ghosting. Moving only when the screen
  was already repainting makes the animation free.
- **The speech bubble** is drawn with a midpoint-circle algorithm for its rounded corners and a
  half-plane test for the tail, since `GfxRenderer` has no rounded-rect primitive.
- **State** persists to `/.crosspoint/companion.json`, checkpointed mid-session so a flat battery
  does not discard an evening's reading.

---

## This is version 1 — tell me what is wrong with it

It works, it has been used on a real device, and it is also the first version of something that
was built in a few days. There will be rough edges I have not hit yet, and choices that seemed
sensible to me and will annoy you.

**Please open an issue.** Anything is fair game:

- **Bugs** — [report one](https://github.com/JoshuaMillerCode/crosspoint-reader-companion/issues/new?template=bug_report.yml).
  Reports from an **X4** or **X4 Pro** are especially valuable, since neither has ever run this
- **Ideas and requests** — [suggest one](https://github.com/JoshuaMillerCode/crosspoint-reader-companion/issues/new?template=feature_request.yml).
  New characters, better lines, different thresholds, a different place on screen
- **Lines that are not funny enough.** Genuinely. The writing is half of this, and it is easy to
  change — each character's dialogue is plain text sitting next to its artwork

Things I already know are unfinished: there is no stats screen, the status bar has no companion
glyph, and the milestone line for beating your best streak has never been seen by anyone because
it takes days of reading to earn.

If you build a character of your own, please send a pull request. One text file is the whole
contribution.

## Scope

This is a fork, not a proposed upstream feature. CrossPoint's [SCOPE.md](SCOPE.md) explicitly
puts interactive extras out of scope for the core project, and that is the right call for a
firmware whose job is to disappear while you read. That is exactly what forks are for.

Everything else in this repository is CrossPoint, tracking upstream `develop`.

---

## Credits

- **[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)** — the firmware
  this builds on, and a genuinely pleasant codebase to work in
- **Sophocles** is inspired by House Telemanus's sigil fox from Pierce Brown's *Red Rising*
  series. No official artwork exists; this design is original. The other four characters are
  original creations
- Pixel technique follows [Derek Yu's tutorial](https://www.derekyu.com/makegames/pixelart.html)
  and standard [1-bit dithering practice](https://pixelparmesan.com/blog/dithering-for-pixel-artists)
- Companion mod by [@JoshuaMillerCode](https://github.com/JoshuaMillerCode)

MIT licensed, same as CrossPoint. See [LICENSE](LICENSE).

Not affiliated with Xteink, CrossPoint, or Pierce Brown.
