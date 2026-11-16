#include "../app/toolbar_view_order.h"
#include "../ui/toolbar_layout.h"
#include <cstdio>
#include <cmath>

int main() {
    using namespace pulse;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) { std::printf("[FAIL] %s\n", label); ++failures; }
    };
    static_assert(app::kToolbarViewIds.size() == ui::kToolbarViewControls, "one id per view control");
    using Ids = std::vector<std::wstring>;
    check(app::NormalizeToolbarViewOrder({}) == Ids{L"view", L"sort", L"filter", L"group"},
        "older preferences start with 视图 before 排序");
    check(app::NormalizeToolbarViewOrder({L"sort", L"filter", L"group"}) == Ids{L"view", L"sort", L"filter", L"group"},
        "a saved three-control order gains 视图 at the front");
    check(app::NormalizeToolbarViewOrder({L"group", L"invalid", L"group", L"filter"}) ==
        Ids{L"view", L"group", L"filter", L"sort"}, "unknown duplicates and missing ids normalize");
    check(app::NormalizeToolbarViewOrder({L"sort", L"filter", L"group", L"view"}) ==
        Ids{L"sort", L"filter", L"group", L"view"}, "a saved 视图 position is kept");
    check(app::ReorderVisibleToolbarViews({L"view", L"sort", L"filter", L"group"}, {L"filter", L"group"}) ==
        Ids{L"view", L"sort", L"filter", L"group"}, "hiding a control preserves its saved position");
    check(app::ReorderVisibleToolbarViews({L"view", L"sort", L"filter", L"group"}, {L"group", L"filter"}) ==
        Ids{L"view", L"sort", L"group", L"filter"}, "visible drag reorders around hidden saved slot");
    check(app::ReorderVisibleToolbarViews({L"view", L"sort", L"filter", L"group"}, {L"sort", L"view"}) ==
        Ids{L"sort", L"view", L"filter", L"group"}, "视图 reorders with the others");
    std::array<int, 4> permutation{0, 1, 2, 3};
    size_t cases = 0;
    do {
        Ids ids;
        for (int i : permutation) ids.emplace_back(app::kToolbarViewIds[i]);
        check(app::ToolbarViewLayoutOrder(ids) == permutation, "saved ids preserve requested order");
        for (float scale : {1.0f, 1.5f, 2.0f}) for (float width : {480.0f, 900.0f, 1400.0f})
        for (int mask = 0; mask < 16; ++mask) for (float expanded : {0.0f, 1.0f}) {
            ui::ToolbarPinOptions pins;
            pins.view_order = app::ToolbarViewLayoutOrder(ids);
            const auto layout = ui::MakeToolbarLayout(width*scale, scale, 0, 12*scale, 80*scale, 0,
                expanded, 0, mask & 4 ? 90*scale : 0, mask & 1 ? 90*scale : -1,
                mask & 2 ? 90*scale : -1, 0, ui::SelectionChipSpan(scale, 80*scale, 4), pins, true,
                mask & 8 ? 90*scale : -1);
            const std::array<D2D1_RECT_F, 4> rects{layout.sort, layout.filter, layout.group, layout.view};
            float previous_right = -1;
            for (int i : permutation) {
                const auto& rect = rects[i];
                check(std::isfinite(rect.left) && rect.right >= rect.left, "finite nonnegative rectangles");
                if (rect.right <= rect.left) continue;
                check(previous_right < 0 || rect.left >= previous_right + 5.9f*scale, "visible controls honor saved order without overlap");
                previous_right = rect.right;
            }
            check((layout.sort.right > layout.sort.left) == !!(mask & 1), "hidden sort has no target");
            check((layout.filter.right > layout.filter.left) == (!!(mask & 2) || expanded > 0), "active filter remains available");
            check((layout.group.right > layout.group.left) == !!(mask & 4), "hidden group has no target");
            // Hidden by the user, or stepping aside for an open filter in the narrow tier.
            const bool view_expected = !!(mask & 8) && !(expanded > 0 && width < 540);
            check((layout.view.right > layout.view.left) == view_expected, "hidden view has no target");
            const auto chip = ui::MakeToolbarSelectionChip(layout, scale, 80*scale, 4);
            if (chip.chip.right > chip.chip.left) {
                float first = 1e9f;
                for (const auto& rect : rects)
                    if (rect.right > rect.left) first = std::min(first, rect.left);
                check(chip.chip.right <= first - 9.9f*scale, "selection chip stops before first reordered control");
            }
            ++cases;
        }
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    // The default arrangement: 视图 leads, then 排序, 筛选 and 分组.
    for (float scale : {1.0f, 1.5f}) {
        ui::ToolbarPinOptions pins;
        const auto layout = ui::MakeToolbarLayout(1400*scale, scale, 0, 12*scale, 80*scale, 0, 0, 0, 90*scale,
            90*scale, 90*scale, 0, 0, pins, true, 90*scale);
        check(layout.view.right > layout.view.left && layout.view.right < layout.sort.left &&
              layout.sort.right < layout.filter.left && layout.filter.right < layout.group.left,
            "default toolbar order is 视图, 排序, 筛选, 分组");
        // Without room for the labels the buttons shrink to icons together, 视图 included.
        const auto tight = ui::MakeToolbarLayout(760*scale, scale, 0, 12*scale, 80*scale, 0, 0, 0, 90*scale,
            90*scale, 90*scale, 0, 0, pins, true, 90*scale);
        check(tight.view.right - tight.view.left <= 90*scale + 0.01f && tight.view.right > tight.view.left &&
              tight.group.right <= tight.overflow.left,
            "视图 folds with the other buttons and stays clear of More");
    }
    std::printf("[%s] toolbar order: %zu layout cases, failures=%d\n", failures ? "FAIL" : "PASS", cases, failures);
    return failures ? 1 : 0;
}
