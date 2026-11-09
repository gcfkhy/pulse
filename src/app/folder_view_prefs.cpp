#include "folder_view_prefs.h"
#include "../common/json_utils.h"
#include "../common/path_utils.h"
#include "../common/preview_extensions.h"
#include <algorithm>
#include <string>

namespace pulse::app {

bool MostlyPictures(size_t files, size_t pictures) {
    return pictures >= 6 && pictures <= files && pictures * 10 >= files * 6;
}

std::wstring FolderPrefKey(const std::wstring& path) {
    if (path.empty() || path.starts_with(L"pulse:")) return {};
    std::wstring key = path::StripExtendedPathPrefix(path);
    std::replace(key.begin(), key.end(), L'/', L'\\');
    // Accept only absolute filesystem paths; virtual views have separate rules.
    if (!(key.size() >= 3 && key[1] == L':' && key[2] == L'\\') &&
        !key.starts_with(L"\\\\")) return {};
    while (key.size() > 3 && key.back() == L'\\') key.pop_back();
    return key;
}

namespace {

std::wstring JsonKey(ui::ViewMode mode) {
    return std::wstring(L"folder_view_") + ui::ViewModeName(mode);
}

// Icon sizes: "<dip>|<path>" strings, This PC as an empty path.
constexpr wchar_t kIconDipKey[] = L"folder_icon_dip";
// Photo walls: the folders' paths, This PC as an empty one.
constexpr wchar_t kWallKey[] = L"folder_wall";
// Last icon views: "<mode index>|<path>" strings, This PC as an empty path;
// the global one as two ints. Older builds ignore all three keys.
constexpr wchar_t kIconModeKey[] = L"folder_icon_mode";
constexpr wchar_t kLastIconModeKey[] = L"folder_icon_last_mode";
constexpr wchar_t kLastIconDipKey[] = L"folder_icon_last_dip";

int StoredDip(float dip) {
    return dip > 0.0f ? static_cast<int>(std::clamp(dip, ui::kMinIconDip, 512.0f) + 0.5f) : 0;
}

} // namespace

bool FolderViewPrefs::PathLess::operator()(const std::wstring& a, const std::wstring& b) const {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
}

std::optional<ui::ViewMode> FolderViewPrefs::Find(const std::wstring& path) const {
    if (path.empty()) return this_pc_;
    const auto key = FolderPrefKey(path);
    if (key.empty()) return std::nullopt;
    const auto it = views_.find(key);
    return it == views_.end() ? std::nullopt : std::optional(it->second);
}

bool FolderViewPrefs::Set(const std::wstring& path, ui::ViewMode mode) {
    if (static_cast<unsigned>(mode) >= 8) return false;
    if (path.empty()) {
        if (this_pc_ == mode) return false;
        this_pc_ = mode;
        return true;
    }
    const auto key = FolderPrefKey(path);
    if (key.empty()) return false;
    const auto [it, inserted] = views_.try_emplace(key, mode);
    if (!inserted && it->second == mode) return false;
    it->second = mode;
    return true;
}

std::optional<float> FolderViewPrefs::FindIconDip(const std::wstring& path) const {
    const auto key = path.empty() ? std::wstring() : FolderPrefKey(path);
    if (key.empty() && !path.empty()) return std::nullopt;
    const auto it = icon_dips_.find(key);
    if (it == icon_dips_.end()) return std::nullopt;
    return static_cast<float>(it->second);
}

bool FolderViewPrefs::SetIconDip(const std::wstring& path, float dip) {
    const auto key = path.empty() ? std::wstring() : FolderPrefKey(path);
    if (key.empty() && !path.empty()) return false;
    const auto it = icon_dips_.find(key);
    if (!(dip > 0.0f)) {
        if (it == icon_dips_.end()) return false;
        icon_dips_.erase(it);
        return true;
    }
    const int value = static_cast<int>(std::clamp(dip, ui::kMinIconDip, 512.0f) + 0.5f);
    if (it != icon_dips_.end() && it->second == value) return false;
    icon_dips_[key] = value;
    return true;
}

bool FolderViewPrefs::FindWall(const std::wstring& path) const {
    const auto key = path.empty() ? std::wstring() : FolderPrefKey(path);
    if (key.empty() && !path.empty()) return false;
    return walls_.contains(key);
}

bool FolderViewPrefs::SetWall(const std::wstring& path, bool wall) {
    const auto key = path.empty() ? std::wstring() : FolderPrefKey(path);
    if (key.empty() && !path.empty()) return false;
    if (!wall) return walls_.erase(key) > 0;
    return walls_.try_emplace(key, true).second;
}

std::optional<FolderViewPrefs::LastIcon> FolderViewPrefs::FindLastIcon(const std::wstring& path) const {
    const auto key = path.empty() ? std::wstring() : FolderPrefKey(path);
    if (!key.empty() || path.empty()) {
        const auto it = icon_modes_.find(key);
        if (it != icon_modes_.end()) return LastIcon{it->second, FindIconDip(path).value_or(0.0f)};
    }
    return last_icon_;
}

bool FolderViewPrefs::SetLastIcon(const std::wstring& path, ui::ViewMode mode, float dip) {
    if (!ui::IsZoomIconMode(mode)) return false;
    bool changed = false;
    const LastIcon last{mode, static_cast<float>(StoredDip(dip))};
    if (!last_icon_ || last_icon_->mode != last.mode || last_icon_->dip != last.dip) {
        last_icon_ = last;
        changed = true;
    }
    const auto key = path.empty() ? std::wstring() : FolderPrefKey(path);
    if (key.empty() && !path.empty()) return changed;
    const auto [it, inserted] = icon_modes_.try_emplace(key, mode);
    if (inserted || it->second != mode) {
        it->second = mode;
        changed = true;
    }
    return changed;
}

void FolderViewPrefs::ApplyToAll(ui::ViewMode mode) {
    views_.clear();
    std::erase_if(icon_dips_, [](const auto& item) { return !item.first.empty(); });
    std::erase_if(walls_, [](const auto& item) { return !item.first.empty(); });
    std::erase_if(icon_modes_, [](const auto& item) { return !item.first.empty(); });
    default_ = ui::ViewModeFromIndex(ui::ViewModeIndex(mode));
}

void FolderViewPrefs::AppendJson(std::wstring& out) const {
    out += L",\n  \"folder_view_default\":" + std::to_wstring(ui::ViewModeIndex(default_));
    out += L",\n  \"folder_view_this_pc\":" +
           std::to_wstring(this_pc_ ? ui::ViewModeIndex(*this_pc_) : -1);
    // Group paths by stable view names, using the existing string-array codec.
    for (int i = 0; i < 8; ++i) {
        const auto mode = ui::ViewModeFromIndex(i);
        out += L",\n  \"" + JsonKey(mode) + L"\":[";
        bool first = true;
        for (const auto& [path, saved_mode] : views_) {
            if (saved_mode != mode) continue;
            if (!first) out += L",";
            first = false;
            out += L"\"";
            json::Escape(path, out);
            out += L"\"";
        }
        out += L"]";
    }
    out += L",\n  \"";
    out += kIconDipKey;
    out += L"\":[";
    bool first_dip = true;
    for (const auto& [path, dip] : icon_dips_) {
        if (!first_dip) out += L",";
        first_dip = false;
        out += L"\"";
        json::Escape(std::to_wstring(dip) + L"|" + path, out);
        out += L"\"";
    }
    out += L"]";
    out += L",\n  \"";
    out += kWallKey;
    out += L"\":[";
    bool first_wall = true;
    for (const auto& item : walls_) {
        if (!first_wall) out += L",";
        first_wall = false;
        out += L"\"";
        json::Escape(item.first, out);
        out += L"\"";
    }
    out += L"]";
    out += L",\n  \"";
    out += kIconModeKey;
    out += L"\":[";
    bool first_mode = true;
    for (const auto& [path, mode] : icon_modes_) {
        if (!first_mode) out += L",";
        first_mode = false;
        out += L"\"";
        json::Escape(std::to_wstring(ui::ViewModeIndex(mode)) + L"|" + path, out);
        out += L"\"";
    }
    out += L"]";
    out += L",\n  \"";
    out += kLastIconModeKey;
    out += L"\":" + std::to_wstring(last_icon_ ? ui::ViewModeIndex(last_icon_->mode) : -1);
    out += L",\n  \"";
    out += kLastIconDipKey;
    out += L"\":" + std::to_wstring(last_icon_ ? StoredDip(last_icon_->dip) : 0);
}

void FolderViewPrefs::ReadJson(const std::wstring& input) {
    Clear();
    default_ = ui::ViewModeFromIndex(json::ExtractInt(
        input, L"folder_view_default", ui::ViewModeIndex(ui::ViewMode::Details)));
    const int this_pc = json::ExtractInt(input, L"folder_view_this_pc", -1);
    if (this_pc >= 0 && this_pc < 8) this_pc_ = ui::ViewModeFromIndex(this_pc);
    for (int i = 0; i < 8; ++i) {
        const auto mode = ui::ViewModeFromIndex(i);
        for (const auto& path : json::ExtractStringArray(input, JsonKey(mode)))
            Set(path, mode);
    }
    for (const auto& item : json::ExtractStringArray(input, kIconDipKey)) {
        const size_t bar = item.find(L'|');
        if (bar == 0 || bar > 3) continue;  // also npos
        int dip = 0;
        bool digits = true;
        for (size_t i = 0; i < bar; ++i) {
            digits = digits && item[i] >= L'0' && item[i] <= L'9';
            dip = dip * 10 + (item[i] - L'0');
        }
        if (digits) SetIconDip(item.substr(bar + 1), static_cast<float>(dip));
    }
    for (const auto& path : json::ExtractStringArray(input, kWallKey)) SetWall(path, true);
    for (const auto& item : json::ExtractStringArray(input, kIconModeKey)) {
        if (item.size() < 2 || item[1] != L'|' || item[0] < L'0' || item[0] > L'7') continue;
        const auto mode = ui::ViewModeFromIndex(item[0] - L'0');
        const std::wstring path = item.substr(2);
        const auto key = path.empty() ? std::wstring() : FolderPrefKey(path);
        if (ui::IsZoomIconMode(mode) && (!key.empty() || path.empty())) icon_modes_[key] = mode;
    }
    const int last_mode = json::ExtractInt(input, kLastIconModeKey, -1);
    if (last_mode >= 0 && last_mode < 8 && ui::IsZoomIconMode(ui::ViewModeFromIndex(last_mode))) {
        const int dip = json::ExtractInt(input, kLastIconDipKey, 0);
        last_icon_ = LastIcon{ui::ViewModeFromIndex(last_mode), static_cast<float>(StoredDip(static_cast<float>(dip)))};
    }
}

} // namespace pulse::app