#!/bin/bash

set -e

cd "$(dirname "$0")"

SYMBOLS_FONT="../builtinFonts/source/NotoSymbols/NotoSansSymbols-Regular.ttf"
PHM_FONT="../builtinFonts/source/NotoSansCJKsc/NotoSansCJKsc-Regular.otf"

# Additional Unicode intervals to include beyond the default Latin/Cyrillic/math set.
# 0x2669-0x266F: Music notes and accidentals (♩♪♫♬♭♮♯)
# 0x03BB: Greek lambda (λ)
# 0x0410-0x0414, 0x0418, 0x041B, 0x041D-0x0423, 0x0425, 0x0427,
# 0x042B-0x042C, 0x042E-0x0432, 0x0434-0x0435, 0x0437, 0x043A,
# 0x043D-0x043E, 0x0440, 0x0442, 0x0446, 0x044C, 0x044E: Cyrillic subset
# 0x2113: Script small l (ℓ)
COMMON_FALLBACK_INTERVALS=(
  --additional-intervals 0x03BB,0x03BB
  --additional-intervals 0x0410,0x0414
  --additional-intervals 0x0418,0x0418
  --additional-intervals 0x041B,0x041B
  --additional-intervals 0x041D,0x0423
  --additional-intervals 0x0425,0x0425
  --additional-intervals 0x0427,0x0427
  --additional-intervals 0x042B,0x042C
  --additional-intervals 0x042E,0x0432
  --additional-intervals 0x0434,0x0435
  --additional-intervals 0x0437,0x0437
  --additional-intervals 0x043A,0x043A
  --additional-intervals 0x043D,0x043E
  --additional-intervals 0x0440,0x0440
  --additional-intervals 0x0442,0x0442
  --additional-intervals 0x0446,0x0446
  --additional-intervals 0x044C,0x044C
  --additional-intervals 0x044E,0x044E
  --additional-intervals 0x2113,0x2113
)

MUSIC_SYMBOL_INTERVALS=(
  --additional-intervals 0x2669,0x266F
)

READING_FALLBACK_INTERVALS=(
  "${COMMON_FALLBACK_INTERVALS[@]}"
  "${MUSIC_SYMBOL_INTERVALS[@]}"
)

# CJK for PHM
PHM_INTERVALS=(
  --additional-intervals 0x4F1A,0x4F1A
  --additional-intervals 0x53BB,0x53BB
  --additional-intervals 0x5458,0x5458
  --additional-intervals 0x59DA,0x59DA
  --additional-intervals 0x5B98,0x5B98
  --additional-intervals 0x5BA4,0x5BA4
  --additional-intervals 0x5E26,0x5E26
  --additional-intervals 0x6211,0x6211
  --additional-intervals 0x62C9,0x62C9
  --additional-intervals 0x653E,0x653E
  --additional-intervals 0x6746,0x677F
  --additional-intervals 0x7532,0x7532
  --additional-intervals 0x7684,0x7684
  --additional-intervals 0x8BAE,0x8BAE
  --additional-intervals 0x8BF7,0x8BF7
  --additional-intervals 0x91CA,0x91CA
)

CHAREINK_FALLBACK_RANGES=(
  0x03BB,0x03BB
  0x0410,0x0414
  0x0418,0x0418
  0x041B,0x041B
  0x041D,0x0423
  0x0425,0x0425
  0x0427,0x0427
  0x042B,0x042C
  0x042E,0x0432
  0x0434,0x0435
  0x0437,0x0437
  0x043A,0x043A
  0x043D,0x043E
  0x0440,0x0440
  0x0442,0x0442
  0x0446,0x0446
  0x044C,0x044C
  0x044E,0x044E
  0x2113,0x2113
)

SYMBOL_FALLBACK_RANGES=(
  0x2669,0x266F
)

# CJK for PHM
PHM_FALLBACK_RANGES=(
  0x4F1A,0x4F1A
  0x53BB,0x53BB
  0x5458,0x5458
  0x59DA,0x59DA
  0x5B98,0x5B98
  0x5BA4,0x5BA4
  0x5E26,0x5E26
  0x6211,0x6211
  0x62C9,0x62C9
  0x653E,0x653E
  0x6746,0x677F
  0x7532,0x7532
  0x7684,0x7684
  0x8BAE,0x8BAE
  0x8BF7,0x8BF7
  0x91CA,0x91CA
)

READING_FONT_SIZES=(10 12 14 16)
READING_FONT_STYLES=("Regular" "Bold" "Italic" "BoldItalic")
READING_FONT_RENDER_ARGS=(--2bit --compress --pnum --darken-aa --zopfli)

font_include_args() {
  local face_index="$1"
  shift
  for range in "$@"; do
    printf '%s\n' --font-include-intervals "${face_index}:${range}"
  done
}

generate_family() {
  local family_name="$1"
  local source_dir="$2"
  local source_prefix="$3"
  local use_chareink_common_fallback="$4"

  for size in ${READING_FONT_SIZES[@]}; do
    for style in ${READING_FONT_STYLES[@]}; do
      local style_lower
      style_lower="$(echo $style | tr '[:upper:]' '[:lower:]')"
      local font_name="${family_name}_${size}_${style_lower}"
      local font_path="../builtinFonts/source/${source_dir}/${source_prefix}-${style}.ttf"
      local output_path="../builtinFonts/${font_name}.h"
      local font_stack=("$font_path")
      local interval_args=("${READING_FALLBACK_INTERVALS[@]}")
      local include_args=()

      if [[ "$use_chareink_common_fallback" == "yes" ]]; then
        font_stack+=("../builtinFonts/source/ChareInk7/ChareInk7-${style}.ttf")
        include_args+=($(font_include_args $(( ${#font_stack[@]} - 1 )) "${CHAREINK_FALLBACK_RANGES[@]}"))
      fi
      font_stack+=("$SYMBOLS_FONT")
      include_args+=($(font_include_args $(( ${#font_stack[@]} - 1 )) "${SYMBOL_FALLBACK_RANGES[@]}"))

      if [[ "$style" == "Regular" ]]; then
        interval_args+=("${PHM_INTERVALS[@]}")
        font_stack+=("$PHM_FONT")
        include_args+=($(font_include_args $(( ${#font_stack[@]} - 1 )) "${PHM_FALLBACK_RANGES[@]}"))
      fi

      python fontconvert.py $font_name $size "${font_stack[@]}" "${interval_args[@]}" "${include_args[@]}" "${READING_FONT_RENDER_ARGS[@]}" > $output_path
      echo "Generated $output_path"
    done
  done
}

generate_reading_fonts() {
  echo "Generating built-in reading fonts..."
  generate_family lexenddeca LexendDeca LexendDeca yes
  generate_family bitter Bitter Bitter yes
  echo ""
  echo "Built-in reading fonts complete."
  echo ""
}

# Built-in reader fonts retain the PHM fallback ranges, but exclude emoticons.
generate_reading_fonts

# Shared fixed-size UI symbols
bash ./generate-ui-symbols.sh

# UI Font - Inter

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

# Hebrew/Arabic UI-font coverage (menus, file browser titles) was dropped
# 2026-11-21 to reclaim ~31.5KB of flash: this build runs English-only
# (custom_i18n_builtin_langs=en in platformio.ini), so no UI string is ever
# in Hebrew/Arabic. Known rough edge, accepted deliberately: the SDK's
# on-screen ArabicAr keyboard layout and any manually-typed Hebrew/Arabic
# text (a filename, a search query) will now render missing-glyph boxes in
# this font. Restore by reinstating the IBMPlexSansHebrew/NotoSansArabic
# source fonts and the ARABIC_INTERVALS/0x05D0-0x05EA args removed here --
# see git history on this file for the exact prior invocation.

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="inter_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Inter/Inter-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path > $output_path
    echo "Generated $output_path"
  done
done

# Small UI Font - Inter

python fontconvert.py inter_8_regular 8 \
  ../builtinFonts/source/Inter/Inter-Regular.ttf > ../builtinFonts/inter_8_regular.h

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
