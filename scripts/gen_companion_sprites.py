#!/usr/bin/env python3
"""
Generate the companion sprite table from editable character-grid sources.

Each companion lives in one .grid file under src/companion/sprites/. The art is
plain text so it can be edited by hand (or diffed in a PR) without a binary
asset pipeline:

    name: Sophocles
    kind: fox

    [thriving]
    ..............##..........##......
    ... 30 rows of 34 characters ...

    [content]
    ...

Cell characters:
    .  transparent      #  ink (outline, eyes, nose)
    w  paper (explicit white marking -- chest, muzzle, tail tip)
    o  body fill, rendered as a 50% checkerboard dither
    d  faded fill, rendered as a 25% dither (ghost fading, robot powered down)

Dither is resolved here rather than on device: the panel is 1-bit, so a
checkerboard baked into the bitmap costs nothing extra to store and removes all
per-pixel pattern logic from the render path.

Output is src/companion/CompanionSprites.generated.h -- packed 1bpp, MSB-first,
bit set = ink. Never edit the generated header; edit the .grid files.

Usage:
    python gen_companion_sprites.py [sprites_dir [output_dir]]
"""

import argparse
import os
import sys

WIDTH = 34
HEIGHT = 30
MOODS = ["thriving", "content", "peckish", "neglected"]
# Quote sections that are not moods. "milestone" fires once when the reader
# beats their own best streak, so it needs its own voice rather than reusing a
# mood line.
EXTRA_QUOTES = ["milestone"]
VALID_CELLS = set(".#wod")
# Speech-bubble copy is wrapped to two lines on a 480px-wide panel; beyond this
# it overflows the bubble, so the generator rejects it rather than letting it
# clip on device.
MAX_QUOTE_CHARS = 64


def dither_ink(cell, x, y):
    """Resolve one cell to a bit. Dither phase is keyed off absolute grid
    position so adjacent fills tile seamlessly instead of seaming at borders."""
    if cell == "#":
        return True
    if cell == "o":
        return (x + y) % 2 == 0
    if cell == "d":
        return x % 2 == 0 and y % 2 == 0
    return False  # '.' transparent and 'w' paper both leave the pixel unlit


def parse_grid_file(path):
    """Returns (name, kind, {mood: [rows]}, {mood: [quotes]}).

    Sections are [<mood>] for art and [quotes.<mood>] for the character's
    speech-bubble lines. Raises ValueError on bad input."""
    name = None
    kind = None
    poses = {}
    quotes = {m: [] for m in MOODS + EXTRA_QUOTES}
    current = None
    current_is_quotes = False

    with open(path, "r", encoding="utf-8") as handle:
        for lineno, raw in enumerate(handle, 1):
            line = raw.rstrip("\n").rstrip("\r")
            stripped = line.strip()

            if not stripped or stripped.startswith("//"):
                continue

            if stripped.startswith("[") and stripped.endswith("]"):
                section = stripped[1:-1].strip().lower()
                current_is_quotes = section.startswith("quotes.")
                current = section[len("quotes.") :] if current_is_quotes else section
                allowed = MOODS + EXTRA_QUOTES if current_is_quotes else MOODS
                if current not in allowed:
                    raise ValueError(
                        f"{path}:{lineno}: unknown section '{current}' (expected one of {', '.join(allowed)})"
                    )
                if current_is_quotes:
                    if quotes[current]:
                        raise ValueError(f"{path}:{lineno}: quotes for '{current}' declared twice")
                else:
                    if current in poses:
                        raise ValueError(f"{path}:{lineno}: mood '{current}' declared twice")
                    poses[current] = []
                continue

            if current is None:
                if ":" not in stripped:
                    raise ValueError(f"{path}:{lineno}: expected 'key: value' before the first section")
                key, value = stripped.split(":", 1)
                key = key.strip().lower()
                value = value.strip()
                if key == "name":
                    name = value
                elif key == "kind":
                    kind = value
                else:
                    raise ValueError(f"{path}:{lineno}: unknown header key '{key}'")
                continue

            if current_is_quotes:
                if '"' in stripped or "\\" in stripped:
                    raise ValueError(f"{path}:{lineno}: quotes cannot contain \" or \\ (they are emitted as C strings)")
                if len(stripped) > MAX_QUOTE_CHARS:
                    raise ValueError(
                        f"{path}:{lineno}: quote is {len(stripped)} chars, max {MAX_QUOTE_CHARS} "
                        f"(longer lines will not fit the speech bubble)"
                    )
                quotes[current].append(stripped)
                continue

            # Inside an art block: this is a row. Use the unstripped line so
            # leading transparent cells are preserved.
            row = line
            if len(row) != WIDTH:
                raise ValueError(f"{path}:{lineno}: row is {len(row)} cells, expected {WIDTH}")
            bad = set(row) - VALID_CELLS
            if bad:
                raise ValueError(f"{path}:{lineno}: invalid cell character(s) {sorted(bad)}")
            if len(poses[current]) >= HEIGHT:
                raise ValueError(f"{path}:{lineno}: more than {HEIGHT} rows in [{current}]")
            poses[current].append(row)

    if not name:
        raise ValueError(f"{path}: missing 'name:' header")
    if not kind:
        raise ValueError(f"{path}: missing 'kind:' header")

    missing = [m for m in MOODS if m not in poses]
    if missing:
        raise ValueError(f"{path}: missing mood block(s): {', '.join(missing)}")
    for mood in MOODS:
        if len(poses[mood]) != HEIGHT:
            raise ValueError(f"{path}: [{mood}] has {len(poses[mood])} rows, expected {HEIGHT}")
        if not quotes[mood]:
            raise ValueError(f"{path}: no [quotes.{mood}] lines; every mood needs at least one")
    for extra in EXTRA_QUOTES:
        if not quotes[extra]:
            raise ValueError(f"{path}: no [quotes.{extra}] lines; at least one is required")

    return name, kind, poses, quotes


def pack_pose(rows):
    """Pack one 34x30 grid into MSB-first rows, 5 bytes per row."""
    row_bytes = (WIDTH + 7) // 8
    out = bytearray()
    for y, row in enumerate(rows):
        packed = bytearray(row_bytes)
        for x, cell in enumerate(row):
            if dither_ink(cell, x, y):
                packed[x >> 3] |= 0x80 >> (x & 7)
        out.extend(packed)
    return bytes(out)


def to_identifier(name):
    return "".join(ch for ch in name if ch.isalnum())


def render_header(companions, row_bytes, sprite_bytes):
    lines = []
    add = lines.append
    max_quotes = max(len(q[mood]) for _n, _k, _p, q in companions for mood in MOODS)

    add("#pragma once")
    add("")
    add("#include <cstdint>")
    add("")
    add("// GENERATED by scripts/gen_companion_sprites.py from src/companion/sprites/*.grid")
    add("// Do not edit: regenerate instead. Dither patterns are already baked into the bits.")
    add("")
    add("namespace companion {")
    add("")
    add(f"inline constexpr int SPRITE_WIDTH = {WIDTH};")
    add(f"inline constexpr int SPRITE_HEIGHT = {HEIGHT};")
    add(f"inline constexpr int SPRITE_ROW_BYTES = {row_bytes};")
    add(f"inline constexpr int SPRITE_BYTES = {sprite_bytes};")
    add(f"inline constexpr int MOOD_COUNT = {len(MOODS)};")
    add(f"inline constexpr int COMPANION_COUNT = {len(companions)};")
    add("")
    add("// Order matches CompanionSprites below and the persisted companionId,")
    add("// so appending a companion is backwards compatible but reordering is not.")
    add("enum class CompanionId : uint8_t {")
    for name, _kind, _poses, _quotes in companions:
        add(f"  {to_identifier(name)},")
    add("};")
    add("")
    add("// [companion][mood][byte] -- 1bpp, MSB-first within each row, set bit = ink.")
    add("inline constexpr uint8_t COMPANION_SPRITES[COMPANION_COUNT][MOOD_COUNT][SPRITE_BYTES] = {")
    for name, kind, poses, _quotes in companions:
        add(f"    // {name} ({kind})")
        add("    {")
        for mood in MOODS:
            packed = pack_pose(poses[mood])
            add(f"        // {mood}")
            add("        {")
            for offset in range(0, len(packed), 15):
                chunk = packed[offset : offset + 15]
                add("            " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
            add("        },")
        add("    },")
    add("};")
    add("")
    add("// ASCII display names for the settings picker. Not translated: they are")
    add("// character names, the same in every UI language.")
    add("inline constexpr const char* COMPANION_NAMES[COMPANION_COUNT] = {")
    for name, _kind, _poses, _quotes in companions:
        add(f'    "{name}",')
    add("};")
    add("")
    add("// One-word species label, shown beside the name in the settings picker so")
    add("// the choice is meaningful before it is made.")
    add("inline constexpr const char* COMPANION_KINDS[COMPANION_COUNT] = {")
    for _n, kind, _p, _q in companions:
        add(f'    "{kind}",')
    add("};")
    add("")
    add(f"inline constexpr int MAX_QUOTES_PER_MOOD = {max_quotes};")
    add("")
    add("// Speech-bubble lines, per companion and mood. Deliberately outside the")
    add("// i18n string table: this is character voice, not UI chrome, and 32")
    add("// translations of every joke is not a burden worth putting on translators.")
    add("// Unused slots are nullptr; consult COMPANION_QUOTE_COUNTS for the real length.")
    add("inline constexpr const char* COMPANION_QUOTES[COMPANION_COUNT][MOOD_COUNT][MAX_QUOTES_PER_MOOD] = {")
    for name, _kind, _poses, quotes in companions:
        add(f"    // {name}")
        add("    {")
        for mood in MOODS:
            add(f"        // {mood}")
            add("        {")
            for quote in quotes[mood]:
                add(f'            "{quote}",')
            for _ in range(max_quotes - len(quotes[mood])):
                add("            nullptr,")
            add("        },")
        add("    },")
    add("};")
    add("")
    add("inline constexpr uint8_t COMPANION_QUOTE_COUNTS[COMPANION_COUNT][MOOD_COUNT] = {")
    for name, _kind, _poses, quotes in companions:
        counts = ", ".join(str(len(quotes[mood])) for mood in MOODS)
        add(f"    {{{counts}}},  // {name}")
    add("};")
    add("")
    max_ms = max(len(q["milestone"]) for _n, _k, _p, q in companions)
    add(f"inline constexpr int MAX_MILESTONE_QUOTES = {max_ms};")
    add("")
    add("// Said once when the reader beats their own best streak.")
    add("inline constexpr const char* COMPANION_MILESTONE_QUOTES[COMPANION_COUNT][MAX_MILESTONE_QUOTES] = {")
    for name, _kind, _poses, quotes in companions:
        add("    {")
        for quote in quotes["milestone"]:
            add(f'        "{quote}",')
        for _ in range(max_ms - len(quotes["milestone"])):
            add("        nullptr,")
        add(f"    }},  // {name}")
    add("};")
    add("")
    add("inline constexpr uint8_t COMPANION_MILESTONE_COUNTS[COMPANION_COUNT] = {")
    for name, _kind, _poses, quotes in companions:
        add(f'    {len(quotes["milestone"])},  // {name}')
    add("};")
    add("")
    add("}  // namespace companion")
    add("")
    return "\n".join(lines)


def main(sprites_dir=None, output_dir=None, verbose=False):
    # Defaults are relative to the project root, which PlatformIO makes the
    # working directory for pre: scripts (same convention as gen_i18n.py).
    # __file__ is not defined when SCons execs this, so it must not be used.
    sprites_dir = sprites_dir or os.path.join("src", "companion", "sprites")
    output_dir = output_dir or os.path.join("src", "companion")

    if not os.path.isdir(sprites_dir):
        print(f"gen_companion_sprites: no sprite directory at {sprites_dir}", file=sys.stderr)
        return 1

    # order.txt fixes enum order so persisted ids stay stable across filesystems
    # that would otherwise hand back a different readdir order.
    order_path = os.path.join(sprites_dir, "order.txt")
    if os.path.isfile(order_path):
        with open(order_path, "r", encoding="utf-8") as handle:
            stems = [ln.strip() for ln in handle if ln.strip() and not ln.startswith("#")]
    else:
        stems = sorted(
            os.path.splitext(f)[0] for f in os.listdir(sprites_dir) if f.endswith(".grid")
        )

    companions = []
    for stem in stems:
        path = os.path.join(sprites_dir, f"{stem}.grid")
        if not os.path.isfile(path):
            print(f"gen_companion_sprites: missing {path}", file=sys.stderr)
            return 1
        try:
            companions.append(parse_grid_file(path))
        except ValueError as exc:
            print(f"gen_companion_sprites: {exc}", file=sys.stderr)
            return 1

    if not companions:
        print("gen_companion_sprites: no .grid files found", file=sys.stderr)
        return 1

    row_bytes = (WIDTH + 7) // 8
    sprite_bytes = row_bytes * HEIGHT
    header = render_header(companions, row_bytes, sprite_bytes)

    out_path = os.path.join(output_dir, "CompanionSprites.generated.h")
    os.makedirs(output_dir, exist_ok=True)

    # Skip the write when nothing changed so PlatformIO does not rebuild every
    # translation unit that includes the header on each invocation.
    if os.path.isfile(out_path):
        with open(out_path, "r", encoding="utf-8") as handle:
            if handle.read() == header:
                if verbose:
                    print(f"gen_companion_sprites: {out_path} up to date")
                return 0

    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write(header)

    total = len(companions) * len(MOODS) * sprite_bytes
    print(
        f"gen_companion_sprites: {len(companions)} companions x {len(MOODS)} moods "
        f"= {total} bytes -> {out_path}"
    )
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("sprites_dir", nargs="?", default=None)
    parser.add_argument("output_dir", nargs="?", default=None)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    sys.exit(main(args.sprites_dir, args.output_dir, args.verbose))
else:
    # PlatformIO pre: script entry. The try guards only the SCons-injected
    # Import; a malformed .grid must fail the build loudly rather than be
    # swallowed and leave a stale generated header behind.
    try:
        Import("env")  # noqa: F821 -- injected by PlatformIO
    except NameError:
        pass
    else:
        if main() != 0:
            raise SystemExit(1)
