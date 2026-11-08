#include "../ui/column_strip_layout.h"
#include <cmath>
#include <cstdio>

int main() {
    using namespace pulse::ui;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    check(ColumnStripWidthSlot(2, 3) == 1 && ColumnStripWidthSlot(0, 3) == 3 &&
          ColumnStripWidthSlot(kColumnStripChildId, 3) == 0 &&
          ColumnStripWidthSlot(kColumnStripBodyId, 3) == kColumnStripBodySlot,
          "widths follow ancestor depth, child and current-folder identity");
    check(ColumnStripWidthDip({1.0f, 900.0f}, 0) == kColumnStripMinDip &&
          ColumnStripWidthDip({1.0f, 900.0f}, 1) == kColumnStripMaxDip, "stored widths respect limits");
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        for (float width : {200.0f, 360.0f, 640.0f, 1200.0f}) {
            const auto bounds = D2D1::RectF(10, 20, 10 + width * scale, 600 * scale);
            const auto layout = LayoutColumnStrip(bounds, 80 * scale, 12, true, {}, scale);
            check(layout.body.left >= bounds.left && layout.body.right <= bounds.right &&
                  layout.body.right >= layout.body.left, "current pane remains inside bounds across DPI and narrow widths");
            check(!layout.active || layout.body.right - layout.body.left >=
                      std::fmin(kColumnStripDefaultDip, width) * scale - .01f,
                  "ancestors and child preserve the current folder's column width");
            check(std::abs(layout.scroll_px - layout.max_scroll_px) < .01f,
                  "initial ancestor viewport stays beside immediate parent");
            const auto scrolled = LayoutColumnStrip(bounds, 80 * scale, 12, true, {}, scale, 100000);
            check(scrolled.scroll_px == 0, "horizontal overscroll clamps at oldest ancestor");
            const auto root = LayoutColumnStrip(bounds, 80 * scale, 0, false, {}, scale);
            check(root.active && root.body.left == bounds.left && root.columns.empty(),
                  "a root folder alone is one column at the left");
        }
    }

    // The whole strip scrolls as one: the ancestors, the list and the child
    // column all move by the same amount, and at rest the right end is the pane's.
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        const auto bounds = D2D1::RectF(10, 20, 10 + 1200.0f * scale, 600 * scale);
        const float top = 80 * scale;
        for (const bool child : {false, true}) {
            const auto rest = LayoutColumnStrip(bounds, top, 12, child, {}, scale);
            const float right_end = child ? rest.columns.back().rect.right : rest.body.right;
            check(rest.max_scroll_px > 0 && std::abs(right_end - bounds.right) < .01f &&
                      std::abs(rest.content_px - (rest.max_scroll_px + (bounds.right - bounds.left))) < .01f,
                  "at rest the strip's last column ends at the pane's right edge");
            const auto moved = LayoutColumnStrip(bounds, top, 12, child, {}, scale, 400);
            const float shift = moved.body.left - rest.body.left;
            check(std::abs(shift - 400.0f * scale) < .01f &&
                      std::abs((moved.body.right - moved.body.left) - (rest.body.right - rest.body.left)) < .01f,
                  "scrolling away from rest moves the current list by the scroll");
            bool ancestors_together = true;
            for (const auto& a : moved.columns) {
                for (const auto& b : rest.columns) {
                    if (a.id == b.id && std::abs((a.rect.left - b.rect.left) - shift) > .01f) ancestors_together = false;
                }
            }
            check(ancestors_together, "every visible ancestor and the child column move by the same amount");
            const auto oldest = LayoutColumnStrip(bounds, top, 12, child, {}, scale, 100000);
            check(oldest.scroll_px == 0.0f && std::abs(oldest.columns.front().rect.left - bounds.left) < .01f &&
                      oldest.columns.front().id == 0,
                  "at the oldest end the first ancestor starts at the pane's left edge");
            check(oldest.body.left > bounds.right - 1.0f * scale,
                  "scrolled to the oldest end the list is out of the pane, to its right");
            check(child ? oldest.columns.back().id == kColumnStripChildId : true,
                  "the child column stays in the layout even when scrolled out of the pane");
        }
    }
    // Everything fits: nothing scrolls and the columns line up from the left.
    {
        const auto bounds = D2D1::RectF(0, 0, 1200.0f, 600.0f);
        const auto fits = LayoutColumnStrip(bounds, 80.0f, 2, true, {}, 1.0f);
        check(fits.max_scroll_px == 0.0f && fits.scroll_px == 0.0f &&
                  std::abs(fits.body.left - 2 * kColumnStripDefaultDip) < .01f &&
                  std::abs(fits.viewport.right - fits.body.left) < .01f,
              "a strip that fits does not scroll and keeps the ancestors left of the list");
    }

    // Arrow buttons: shown only toward hidden columns, at the pane's left and
    // right edge, vertically centred, never overlapping each other.
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        const auto bounds = D2D1::RectF(10, 20, 10 + 1200.0f * scale, 600 * scale);
        const float top = 80 * scale;
        const auto at_parent = LayoutColumnStrip(bounds, top, 12, false, {}, scale);
        const auto nav_parent = LayoutColumnStripNav(at_parent, scale);
        check(nav_parent.left && !nav_parent.right,
              "strip resting beside the parent has an arrow only toward older ancestors");
        const auto at_oldest = LayoutColumnStrip(bounds, top, 12, false, {}, scale, 100000);
        const auto nav_oldest = LayoutColumnStripNav(at_oldest, scale);
        check(!nav_oldest.left && nav_oldest.right,
              "strip at the oldest ancestor has an arrow only toward the parent");
        const auto in_between = LayoutColumnStrip(bounds, top, 12, false, {}, scale, 300);
        const auto nav_mid = LayoutColumnStripNav(in_between, scale);
        check(nav_mid.left && nav_mid.right, "a strip scrolled part way has both arrows");
        const D2D1_RECT_F& vp = in_between.viewport;
        const float d = kColumnStripNavDiameterDip * scale;
        const float inset = kColumnStripNavInsetDip * scale;
        // One arrow at each edge of the pane, whatever column is there.
        check(std::abs(nav_mid.left_rect.left - (bounds.left + inset)) < .01f &&
                  std::abs(nav_mid.right_rect.right - (bounds.right - inset)) < .01f &&
                  nav_mid.left_rect.right <= nav_mid.right_rect.left &&
                  std::abs((nav_mid.left_rect.right - nav_mid.left_rect.left) - d) < .01f &&
                  std::abs((nav_mid.right_rect.right - nav_mid.right_rect.left) - d) < .01f &&
                  std::abs((nav_mid.left_rect.top + nav_mid.left_rect.bottom) * 0.5f -
                           (vp.top + vp.bottom) * 0.5f) < .01f &&
                  nav_mid.left_rect.top == nav_mid.right_rect.top,
              "arrows sit at the pane's left and right edge, vertically centred, equal and apart");
        const auto with_child = LayoutColumnStrip(bounds, top, 12, true, {}, scale, 300);
        const auto nav_child = LayoutColumnStripNav(with_child, scale);
        check(nav_child.left && nav_child.right &&
                  std::abs(nav_child.right_rect.right - (bounds.right - inset)) < .01f &&
                  std::abs(nav_child.left_rect.left - (bounds.left + inset)) < .01f,
              "the arrows stay at the pane's edges with a child column too");
        const auto few = LayoutColumnStrip(bounds, top, 2, false, {}, scale);
        const auto nav_few = LayoutColumnStripNav(few, scale);
        check(!nav_few.left && !nav_few.right, "ancestors that all fit need no arrows");
        const auto none = LayoutColumnStrip(bounds, top, 0, false, {}, scale);
        check(!LayoutColumnStripNav(none, scale).left && !LayoutColumnStripNav(none, scale).right,
              "a root folder has no arrows");
        check(!LayoutColumnStripNav(ColumnStripLayout{}, scale).left, "an inactive strip has no arrows");
        // A narrow pane: the list and the child column fill it, yet the ancestors
        // are one scroll away, so the arrow toward them is there.
        const auto narrow_bounds = D2D1::RectF(0, 0, 2 * kColumnStripDefaultDip * scale, 600 * scale);
        const auto narrow = LayoutColumnStrip(narrow_bounds, top, 12, true, {}, scale);
        const auto nav_narrow = LayoutColumnStripNav(narrow, scale);
        check(narrow.max_scroll_px > 0 && narrow.columns.size() == 1 && nav_narrow.left && !nav_narrow.right &&
                  nav_narrow.left_rect.right <= narrow_bounds.right,
              "with the list and child filling a narrow pane the arrow toward the ancestors shows");
        const auto narrow_oldest = LayoutColumnStrip(narrow_bounds, top, 12, true, {}, scale, 100000);
        check(narrow_oldest.scroll_px == 0.0f && narrow_oldest.columns.front().id == 0 &&
                  LayoutColumnStripNav(narrow_oldest, scale).right,
              "the ancestors of a narrow pane can be scrolled into view");
    }

    // One column per step, to the ends of the strip.
    for (float scale : {1.0f, 1.25f, 2.0f}) {
        const auto bounds = D2D1::RectF(0, 0, 1000.0f * scale, 600 * scale);
        const float top = 80 * scale;
        const std::vector<float> widths{0.0f, 0.0f, 180.0f, 0.0f, 300.0f};
        const size_t count = 9;
        const auto rest = LayoutColumnStrip(bounds, top, count, false, widths, scale);
        float at = rest.scroll_px;
        int steps = 0;
        bool strictly_left = true, on_edges = true;
        while (at > 0.0f && steps < 40) {
            const float next = StepColumnStripScroll(rest, count, widths, scale, at, kColumnStripNavLeft);
            strictly_left = strictly_left && next < at;
            // Every stop but the last is a column edge.
            float edge = 0.0f;
            bool is_edge = next == 0.0f;
            for (size_t i = 0; i < count && !is_edge; ++i) {
                is_edge = std::abs(edge - next) < .01f;
                edge += ColumnStripWidthDip(widths, count - i) * scale;
            }
            on_edges = on_edges && is_edge;
            at = next;
            ++steps;
        }
        check(at == 0.0f && steps > 1 && steps <= static_cast<int>(count) + 1 && strictly_left && on_edges,
              "stepping left reaches the oldest ancestor over column edges");
        check(StepColumnStripScroll(rest, count, widths, scale, 0.0f, kColumnStripNavLeft) == 0.0f,
              "stepping left at the oldest ancestor stays put");
        int back = 0;
        at = 0.0f;
        bool strictly_right = true;
        while (at < rest.max_scroll_px && back < 40) {
            const float next = StepColumnStripScroll(rest, count, widths, scale, at, kColumnStripNavRight);
            strictly_right = strictly_right && next > at;
            at = next;
            ++back;
        }
        check(std::abs(at - rest.max_scroll_px) < .01f && back > 1 && strictly_right,
              "stepping right returns to the parent end of the strip");
        check(StepColumnStripScroll(rest, count, widths, scale, rest.max_scroll_px, kColumnStripNavRight) ==
                  rest.max_scroll_px,
              "stepping right beside the parent stays put");
        // A strip dragged to a point just past an edge still travels visibly.
        float edge_two = 0.0f;
        for (size_t i = 0; i < 2; ++i) edge_two += ColumnStripWidthDip(widths, count - i) * scale;
        const float near_edge = edge_two + 4.0f * scale;
        check(near_edge <= rest.max_scroll_px &&
                  near_edge - StepColumnStripScroll(rest, count, widths, scale, near_edge, kColumnStripNavLeft) >=
                      32.0f * scale - .01f,
              "a step is never a barely visible nudge");
    }
    return failures ? 1 : 0;
}
