#pragma once

#include <algorithm>
#include <cstdint>

#include "Epub/css/CssStyle.h"

// Vertical exclusion zone created by a left- or right-floated inline image (figleft/figright).
// Lines whose vertical extent overlaps [top, bottom) have their available width reduced by
// `width` pixels so text wraps correctly beside the image. Coordinates are relative to the page
// top, corrected to the block's real position once its first line is placed (see
// ChapterHtmlSlimParser::attachPendingFloatImage/makePages).
// NOT serialised (BlockStyle's fields are written individually in TextBlock.cpp -- this is never
// one of them): computed at parse time and discarded after layout, like textIndentYields above.
struct FloatZone {
  int16_t top = 0;     // y of image top relative to page top
  int16_t bottom = 0;  // top + imageHeight
  int16_t width = 0;   // imageWidth + a small gap
  bool isRight = false;  // true = image floated right (narrows from the right, no left xOffset shift)
};

/**
 * BlockStyle - Block-level styling properties
 */
struct BlockStyle {
  static constexpr int kMaxFloatZones = 2;
  FloatZone floatZones[kMaxFloatZones];
  int8_t floatZoneCount = 0;
  // XML nesting depth at which this entry was pushed onto
  // ChapterHtmlSlimParser's block-style stack (0 elsewhere/unused). Lets the
  // matching close tag verify it's popping the entry IT pushed, mirroring
  // StyleStackEntry::depth's role for the inline-style stack -- without it, an
  // element whose own push was skipped by the stack's overflow guard would
  // still trigger an unconditional pop on close, popping an ancestor's entry
  // instead.
  int depth = 0;

  // Upper bound (in em) for any single side's horizontal margin or padding.
  // Some EPUBs apply huge em-based insets to chapter-opener classes; without a
  // cap, effectiveWidth collapses to 1-2 words per line and justification dumps
  // the remaining space into a single gap.
  static constexpr float MAX_HORIZONTAL_INSET_EM = 2.0f;

  // Zero keeps the reader font. Scale is layout-only Q8 relative to the
  // user's body size; cached lines retain only the resolved size and height.
  uint16_t fontScale = 256;
  uint8_t fontSize = 0;
  uint16_t lineHeight = 0;

  CssTextAlign alignment = CssTextAlign::Justify;
  // Layout-only source alignment, kept separately from the reader prose override.
  CssTextAlign publisherAlignment = CssTextAlign::None;

  // Spacing (in pixels)
  int16_t marginTop = 0;
  int16_t marginBottom = 0;
  int16_t marginLeft = 0;
  int16_t marginRight = 0;
  int16_t paddingTop = 0;     // treated same as margin for rendering
  int16_t paddingBottom = 0;  // treated same as margin for rendering
  int16_t paddingLeft = 0;    // treated same as margin for rendering
  int16_t paddingRight = 0;   // treated same as margin for rendering
  int16_t textIndent = 0;
  bool textIndentDefined = false;  // true if text-indent was explicitly set in CSS
  // The indent came from margin-left on an inline span (one poem line drawn indented), and gives
  // way before the line does: see ParsedText::resolveFirstLineIndent.
  bool textIndentYields = false;
  bool textAlignDefined = false;   // true if text-align was explicitly set in CSS
  bool isRtl = false;              // true if resolved direction is RTL
  bool directionDefined = false;   // true if direction was explicitly set in CSS/HTML
  // Extra pixels between adjacent non-space glyphs (reader setting, not CSS): set by ParsedText when the
  // paragraph is laid out and read back by TextBlock::render(). Persisted with each cached line.
  int8_t characterSpacing = 0;
  bool pageBreakBefore = false;
  bool pageBreakAfter = false;
  // Set when this block was created by a <br> element. Used by startNewTextBlock to inject
  // a full line-height gap when the <br> block stays empty (section-break use case).
  // NOT propagated through getCombinedBlockStyle so it can't leak into sibling blocks.
  bool fromBrElement = false;
  // Presence-only border sides (see CssStyle.h). Belongs strictly to the
  // element that declared it -- getCombinedBlockStyle starts from a copy of
  // the child style, so these are never inherited from an ancestor block.
  bool borderTop = false;
  bool borderRight = false;
  bool borderBottom = false;
  bool borderLeft = false;
  // Block-level font-size resolution (see FontSizeLadder.h and
  // ChapterHtmlSlimParser's resolveBlockFont). fontSizeMultiplier is the raw
  // CSS value (unitless, relative to the body em size); headingFontId is 0
  // ("use the body font") unless the ladder found a real pre-rendered font
  // resource close enough to render natively. fontResolved guards against
  // re-resolving a block whose style is read more than once. When the ladder
  // can't map the desired size onto a real font resource (empty ladder, e.g.
  // an SD-card body font, or the nearest rung is just the body font itself),
  // headingFontId stays 0 and fontSizeResidualScale carries the leftover
  // scale instead, clamped to ~0.6x-2x -- GfxRenderer::drawTextScaled()
  // resamples the body font's own glyphs rather than leaving font-size with
  // no visual effect at all.
  float fontSizeMultiplier = 1.0f;
  int32_t headingFontId = 0;
  bool fontResolved = false;
  float fontSizeResidualScale = 1.0f;

  // Set directly from a class="...hr-sect..." token match in
  // ChapterHtmlSlimParser::startElement(), not derived from CssStyle/CSS
  // rules at all -- FanFicFare's own boilerplate stylesheet draws this
  // decorative divider's flanking lines via ::before/::after pseudo-elements
  // and flexbox, neither of which CssParser supports, so the class name
  // itself is the only signal. addLineToPage() uses it to draw two short
  // horizontal lines flanking this block's (already CSS-centered) content.
  bool hrSectDivider = false;

  // Combined insets (margin + padding)
  [[nodiscard]] int16_t leftInset() const { return marginLeft + paddingLeft; }
  [[nodiscard]] int16_t rightInset() const { return marginRight + paddingRight; }
  [[nodiscard]] int16_t totalHorizontalInset() const { return leftInset() + rightInset(); }
  [[nodiscard]] int16_t topInset() const { return marginTop + paddingTop; }
  [[nodiscard]] int16_t bottomInset() const { return marginBottom + paddingBottom; }

  // Return a copy with bottom margins/padding zeroed out.
  [[nodiscard]] BlockStyle withoutBottom() const {
    BlockStyle result = *this;
    result.marginBottom = 0;
    result.paddingBottom = 0;
    return result;
  }

  // Return a copy with top margins/padding zeroed out.
  [[nodiscard]] BlockStyle withoutTop() const {
    BlockStyle result = *this;
    result.marginTop = 0;
    result.paddingTop = 0;
    return result;
  }

  // Return a copy with bottom margins/padding collapsed (max) with the source's.
  // Uses CSS margin collapsing: adjacent parent-child margins resolve to the larger value.
  [[nodiscard]] BlockStyle addBottom(const BlockStyle& source) const {
    BlockStyle result = *this;
    result.marginBottom = std::max(marginBottom, source.marginBottom);
    result.paddingBottom = static_cast<int16_t>(paddingBottom + source.paddingBottom);
    return result;
  }

  enum class CombineAxis : uint8_t {
    Horizontal = 1,  // margins left/right, padding left/right, text-align, text-indent
    Vertical = 2,    // margins top/bottom, padding top/bottom
  };

  // Combine this style's properties with a child style along the specified axis.
  // Properties on the other axis are kept from the child unchanged.
  [[nodiscard]] BlockStyle getCombinedBlockStyle(const BlockStyle& child, CombineAxis axis) const {
    BlockStyle result = child;

    if (axis == CombineAxis::Horizontal) {
      result.marginLeft = static_cast<int16_t>(child.marginLeft + marginLeft);
      result.marginRight = static_cast<int16_t>(child.marginRight + marginRight);
      result.paddingLeft = static_cast<int16_t>(child.paddingLeft + paddingLeft);
      result.paddingRight = static_cast<int16_t>(child.paddingRight + paddingRight);
      if (!child.textIndentDefined && textIndentDefined) {
        result.textIndent = textIndent;
        result.textIndentDefined = true;
        result.textIndentYields = textIndentYields;
      }
      if (child.publisherAlignment == CssTextAlign::None) {
        result.publisherAlignment = publisherAlignment;
      }
      if (!child.textAlignDefined && textAlignDefined) {
        result.alignment = alignment;
        result.textAlignDefined = true;
      }
    } else {
      result.marginTop = std::max(child.marginTop, marginTop);
      result.marginBottom = std::max(child.marginBottom, marginBottom);
      result.paddingTop = static_cast<int16_t>(child.paddingTop + paddingTop);
      result.paddingBottom = static_cast<int16_t>(child.paddingBottom + paddingBottom);
    }
    // fromBrElement is consumed by startNewTextBlock and should not leak through ancestor style merging.
    result.fromBrElement = false;

    // Direction is inherited independently of the horizontal/vertical box model.
    if (!child.directionDefined && directionDefined) {
      result.isRtl = isRtl;
      result.directionDefined = true;
    }
    return result;
  }

  // Create a BlockStyle from CSS style properties, resolving CssLength values to pixels
  // emSize is the current font line height, used for em/rem unit conversion
  // paragraphAlignment is the user's paragraphAlignment setting preference
  // viewportHeight is optional context for resolving percentage vertical margins/padding against
  // the column's short axis instead of width -- see verticalPercentBasis below.
  static BlockStyle fromCssStyle(const CssStyle& cssStyle, const float emSize, const CssTextAlign paragraphAlignment,
                                 const uint16_t viewportWidth = 0, const uint16_t viewportHeight = 0) {
    BlockStyle blockStyle;
    const float vw = viewportWidth;
    // CSS spec resolves percentage margins/padding -- vertical ones included -- against the
    // containing block's width. On a reader that rotates between portrait and landscape, that
    // makes vertical percentage gaps balloon in landscape: width becomes the long axis there,
    // so the same `margin: 2% 0` that looked right in portrait is now sized off a much bigger
    // number. Using the short axis (whichever of width/height is smaller) keeps vertical
    // whitespace visually proportionate to the actually-available vertical room in both
    // orientations; in portrait, width already IS the short axis, so this changes nothing there.
    // A deliberate e-reader-usability deviation from spec, not a spec-compliance fix.
    const float verticalPercentBasis = (viewportHeight > 0) ? std::min(vw, static_cast<float>(viewportHeight)) : vw;
    const auto maxHorizontalInsetPx = static_cast<int16_t>(emSize * MAX_HORIZONTAL_INSET_EM);
    // Resolve all CssLength values to pixels using the current font's em size and viewport width
    blockStyle.marginTop = cssStyle.marginTop.toPixelsInt16(emSize, verticalPercentBasis);
    blockStyle.marginBottom = cssStyle.marginBottom.toPixelsInt16(emSize, verticalPercentBasis);
    blockStyle.marginLeft = std::min(cssStyle.marginLeft.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);
    blockStyle.marginRight = std::min(cssStyle.marginRight.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);

    blockStyle.paddingTop = cssStyle.paddingTop.toPixelsInt16(emSize, verticalPercentBasis);
    blockStyle.paddingBottom = cssStyle.paddingBottom.toPixelsInt16(emSize, verticalPercentBasis);
    blockStyle.paddingLeft = std::min(cssStyle.paddingLeft.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);
    blockStyle.paddingRight = std::min(cssStyle.paddingRight.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);

    // For textIndent: if it's a percentage we can't resolve (no viewport width),
    // leave textIndentDefined=false so the space-width fallback in resolveFirstLineIndent() is used
    if (cssStyle.hasTextIndent() && cssStyle.textIndent.isResolvable(vw)) {
      blockStyle.textIndent = cssStyle.textIndent.toPixelsInt16(emSize, vw);
      blockStyle.textIndentDefined = true;
    }
    blockStyle.textAlignDefined = cssStyle.hasTextAlign();
    if (blockStyle.textAlignDefined) blockStyle.publisherAlignment = cssStyle.textAlign;
    // User setting overrides CSS, unless "Book's Style" alignment setting is selected
    if (paragraphAlignment == CssTextAlign::None) {
      blockStyle.alignment = blockStyle.textAlignDefined ? cssStyle.textAlign : CssTextAlign::Justify;
    } else {
      blockStyle.alignment = paragraphAlignment;
    }
    if (cssStyle.hasDirection()) {
      blockStyle.isRtl = cssStyle.direction == CssTextDirection::Rtl;
      blockStyle.directionDefined = true;
    }
    if (cssStyle.hasPageBreakBefore()) {
      blockStyle.pageBreakBefore = cssStyle.pageBreakBefore;
    }
    if (cssStyle.hasPageBreakAfter()) {
      blockStyle.pageBreakAfter = cssStyle.pageBreakAfter;
    }
    if (cssStyle.hasBorderTop()) blockStyle.borderTop = cssStyle.borderTop;
    if (cssStyle.hasBorderRight()) blockStyle.borderRight = cssStyle.borderRight;
    if (cssStyle.hasBorderBottom()) blockStyle.borderBottom = cssStyle.borderBottom;
    if (cssStyle.hasBorderLeft()) blockStyle.borderLeft = cssStyle.borderLeft;
    if (cssStyle.hasFontSizeMultiplier()) blockStyle.fontSizeMultiplier = cssStyle.fontSizeMultiplier;
    return blockStyle;
  }
};
