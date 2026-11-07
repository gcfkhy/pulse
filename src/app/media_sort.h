#pragma once
#include "../fs/fs_enum.h"
#include "../fs/fs_snapshot.h"
#include "../ui/ui_view_model.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace pulse {
struct AppState;
namespace app {
struct Tab;

struct MediaSortRequest {
    std::wstring path;
    uint64_t modified = 0;
    uint64_t size = 0;
};

struct MediaSortValue {
    uint32_t duration_ms = 0;
    uint8_t rating = 0; // 0 unrated, 1-5
};

// Image, camera raw, or video: folders of these get Rating / Duration sorts.
bool IsMediaSortName(std::wstring_view name);
bool EntriesHaveMedia(const std::vector<fs::DirEntry>& entries, size_t limit = 400);
// Photos, videos and audio files have a playing time / rating to read.
bool HasMediaProperties(std::wstring_view name);
// The ones Windows can write a star rating into: photos, videos, MP3 / M4A / WMA.
bool CanRateName(std::wstring_view name);

class MediaSortCache {
public:
    MediaSortCache();
    ~MediaSortCache();
    MediaSortCache(const MediaSortCache&) = delete;
    MediaSortCache& operator=(const MediaSortCache&) = delete;

    void SetChangeNotify(HWND hwnd, UINT message);
    bool TakeChanged();
    uint64_t Revision() const;
    // Queue property reads. Already-known files with the same mtime/size skip
    // the worker. True when at least one requested file is already ready.
    bool Request(const std::vector<MediaSortRequest>& files);
    void Apply(std::vector<fs::DirEntry>& entries, const std::wstring& parent) const;
    // The values already read for `path` (whatever its mtime was), if any.
    bool Lookup(const std::wstring& path, MediaSortValue& out) const;
    // Write 1-5 stars, or 0 to clear, on the worker thread (property handlers
    // open the file). A written file is read back, so Lookup / Apply show what
    // the file really holds.
    void SetRating(std::vector<std::wstring> paths, uint8_t stars);
    // Writes that failed since the last call.
    uint32_t TakeRatingFailures();
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Queue property reads for listings sorted by Rating or Duration.
bool RequestMediaSort(AppState& state);
// Re-sort those listings as properties arrive. True while a listing still waits.
bool ResortForMediaSort(AppState& state);
// Stamp cached rating/duration onto a listing. Re-sorts when `resort` is true
// (a fresh listing); a keep-order refresh only fills the extra fields.
fs::SnapshotPtr WithMediaSortApplied(AppState& state, Tab& tab, fs::SnapshotPtr listing, bool resort);

// The selected files' full paths when every one of them can hold a rating;
// empty when the selection is empty or has a folder, a cloud placeholder or
// any other kind of file in it.
std::vector<std::wstring> RatableSelection(const Tab& tab);
// Keys 0-5 and the details panel's stars: rate the selected files (0 clears).
// False when the selection cannot be rated, so the key keeps its other meaning.
bool SetSelectedRating(AppState& state, uint8_t stars);

} // namespace app
} // namespace pulse
