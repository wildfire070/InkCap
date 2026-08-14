#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT_DIR"

process() {
  local lang="$1"

  mkdir -p "build"
  wget -O "build/$lang.bin" "https://github.com/typst/hypher/raw/refs/heads/main/tries/$lang.bin"

  python scripts/generate_hyphenation_trie.py \
    --input "build/$lang.bin" \
    --output "lib/Epub/Epub/hyphenation/generated/hyph-${lang}.trie.h"
}

process en
process fr
# German (de) intentionally omitted in InkCap: its ~206 KB trie was dropped to
# reclaim C3 (X3/X4) flash. Re-enable this line if German hyphenation is wanted.
# process de
process es
process ru
process it
process uk
process pl
process sv
process pt
