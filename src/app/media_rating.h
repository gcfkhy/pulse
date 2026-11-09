#pragma once
// 分级: rating the selected photos, videos and music with the keys 0-5 or the
// details panel's stars. Reading ratings belongs to the media reader
// (media_props: the 分级 column, its sort and the panel); this is the write
// side, on a worker of its own because a file's property handler opens the
// file and can be slow.
#include "media_folder.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse {
struct AppState;
namespace app {
struct Tab;

// The files Windows can write a star rating into: photos, videos and MP3 /
// M4A / WMA (WAV / FLAC / OGG have no writable property handler).
inline bool CanRateName(std::wstring_view name) {
    switch (media::KindOfName(name)) {
    case media::Kind::Photo:
    case media::Kind::Video:
        return true;
    case media::Kind::Music: {
        const size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring_view::npos || name.size() - dot > 5) return false;
        wchar_t ext[6]{};
        for (size_t i = dot; i < name.size(); ++i) {
            const wchar_t c = name[i];
            ext[i - dot] = c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
        }
        const std::wstring_view e(ext, name.size() - dot);
        return e == L".mp3" || e == L".m4a" || e == L".wma";
    }
    default:
        return false;
    }
}

// The selected files' full paths when every one of them can hold a rating;
// empty when the selection is empty or has a folder, a cloud placeholder or
// any other kind of file in it.
std::vector<std::wstring> RatableSelection(const Tab& tab);
// Keys 0-5 and the details panel's stars: rate the selected files (0 clears).
// False when the selection cannot be rated, so the key keeps its other meaning.
bool SetSelectedRating(AppState& state, uint8_t stars);
// Writes that failed since the last call (the window shows a toast).
uint32_t TakeRatingFailures();
// Ends the writer thread (window shutdown).
void StopRatingWriter();

} // namespace app
} // namespace pulse
