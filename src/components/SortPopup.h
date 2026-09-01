#pragma once
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/icon_sort_direction.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

// A popup for picking one sort field plus its direction (ascending/descending).
// Deliberately not built on OptionPopup: OptionPopup's confirmationMode closes
// the popup the instant any row is tapped (see its handleInput()/activateSelection()),
// with no per-row secondary state -- which doesn't fit "tap a field to select it,
// tap it again to flip its direction, then a separate Confirm to apply." Rendering
// still goes through the same GUI.drawOptionPopup() every option-style popup in this
// app uses, so visually it matches; only the interaction model and layout math (kept
// as SortPopup's own copy, the same way OptionPopup's layout doesn't share code with
// BaseTheme::drawOptionPopup's own layout math) are new.
//
// Shared by FileBrowserActivity and BookFusionBrowserActivity.
class SortPopup {
 public:
  // fieldLabels are plain field names (e.g. "Title", "Author") -- the active field's
  // direction indicator is appended automatically when rendering. activeField < 0
  // means no field is active yet (first-time open); onApply(field, ascending) fires
  // only when Confirm is pressed with a field actually active.
  void show(StrId titleId, std::vector<std::string> fieldLabels, int activeField, bool ascending,
            std::function<void(int field, bool ascending)> onApply, std::function<void()> onCancel = nullptr) {
    title = I18N.get(titleId);
    labels = std::move(fieldLabels);
    onApplyCallback = std::move(onApply);
    onCancelCallback = std::move(onCancel);
    const int count = static_cast<int>(labels.size());
    activeIndex = (activeField >= 0 && activeField < count) ? activeField : -1;
    isAscending = ascending;
    highlightIndex = std::clamp(activeIndex >= 0 ? activeIndex : 0, 0, std::max(0, count - 1));
    longPressHandled = false;
    touchDownIndex = -1;
    touchDownTarget = TouchTarget::None;
    layoutValid = false;
    active = count > 0;
  }

  bool isActive() const { return active; }

  bool handleInput(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (!active) return false;
    const int count = static_cast<int>(labels.size());
    if (count <= 0) {
      active = false;
      return true;
    }

    // Touch: a short tap selects a row (resetting it to ascending) or, tapped again
    // on the row that's already selected, applies and closes -- the same "tap to
    // pick, tap again to confirm" shape as picking a file in the browser. A tap
    // anywhere outside the dialog cancels, matching every other popup's outside-tap
    // convention. A long-press on a row flips its direction in place without
    // selecting-or-confirming, so direction can be set before or after the field is
    // already active. Canceling is also available via Back (a physical Back press,
    // or the standard left-edge swipe-in gesture, which
    // MappedInputManager::wasReleased(Back) already recognizes on its own).
    int lpx = 0;
    int lpy = 0;
    if (input.wasScreenLongPress(lpx, lpy)) {
      const auto& hitLayout = getLayout(input.getRenderer());
      for (int i = 0; i < static_cast<int>(hitLayout.options.size()); i++) {
        if (contains(hitLayout.options[i], lpx, lpy)) {
          input.suppressCurrentTouchContact();
          toggleDirection(hitLayout.firstOptionIndex + i);
          touchDownIndex = -1;
          touchDownTarget = TouchTarget::None;
          requestUpdate();
          return true;
        }
      }
    }

    int tx = 0;
    int ty = 0;
    if (input.wasScreenTouchDown(tx, ty)) {
      const auto& hitLayout = getLayout(input.getRenderer());
      touchDownIndex = -1;
      touchDownTarget = TouchTarget::None;
      for (int i = 0; i < static_cast<int>(hitLayout.options.size()); i++) {
        if (contains(hitLayout.options[i], tx, ty)) {
          touchDownIndex = hitLayout.firstOptionIndex + i;
          touchDownTarget = TouchTarget::Option;
          if (highlightIndex != touchDownIndex) {
            highlightIndex = touchDownIndex;
            layoutValid = false;
            requestUpdate();
          }
          break;
        }
      }
      if (touchDownTarget == TouchTarget::None && !contains(hitLayout.dialog, tx, ty)) {
        touchDownTarget = TouchTarget::Outside;
      }
      return true;
    }

    if (input.wasScreenTapped(tx, ty)) {
      const auto& hitLayout = getLayout(input.getRenderer());
      if (touchDownTarget == TouchTarget::Outside && !contains(hitLayout.dialog, tx, ty)) {
        cancel(input, requestUpdate, false);
        return true;
      }
      if (touchDownTarget == TouchTarget::Option && touchDownIndex >= 0) {
        const bool alreadyActive = selectOrActivate(touchDownIndex);
        touchDownIndex = -1;
        if (alreadyActive) {
          confirm(input, requestUpdate, false);
        } else {
          requestUpdate();
        }
        return true;
      }
      // A tap that lands inside the dialog but on neither a row nor (already
      // handled above) outside it -- e.g. the title -- is a no-op, not a cancel.
      return true;
    }

    const auto swipe = input.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const auto& hitLayout = getLayout(input.getRenderer());
      const int visibleCount = static_cast<int>(hitLayout.options.size());
      if (visibleCount < count) {
        const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleCount : -visibleCount;
        highlightIndex = std::clamp(highlightIndex + delta, 0, count - 1);
        layoutValid = false;
        requestUpdate();
      }
      touchDownIndex = -1;
      touchDownTarget = TouchTarget::None;
      return true;
    }

    // Non-touch: Up/Down move a highlight cursor through the N label rows (no
    // virtual footer slot needed anymore -- see below). PageBack/PageForward are
    // NOT used here: on at least this hardware they turned out to be dual-mapped
    // to the same physical buttons as Left/Right, which ButtonNavigator already
    // treats as row-navigation Next/Previous, so a PageBack/PageForward press was
    // silently moving the highlight instead of toggling a direction.
    buttonNavigator.onPreviousRelease([&] {
      highlightIndex = ButtonNavigator::previousIndex(highlightIndex, count);
      layoutValid = false;
      requestUpdate();
    });
    buttonNavigator.onNextRelease([&] {
      highlightIndex = ButtonNavigator::nextIndex(highlightIndex, count);
      layoutValid = false;
      requestUpdate();
    });

    // Mirrors the touch model: a short Confirm press on the highlighted row
    // selects it (or applies and closes if it's already the active row); holding
    // Confirm flips that row's direction in place, the same "hold vs. short press"
    // split BookFusionBrowserActivity's own long-press-Confirm already uses.
    if (!longPressHandled && input.isPressed(MappedInputManager::Button::Confirm) &&
        input.getHeldTime() >= kLongPressMs) {
      longPressHandled = true;
      toggleDirection(highlightIndex);
      requestUpdate();
      return true;
    }
    if (input.wasReleased(MappedInputManager::Button::Confirm)) {
      if (longPressHandled) {
        longPressHandled = false;
        return true;
      }
      if (selectOrActivate(highlightIndex)) {
        confirm(input, requestUpdate, true);
      } else {
        requestUpdate();
      }
      return true;
    }
    if (input.wasReleased(MappedInputManager::Button::Back)) {
      cancel(input, requestUpdate, false);
      return true;
    }
    return true;
  }

  bool processRender(GfxRenderer& renderer, const MappedInputManager& input) const {
    if (!active) return false;
    // Confirm's short-press action is "select this row" (or apply, once it's
    // already the active one) -- the hint bar has no separate slot for the
    // hold-to-flip-direction gesture, matching how BookFusionBrowserActivity's own
    // long-press-Confirm shortcut isn't hinted either.
    const auto popupLabels = input.mapLabels(tr(STR_CANCEL), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, popupLabels.btn1, popupLabels.btn2, popupLabels.btn3, popupLabels.btn4, true);
    render(renderer);
    renderer.displayBuffer();
    return true;
  }

  void render(const GfxRenderer& renderer) const {
    if (!active) return;
    // No on-screen footer: touch applies by tapping outside the dialog, cancels by
    // edge-swipe; non-touch still applies via physical Confirm on the (invisible)
    // footer slot -- the button-hint bar (see processRender()) is what communicates
    // that state to non-touch users now, not a drawn footer bar.
    GUI.drawOptionPopup(renderer, title.c_str(), labels, highlightIndex, /*showConfirmationFooter=*/false, nullptr,
                        nullptr, false, /*primaryOptionIndex=*/-1, nullptr, nullptr);

    // The active field's direction icon draws as a separate overlay after
    // drawOptionPopup, since that shared renderer only takes plain row strings.
    if (activeIndex < 0) return;
    const auto& hitLayout = getLayout(renderer);
    const int visibleOffset = activeIndex - hitLayout.firstOptionIndex;
    if (visibleOffset < 0 || visibleOffset >= static_cast<int>(hitLayout.options.size())) return;
    const Rect& row = hitLayout.options[visibleOffset];

    // drawOptionPopup centers each row's label text within the row
    // (textX = itemRectX + (itemRectW - textW) / 2, see BaseTheme::drawOptionPopup) --
    // anchoring the icon to the row's right edge instead left a large gap after short
    // labels. Replicate that same centering here so the icon sits right after the text.
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int optionFontId = uiScaleSpec().bodyFontId;
    const EpdFontFamily::Style optionStyle =
        metrics.optionPopupOptionFontBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int textW = renderer.getTextWidth(optionFontId, labels[activeIndex].c_str(), optionStyle);
    const int textEndX = row.x + (row.width + textW) / 2;

    const auto& icon = isAscending ? icon_sort_asc_24 : icon_sort_desc_24;
    const int iconX = std::min(textEndX + kDirectionIconGap, row.x + row.width - icon.w);
    const int iconY = row.y + (row.height - icon.h) / 2;
    drawLucideIcon(renderer, icon, iconX, iconY);
  }

 private:
  // icon_sort_asc_24/icon_sort_desc_24's own square size, plus a small gap from the
  // row's text before it and the row's right inner edge after it.
  static constexpr int kDirectionIconGap = 6;
  static constexpr int kDirectionIconReserve = 24 + kDirectionIconGap * 2;
  // Matches BookFusionBrowserActivity's SORT_LONG_PRESS_MS for the non-touch
  // held-Confirm toggle -- same physical gesture, same threshold.
  static constexpr unsigned long kLongPressMs = 600;

  struct Layout {
    Rect dialog{0, 0, 0, 0};
    std::vector<Rect> options;
    int firstOptionIndex = 0;
  };

  enum class TouchTarget : uint8_t { None, Option, Outside };

  // Mirrors BaseTheme::drawOptionPopup's own layout math (see that function's comment
  // for why this isn't shared code -- OptionPopup keeps an equivalent private copy of
  // its own for the same reason: the renderer and the touch-hit-tester are two call
  // sites that both need the same numbers, and this component predates a shared helper).
  const Layout& getLayout(const GfxRenderer& renderer) const {
    if (layoutValid) return layout;

    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();
    const int optionFontId = uiScaleSpec().bodyFontId;
    const EpdFontFamily::Style optionStyle =
        metrics.optionPopupOptionFontBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

    const int itemSpacing = metrics.optionPopupItemSpacing;
    const int innerPadding = metrics.optionPopupInnerPadding;
    const int selectionHPadding = metrics.optionPopupSelectionHPadding;

    const int optionLineHeight = renderer.getLineHeight(optionFontId);
    const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int rowHeight = optionLineHeight + metrics.optionPopupSelectionVPadding * 2;

    int maxTextWidth = renderer.getTextWidth(UI_12_FONT_ID, title.c_str(), EpdFontFamily::BOLD);
    for (const auto& opt : labels) {
      // The direction icon only ever draws on one row, but every row reserves its
      // width so the dialog doesn't resize when the active field changes.
      const int width = renderer.getTextWidth(optionFontId, opt.c_str(), optionStyle) + kDirectionIconReserve;
      if (width > maxTextWidth) maxTextWidth = width;
    }

    // No footer reserved here, matching drawOptionPopup's own math when
    // showConfirmationFooter=false (see render()).
    const int optionCount = static_cast<int>(labels.size());
    const int maxDialogH =
        std::max(rowHeight + titleLineHeight + metrics.optionPopupTitleGap + innerPadding * 2,
                 pageHeight - metrics.buttonHintsHeight - metrics.optionPopupDialogSideMargin * 2);
    const int dialogW = std::min((maxTextWidth + innerPadding * 2 + selectionHPadding * 2 + metrics.scrollBarWidth +
                                  metrics.scrollBarRightOffset + selectionHPadding) *
                                     12 / 10,
                                 pageWidth - metrics.optionPopupDialogSideMargin * 2);
    const int titleContentWidth = std::max(1, dialogW - innerPadding * 2);
    const int maxTitleLines =
        std::max(1, (maxDialogH - innerPadding * 2 - metrics.optionPopupTitleGap - rowHeight) / titleLineHeight);
    const auto titleLines =
        renderer.wrappedText(UI_12_FONT_ID, title.c_str(), titleContentWidth, maxTitleLines, EpdFontFamily::BOLD);
    const int titleHeight = static_cast<int>(titleLines.size()) * titleLineHeight;
    const int maxListHeight =
        std::max(rowHeight, maxDialogH - innerPadding * 2 - titleHeight - metrics.optionPopupTitleGap);
    const int rowStep = rowHeight + itemSpacing;
    const int visibleCount = std::max(1, std::min(optionCount, (maxListHeight + itemSpacing) / rowStep));
    const int safeHighlight = std::clamp(highlightIndex, 0, optionCount - 1);
    const int visibleStart = std::clamp(safeHighlight - visibleCount / 2, 0, optionCount - visibleCount);
    const int listHeight = rowHeight * visibleCount + itemSpacing * (visibleCount - 1);
    const bool hasHiddenOptions = visibleCount < optionCount;
    const int scrollBarGutter =
        hasHiddenOptions ? metrics.scrollBarWidth + metrics.scrollBarRightOffset + selectionHPadding : 0;
    const int contentHeight = titleHeight + metrics.optionPopupTitleGap + listHeight;
    const int dialogH = contentHeight + innerPadding * 2;
    const int dialogX = (pageWidth - dialogW) / 2;
    const int dialogY = (pageHeight - dialogH) / 2;
    const int itemRectX = dialogX + innerPadding;
    const int itemRectW = std::max(1, dialogW - innerPadding * 2 - scrollBarGutter);
    const int firstItemY = dialogY + innerPadding + titleHeight + metrics.optionPopupTitleGap;

    layout.dialog = Rect{dialogX, dialogY, dialogW, dialogH};
    layout.firstOptionIndex = visibleStart;
    layout.options.clear();
    layout.options.reserve(visibleCount);
    for (int i = 0; i < visibleCount; i++) {
      layout.options.push_back(Rect{itemRectX, firstItemY + i * (rowHeight + itemSpacing), itemRectW, rowHeight});
    }

    layoutValid = true;
    return layout;
  }

  static bool contains(const Rect& rect, const int x, const int y) {
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
  }

  // Selecting a field that isn't active makes it active at the default (ascending)
  // direction and returns false; selecting the field that's already active does
  // nothing (the caller applies/confirms instead) and returns true.
  bool selectOrActivate(int index) {
    if (index < 0 || index >= static_cast<int>(labels.size())) return false;
    if (index == activeIndex) return true;
    activeIndex = index;
    isAscending = true;
    highlightIndex = index;
    layoutValid = false;
    return false;
  }

  // Long-press (touch) / held Confirm (non-touch): activates the field at the
  // default direction if it wasn't already active, or flips its direction in
  // place if it was -- independent of selectOrActivate's select-vs-confirm split.
  void toggleDirection(int index) {
    if (index < 0 || index >= static_cast<int>(labels.size())) return;
    if (index == activeIndex) {
      isAscending = !isAscending;
    } else {
      activeIndex = index;
      isAscending = true;
    }
    highlightIndex = index;
    layoutValid = false;
  }

  void confirm(MappedInputManager& input, const std::function<void()>& requestUpdate, const bool suppressRelease) {
    active = false;
    if (suppressRelease) input.suppressNextConfirmRelease();
    if (onApplyCallback && activeIndex >= 0) onApplyCallback(activeIndex, isAscending);
    requestUpdate();
  }

  void cancel(MappedInputManager& input, const std::function<void()>& requestUpdate, const bool suppressRelease) {
    active = false;
    if (suppressRelease) input.suppressNextBackRelease();
    if (onCancelCallback) onCancelCallback();
    requestUpdate();
  }

  bool active = false;
  std::string title;
  std::vector<std::string> labels;
  int activeIndex = -1;
  bool isAscending = true;
  int highlightIndex = 0;
  // Non-touch only: one-shot guard so a held-past-threshold Confirm press fires
  // toggleDirection() exactly once, and the eventual release doesn't also fire
  // the short-press select/confirm action (same pattern as
  // BookFusionBrowserActivity's longPressConfirmHandled).
  bool longPressHandled = false;
  int touchDownIndex = -1;
  TouchTarget touchDownTarget = TouchTarget::None;
  std::function<void(int, bool)> onApplyCallback;
  std::function<void()> onCancelCallback;
  ButtonNavigator buttonNavigator;
  mutable Layout layout;
  mutable bool layoutValid = false;
};
