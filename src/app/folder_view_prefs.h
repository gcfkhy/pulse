#pragma once
#include "../common/preview_extensions.h"
#include <algorithm>
#include <iterator>

#include "../ui/view_layout.h"
#include <map>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace pulse::app {

// Normalized key for per-folder preferences; empty for virtual views and
// nonabsolute paths, which are never persisted.
std::wstring FolderPrefKey(const std::wstring& path);

// An icon view to return to; dip 0 = the mode's preset.
struct FolderLastIcon {
    ui::ViewMode mode = ui::ViewMode::MediumIcons;
    float dip = 0.0f;
};

// #58-7b: a folder never given a view opens in Large icons when its files
// are mostly pictures: at least six, and 60% or more of the files (folders
// do not count). Judged by extension only, from the listing already loaded.
inline bool IsPictureName(std::wstring_view name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0 || name.size() - dot > 6) return false;
    wchar_t buffer[8]{};
    const size_t length = name.size() - dot;
    for (size_t i = 0; i < length; ++i) {
        const wchar_t c = name[dot + i];
        buffer[i] = c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    }
    const std::wstring_view ext(buffer, length);
    if (preview::IsImageExtension(ext)) return true;
    // Camera raw files: what photo folders are full of.
    static constexpr std::wstring_view kRaw[] = {L".dng", L".cr2", L".cr3", L".nef", L".arw",
                                                 L".orf", L".rw2", L".raf", L".pef", L".srw"};
    return std::find(std::begin(kRaw), std::end(kRaw), ext) != std::end(kRaw);
}

bool MostlyPictures(size_t files, size_t pictures);

// This PC (the empty path) has no folder key but keeps its own choice, like
// File Explorer; other virtual views are still never remembered.
class FolderViewPrefs {
public:
    std::optional<ui::ViewMode> Find(const std::wstring& path) const;
    bool Set(const std::wstring& path, ui::ViewMode mode);
    // Continuous icon size (DIP) chosen in a folder or This PC; none = the
    // mode's preset. dip 0 forgets it. Kept apart from the modes so older
    // builds read the same file.
    std::optional<float> FindIconDip(const std::wstring& path) const;
    bool SetIconDip(const std::wstring& path, float dip);
    // Photo wall (the header's wall button) chosen in a folder or This PC;
    // stored apart from the modes like the sizes.
    bool FindWall(const std::wstring& path) const;
    bool SetWall(const std::wstring& path, bool wall);
    // The icon view to return to from Details / List (the header's icons
    // button, Ctrl+wheel up): the last one chosen in this folder, else the
    // last one chosen anywhere.
    using LastIcon = FolderLastIcon;
    std::optional<LastIcon> FindLastIcon(const std::wstring& path) const;
    // Records an icon view chosen in `path` (also the fallback for folders
    // without one). Non-icon modes are ignored.
    bool SetLastIcon(const std::wstring& path, ui::ViewMode mode, float dip);
    // Mode for folders without a saved choice.
    ui::ViewMode Default() const { return default_; }
    // "Apply to all folders": `mode` becomes the default, per-folder choices go.
    // This PC is not a folder, so its own choice stays.
    void ApplyToAll(ui::ViewMode mode);
    void Clear() {
        views_.clear();
        icon_dips_.clear();
        walls_.clear();
        icon_modes_.clear();
        last_icon_.reset();
        this_pc_.reset();
        default_ = ui::ViewMode::Details;
    }
    void AppendJson(std::wstring& out) const;
    void ReadJson(const std::wstring& json);

private:
    struct PathLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const;
    };
    std::map<std::wstring, ui::ViewMode, PathLess> views_;
    std::map<std::wstring, int, PathLess> icon_dips_;  // "" = This PC
    std::map<std::wstring, bool, PathLess> walls_;     // "" = This PC; true only
    std::map<std::wstring, ui::ViewMode, PathLess> icon_modes_;  // "" = This PC
    std::optional<LastIcon> last_icon_;
    std::optional<ui::ViewMode> this_pc_;
    ui::ViewMode default_ = ui::ViewMode::Details;
};

} // namespace pulse::app