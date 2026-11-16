#pragma once
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {
// The toolbar's view controls. The index is the control's identity in the
// layout (ui::kToolbarViewControls of them); the saved order lists the ids.
inline constexpr std::array<std::wstring_view, 4> kToolbarViewIds{L"sort", L"filter", L"group", L"view"};
inline int ToolbarViewIndex(std::wstring_view id) {
    for (size_t i = 0; i < kToolbarViewIds.size(); ++i)
        if (id == kToolbarViewIds[i]) return static_cast<int>(i);
    return -1;
}
// Unknown and repeated ids are dropped; a control missing from the list is
// added. 视图 was added after the order was first saved and belongs before
// 排序, so it joins at the front; the others keep joining at the end.
inline std::vector<std::wstring> NormalizeToolbarViewOrder(const std::vector<std::wstring>& ids) {
    std::vector<std::wstring> result;
    std::array<bool, kToolbarViewIds.size()> placed{};
    for (const auto& id : ids) {
        const int index = ToolbarViewIndex(id);
        if (index >= 0 && !placed[index]) { result.push_back(id); placed[index] = true; }
    }
    for (size_t i = 0; i < placed.size(); ++i) {
        if (placed[i]) continue;
        if (kToolbarViewIds[i] == L"view") result.insert(result.begin(), std::wstring(kToolbarViewIds[i]));
        else result.emplace_back(kToolbarViewIds[i]);
    }
    return result;
}
inline std::array<int, kToolbarViewIds.size()> ToolbarViewLayoutOrder(const std::vector<std::wstring>& ids) {
    std::array<int, kToolbarViewIds.size()> result{};
    size_t count = 0;
    for (const auto& id : NormalizeToolbarViewOrder(ids)) result[count++] = ToolbarViewIndex(id);
    return result;
}
inline std::vector<std::wstring> ReorderVisibleToolbarViews(const std::vector<std::wstring>& current,
                                                           const std::vector<std::wstring>& shown) {
    auto result = NormalizeToolbarViewOrder(current);
    std::vector<std::wstring> ordered;
    std::array<bool, kToolbarViewIds.size()> present{};
    for (const auto& id : shown) {
        const int index = ToolbarViewIndex(id);
        if (index >= 0 && !present[index]) { ordered.push_back(id); present[index] = true; }
    }
    size_t next = 0;
    for (auto& id : result)
        if (present[ToolbarViewIndex(id)]) id = ordered[next++];
    return result;
}
}
