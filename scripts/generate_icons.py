#!/usr/bin/env python3
"""Generate CrossInk icon headers from a pinned SVG icon library."""

import argparse
from pathlib import Path
import subprocess
import sys


PROJECT_ROOT = Path(__file__).resolve().parent.parent
SDK_GENERATOR = PROJECT_ROOT / "freeink-sdk/libs/assets/Icons/tools/gen_icons.py"
SVG_DIRS = {
    "lucide": PROJECT_ROOT / "freeink-sdk/libs/assets/Icons/lucide/icons",
    "tabler": PROJECT_ROOT / "assets/tabler-icons/icons/outline",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", choices=SVG_DIRS, required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--sizes", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    svg_dir = SVG_DIRS[args.library]
    if not SDK_GENERATOR.is_file():
        parser.error(f"FreeInk SDK icon generator is missing: {SDK_GENERATOR}")
    if not svg_dir.is_dir():
        parser.error(f"{args.library.title()} icon source is missing: {svg_dir}")

    output = Path(args.out)
    subprocess.run(
        [
            sys.executable,
            str(SDK_GENERATOR),
            "--manifest",
            args.manifest,
            "--svgdir",
            str(svg_dir),
            "--sizes",
            args.sizes,
            "--out",
            str(output),
        ],
        check=True,
    )

    # The SDK generator accepts any single-colour SVGs, but its generated
    # comments currently name Lucide. Keep firmware headers accurate without
    # changing the SDK implementation.
    if args.library == "tabler":
        generated = output.read_text()
        output.write_text(generated.replace("Lucide", "Tabler").replace("lucide:", "tabler:"))


if __name__ == "__main__":
    main()
