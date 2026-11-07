// details_column_set.h — Which optional Details columns are shown (#26, #36).
//
// One global choice (AppPrefs::details_columns), edited from the column
// header's right-click menu. Bit = 1 << MainRenderer::ColumnKind value; Name
// is always shown and Path belongs to search views, so neither has a bit.
// Status (cloud files) is chosen here too, but only cloud folders draw it.
#pragma once
#include <array>
#include <cstdint>

namespace pulse::ui {

inline constexpr uint32_t kDetailsColumnModified = 1u << 2;
inline constexpr uint32_t kDetailsColumnType = 1u << 3;
inline constexpr uint32_t kDetailsColumnSize = 1u << 4;
inline constexpr uint32_t kDetailsColumnCreated = 1u << 5;
inline constexpr uint32_t kDetailsColumnAccessed = 1u << 6;
inline constexpr uint32_t kDetailsColumnStatus = 1u << 7;
// Photo / video folders only (分级, 时长); off unless ticked, or while the list
// is sorted by that column.
inline constexpr uint32_t kDetailsColumnRating = 1u << 8;
inline constexpr uint32_t kDetailsColumnDuration = 1u << 9;
inline constexpr uint32_t kDetailsColumnsMedia = kDetailsColumnRating | kDetailsColumnDuration;

inline constexpr uint32_t kDetailsColumnsDefault =
    kDetailsColumnModified | kDetailsColumnType | kDetailsColumnSize | kDetailsColumnStatus;
inline constexpr uint32_t kDetailsColumnsAll = kDetailsColumnsDefault |
    kDetailsColumnCreated | kDetailsColumnAccessed | kDetailsColumnsMedia;

constexpr uint32_t NormalizeDetailsColumns(uint32_t mask) noexcept {
    return mask & kDetailsColumnsAll;
}

// What one pane adds to the global choice above: the cloud 状态 column, which
// media columns the folder offers, and which of those are shown whatever the
// choice (the sort key must stay visible).
struct DetailsExtraColumns {
    bool status = false;
    uint32_t media = 0;
    uint32_t forced = 0;
};

// Manual widths (DIP) of the folder-view metadata columns, by slot:
// 0 modified, 1 type, 2 size, 3 created, 4 accessed. 0 = automatic.
using DetailsColumnWidths = std::array<float, 5>;

} // namespace pulse::ui
