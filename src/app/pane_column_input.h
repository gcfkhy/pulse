#pragma once
#include "app_model.h"
#include "../ui/ui_renderer.h"

namespace pulse::app {
inline void ResizePaneColumn(Tab& tab, const ui::PaneViewModel& view,
                             const ui::MainRenderer& renderer, const D2D1_RECT_F& bounds,
                             int divider, float cursor_x) {
    if (view.is_search)
        tab.search_column_dividers = renderer.ResizeSearchColumnDivider(
            bounds, tab.search_column_dividers, divider, cursor_x);
    else
        tab.details_column_dividers = renderer.ResizeDetailsColumnDivider(
            bounds, tab.details_column_dividers, divider, cursor_x, view.ExtraColumns());
}

inline void AutoFitPaneColumn(Tab& tab, const ui::PaneViewModel& view,
                              const ui::MainRenderer& renderer, const D2D1_RECT_F& bounds,
                              int divider) {
    renderer.AutoFitColumnDivider(bounds, tab.details_column_dividers,
                                   view.is_search, tab.search_column_dividers, divider,
                                   view.ExtraColumns());
}
}
