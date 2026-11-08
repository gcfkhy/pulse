// column_strip_layout.h — Pure horizontal geometry of the column (Miller) view.
#pragma once

#include <d2d1.h>
#include <cstddef>
#include <vector>

namespace pulse::ui {

// Finder-style columns: every column, the current folder's list included,
// has the same default width, so going one level deeper only adds a column
// on the right and the ones already shown stay where they are.
inline constexpr float kColumnStripDefaultDip = 240.0f;
inline constexpr float kColumnStripChildDefaultDip = 240.0f;
inline constexpr float kColumnStripMinDip = 120.0f;
inline constexpr float kColumnStripMaxDip = 560.0f;
// Column id of the trailing "contents of the selected folder" column.
// Ancestor columns use their index into the root-to-parent chain.
inline constexpr int kColumnStripChildId = 10000;
// Divider id of the current folder's list (its right edge).
inline constexpr int kColumnStripBodyId = 10001;
// Width slot of the current folder's list (past every ancestor slot).
inline constexpr size_t kColumnStripBodySlot = 16;

// Width slot 0 is the child column, slot k (1 <= k < kColumnStripBodySlot)
// the k-th ancestor counted from the current folder (1 = parent) and
// kColumnStripBodySlot the current folder. Missing/zero entries use defaults.
float ColumnStripWidthDip(const std::vector<float>& widths_dip, size_t slot) noexcept;
size_t ColumnStripWidthSlot(int column_id, size_t ancestor_count) noexcept;

struct ColumnStripSlot {
    int id = -1;
    D2D1_RECT_F rect{};
};

struct ColumnStripLayout {
    bool active = false;
    // Replaces the pane bounds for everything the regular list draws:
    // same top/bottom as the pane, narrowed horizontally. It is one of the
    // scrolling columns: once the strip overflows it moves with scroll_px and
    // may reach past the pane (draw and hit-test clip to the pane).
    D2D1_RECT_F body{};
    // What the columns are clipped to: the whole pane width when the strip
    // overflows, otherwise just the ancestors (empty when there are none).
    D2D1_RECT_F viewport{};
    float scroll_px = 0.0f;      // how far the whole strip is scrolled from its left end
    float max_scroll_px = 0.0f;  // > 0: the strip is wider than the pane
    float content_px = 0.0f;     // total strip width: ancestors, the list and the child column
    // Left to right: ancestors intersecting the viewport (rects may extend
    // past it; clip to viewport), then the child column (always, even when
    // it is scrolled out of the pane).
    std::vector<ColumnStripSlot> columns;
};

// pane_bounds is the full pane rect; column rects span [top, pane bottom).
// Columns line up from the left: the ancestors, the current folder's list,
// then the child column. When they do not fit the pane, the whole strip
// scrolls horizontally as one. scroll_from_right_dip = 0 rests at the right
// end: the last column ends at the pane's right edge, the parent sits next to
// the list.
ColumnStripLayout LayoutColumnStrip(const D2D1_RECT_F& pane_bounds, float top,
                                    size_t ancestor_count, bool child_slot,
                                    const std::vector<float>& widths_dip,
                                    float scale, float scroll_from_right_dip = 0.0f) noexcept;

// Round arrow buttons at the left and right edge of the pane's column area,
// centred vertically. Each exists only while the strip can still move that
// way, so the one at the end of its travel is gone (not greyed).
inline constexpr float kColumnStripNavDiameterDip = 28.0f;
// Gap to the area's edge; clears the thin vertical scrollbars drawn there.
inline constexpr float kColumnStripNavInsetDip = 10.0f;
// Direction of a nav button / a one-column step: toward the oldest ancestors
// (the strip content moves right) or back toward the list at the right end.
inline constexpr int kColumnStripNavLeft = -1;
inline constexpr int kColumnStripNavRight = 1;

struct ColumnStripNavLayout {
    bool left = false;
    bool right = false;
    D2D1_RECT_F left_rect{};   // bounding square of the round button
    D2D1_RECT_F right_rect{};
};

ColumnStripNavLayout LayoutColumnStripNav(const ColumnStripLayout& layout, float scale) noexcept;

// The strip offset (px from its left end, like ColumnStripLayout::scroll_px)
// one column away from scroll_px: the next column's left edge in `direction`
// (every column of the strip counts, the list and the child column too),
// ending at the ends of the strip. Moves of under 32 dip skip to the
// following edge so a click always travels visibly.
float StepColumnStripScroll(const ColumnStripLayout& layout, size_t ancestor_count,
                            const std::vector<float>& widths_dip, float scale,
                            float scroll_px, int direction) noexcept;

} // namespace pulse::ui
