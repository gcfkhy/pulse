#include "column_strip_layout.h"

#include <algorithm>

namespace pulse::ui {

float ColumnStripWidthDip(const std::vector<float>& widths_dip, size_t slot) noexcept {
    const float fallback = slot == 0 ? kColumnStripChildDefaultDip : kColumnStripDefaultDip;
    if (slot >= widths_dip.size() || widths_dip[slot] <= 0.0f) return fallback;
    return std::clamp(widths_dip[slot], kColumnStripMinDip, kColumnStripMaxDip);
}

size_t ColumnStripWidthSlot(int column_id, size_t ancestor_count) noexcept {
    if (column_id == kColumnStripBodyId) return kColumnStripBodySlot;
    if (column_id == kColumnStripChildId || column_id < 0) return 0;
    const size_t index = static_cast<size_t>(column_id);
    return index < ancestor_count ? ancestor_count - index : 0;
}

ColumnStripLayout LayoutColumnStrip(const D2D1_RECT_F& pane_bounds, float top,
                                    size_t ancestor_count, bool child_slot,
                                    const std::vector<float>& widths_dip,
                                    float scale, float scroll_from_right_dip) noexcept {
    ColumnStripLayout out;
    out.body = pane_bounds;
    const float unit = std::max(0.5f, scale);
    const float width = std::max(0.0f, pane_bounds.right - pane_bounds.left);
    const float min_w = kColumnStripMinDip * unit;
    if (width < min_w) return out;

    const float body_w = std::min(width, ColumnStripWidthDip(widths_dip, kColumnStripBodySlot) * unit);
    float content = 0.0f;
    for (size_t k = 1; k <= ancestor_count; ++k) content += ColumnStripWidthDip(widths_dip, k) * unit;

    // The selected folder's contents come before the ancestors; the column
    // narrows to fit. A selected file adds nothing: its properties live in
    // the details panel.
    float trail_w = 0.0f;
    if (child_slot) {
        const float w = ColumnStripWidthDip(widths_dip, 0) * unit;
        if (body_w + w <= width) trail_w = w;
        else if (width - body_w >= min_w) trail_w = width - body_w;
    }

    // One strip: the ancestors, the list, the child column. It rests at its
    // right end (the last column ends at the pane's edge) and scrolls as a whole.
    const float total = content + body_w + trail_w;
    out.active = true;
    const float bottom = std::max(top, pane_bounds.bottom);
    out.content_px = total;
    out.max_scroll_px = std::max(0.0f, total - width);
    out.scroll_px = std::clamp(out.max_scroll_px - std::max(0.0f, scroll_from_right_dip) * unit,
                               0.0f, out.max_scroll_px);
    const float strip_left = pane_bounds.left - out.scroll_px;
    out.body.left = strip_left + content;
    out.body.right = out.body.left + body_w;
    if (content > 0.0f) {
        // Overflowing, the columns show through the whole pane; when all fit,
        // the ancestors just occupy their own width.
        const float view_right = out.max_scroll_px > 0.0f ? pane_bounds.right : pane_bounds.left + content;
        out.viewport = D2D1::RectF(pane_bounds.left, top, view_right, bottom);
        float x = strip_left;
        out.columns.reserve(ancestor_count + 1);
        for (size_t i = 0; i < ancestor_count; ++i) {
            const float w = ColumnStripWidthDip(widths_dip, ancestor_count - i) * unit;
            if (x + w > out.viewport.left && x < out.viewport.right) {
                out.columns.push_back({static_cast<int>(i), D2D1::RectF(x, top, x + w, bottom)});
            }
            x += w;
        }
    }
    if (trail_w > 0.0f) {
        out.columns.push_back({kColumnStripChildId,
                               D2D1::RectF(out.body.right, top, out.body.right + trail_w, bottom)});
    }
    return out;
}

ColumnStripNavLayout LayoutColumnStripNav(const ColumnStripLayout& layout, float scale) noexcept {
    ColumnStripNavLayout out;
    if (!layout.active || layout.max_scroll_px <= 0.0f) return out;
    const float unit = std::max(0.5f, scale);
    const float d = kColumnStripNavDiameterDip * unit;
    const float inset = kColumnStripNavInsetDip * unit;
    // The strip overflows, so the viewport is the whole pane: one button at
    // each edge. Both must fit side by side; a too small area keeps the
    // scrollbar and the wheel only.
    const D2D1_RECT_F& vp = layout.viewport;
    if (vp.right - vp.left < 2.0f * d + 3.0f * inset || vp.bottom - vp.top < d + 2.0f * inset) return out;
    const float cy = (vp.top + vp.bottom) * 0.5f;
    const float top = cy - d * 0.5f;
    const float eps = 0.5f;
    out.left = layout.scroll_px > eps;
    out.right = layout.scroll_px < layout.max_scroll_px - eps;
    out.left_rect = D2D1::RectF(vp.left + inset, top, vp.left + inset + d, top + d);
    out.right_rect = D2D1::RectF(vp.right - inset - d, top, vp.right - inset, top + d);
    return out;
}

float StepColumnStripScroll(const ColumnStripLayout& layout, size_t ancestor_count,
                            const std::vector<float>& widths_dip, float scale,
                            float scroll_px, int direction) noexcept {
    const float unit = std::max(0.5f, scale);
    const float max_px = std::max(0.0f, layout.max_scroll_px);
    const float min_step = 32.0f * unit;
    float target = std::clamp(scroll_px, 0.0f, max_px);
    // Left edges of the columns, counted from the left end of the strip:
    // 0, w0, w0 + w1, ... then the list and the child column.
    std::vector<float> edges;
    edges.reserve(ancestor_count + 2);
    float edge = 0.0f;
    for (size_t i = 0; i < ancestor_count; ++i) {
        edges.push_back(edge);
        edge += ColumnStripWidthDip(widths_dip, ancestor_count - i) * unit;
    }
    edges.push_back(edge);
    edge += std::max(0.0f, layout.body.right - layout.body.left);
    if (!layout.columns.empty() && layout.columns.back().id == kColumnStripChildId) edges.push_back(edge);
    if (direction < 0) {
        target = 0.0f;
        for (const float e : edges) {
            if (e >= scroll_px - min_step) break;
            target = e;
        }
    } else if (direction > 0) {
        target = max_px;
        for (const float e : edges) {
            if (e > scroll_px + min_step) {
                target = std::min(e, max_px);
                break;
            }
        }
    }
    return std::clamp(target, 0.0f, max_px);
}

} // namespace pulse::ui
