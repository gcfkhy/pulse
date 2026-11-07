#include "media_sort.h"
#include "app_state.h"
#include "app_messages.h"
#include "app_runtime.h"
#include "entry_group.h"
#include "entry_sort.h"
#include "folder_view_prefs.h"
#include "../common/path_utils.h"
#include "../common/preview_extensions.h"
#include "../fs/fs_enum.h"
#include <objbase.h>
#include <propsys.h>
#include <propkey.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#pragma comment(lib, "propsys.lib")

using Microsoft::WRL::ComPtr;

namespace pulse::app {
namespace {

constexpr size_t kRequestLimit = 2048;
constexpr size_t kLiveResortLimit = 20000;
constexpr uint64_t kResortIntervalMs = 750;
constexpr size_t kNotifyBatch = 32;

std::wstring ChildPath(const std::wstring& parent, const std::wstring& name) {
    return !parent.empty() && parent.back() == L'\\' ? parent + name : parent + L"\\" + name;
}

uint64_t FileTimeTicks(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::wstring LowerExt(std::wstring_view name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0 || name.size() - dot > 8) return {};
    std::wstring ext(name.substr(dot));
    for (wchar_t& c : ext)
        c = c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    return ext;
}

uint8_t StarsFromRating(uint32_t rating) {
    if (rating == 0) return 0;
    if (rating <= 12) return 1;
    if (rating <= 37) return 2;
    if (rating <= 62) return 3;
    if (rating <= 87) return 4;
    return 5;
}

uint32_t PropUInt(const PROPVARIANT& value) {
    switch (value.vt) {
    case VT_UI4: return value.uintVal;
    case VT_UI2: return value.uiVal;
    case VT_UI1: return value.bVal;
    case VT_I4: return value.lVal > 0 ? static_cast<uint32_t>(value.lVal) : 0;
    case VT_I2: return value.iVal > 0 ? static_cast<uint32_t>(value.iVal) : 0;
    default: return 0;
    }
}

uint64_t PropDuration100ns(const PROPVARIANT& value) {
    if (value.vt == VT_UI8) return value.uhVal.QuadPart;
    if (value.vt == VT_I8 && value.hVal.QuadPart > 0) return static_cast<uint64_t>(value.hVal.QuadPart);
    return 0;
}

MediaSortValue ReadMediaProperties(const std::wstring& path) {
    MediaSortValue out;
    const std::wstring shell = path::StripExtendedPathPrefix(path);
    ComPtr<IPropertyStore> store;
    if (shell.empty() || FAILED(SHGetPropertyStoreFromParsingName(shell.c_str(), nullptr, GPS_BESTEFFORT,
                                                                  IID_PPV_ARGS(&store))) || !store)
        return out;
    PROPVARIANT value{};
    PropVariantInit(&value);
    if (SUCCEEDED(store->GetValue(PKEY_Media_Duration, &value))) {
        if (const uint64_t ticks = PropDuration100ns(value))
            out.duration_ms = static_cast<uint32_t>((std::min)(ticks / 10000ull, 0xFFFFFFFFull));
    }
    PropVariantClear(&value);
    PropVariantInit(&value);
    uint32_t simple = 0;
    if (SUCCEEDED(store->GetValue(PKEY_SimpleRating, &value))) simple = PropUInt(value);
    PropVariantClear(&value);
    if (simple >= 1 && simple <= 5) {
        out.rating = static_cast<uint8_t>(simple);
        return out;
    }
    PropVariantInit(&value);
    uint32_t rating = 0;
    if (SUCCEEDED(store->GetValue(PKEY_Rating, &value))) rating = PropUInt(value);
    PropVariantClear(&value);
    out.rating = StarsFromRating(rating);
    return out;
}

struct ChangeFlag {
    std::atomic<bool> value{false}, posted{false};
    std::atomic<HWND> hwnd{nullptr};
    std::atomic<UINT> message{0};
    void Raise() {
        value.store(true);
        if (!posted.exchange(true))
            if (const HWND target = hwnd.load()) PostMessageW(target, message.load(), 0, 0);
    }
    bool Take() {
        posted.store(false);
        return value.exchange(false);
    }
};

} // namespace

bool IsMediaSortName(std::wstring_view name) {
    if (IsPictureName(name)) return true;
    const std::wstring ext = LowerExt(name);
    return !ext.empty() && preview::IsVideoExtension(ext);
}

// Rating and duration properties also exist on audio; read them when those
// sorts are active, even though the extra columns only appear for photo/video.
static bool IsMediaPropertyName(std::wstring_view name) {
    if (IsMediaSortName(name)) return true;
    const std::wstring ext = LowerExt(name);
    return !ext.empty() && preview::IsAudioExtension(ext);
}

bool EntriesHaveMedia(const std::vector<fs::DirEntry>& entries, size_t limit) {
    const size_t n = (std::min)(entries.size(), limit);
    for (size_t i = 0; i < n; ++i)
        if (!entries[i].is_dir && IsMediaSortName(entries[i].name)) return true;
    return false;
}

struct MediaSortCache::Impl {
    struct Item {
        std::wstring path;
        uint64_t modified = 0;
        uint64_t size = 0;
        uint32_t duration_ms = 0;
        uint8_t rating = 0;
        bool ready = false;
        bool queued = false;
    };

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::unordered_map<std::wstring, Item> items;
    std::deque<std::wstring> queue;
    std::thread worker;
    bool stopping = false;
    uint64_t revision = 0;
    ChangeFlag changed;

    void EnsureWorker() {
        if (worker.joinable()) return;
        worker = std::thread([this] { Run(); });
    }

    void Run() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        size_t since_notify = 0;
        for (;;) {
            std::wstring path;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || !queue.empty(); });
                if (stopping && queue.empty()) break;
                path = std::move(queue.front());
                queue.pop_front();
            }
            std::wstring read_path;
            {
                std::lock_guard lock(mutex);
                const auto it = items.find(path);
                if (it != items.end()) read_path = it->second.path;
            }
            const MediaSortValue value = read_path.empty() ? MediaSortValue{} : ReadMediaProperties(read_path);
            bool idle = false;
            {
                std::lock_guard lock(mutex);
                auto it = items.find(path);
                if (it != items.end()) {
                    it->second.duration_ms = value.duration_ms;
                    it->second.rating = value.rating;
                    it->second.ready = true;
                    it->second.queued = false;
                    ++revision;
                }
                idle = queue.empty();
            }
            // Small folders never reach kNotifyBatch; raise when the queue drains.
            if (++since_notify >= kNotifyBatch || idle) {
                since_notify = 0;
                changed.Raise();
            }
        }
        if (since_notify) changed.Raise();
        CoUninitialize();
    }
};

MediaSortCache::MediaSortCache() : impl_(std::make_unique<Impl>()) {}
MediaSortCache::~MediaSortCache() { Stop(); }

void MediaSortCache::SetChangeNotify(HWND hwnd, UINT message) {
    impl_->changed.hwnd.store(hwnd);
    impl_->changed.message.store(message);
}

bool MediaSortCache::TakeChanged() { return impl_->changed.Take(); }

uint64_t MediaSortCache::Revision() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->revision;
}

bool MediaSortCache::Request(const std::vector<MediaSortRequest>& files) {
    bool already = false;
    bool queued_any = false;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) return false;
        for (const auto& file : files) {
            if (file.path.empty()) continue;
            const std::wstring key = path::FoldedKey(file.path);
            auto [it, inserted] = impl_->items.try_emplace(key);
            auto& item = it->second;
            item.path = file.path;
            if (!inserted && item.ready && item.modified == file.modified && item.size == file.size) {
                already = true;
                continue;
            }
            item.modified = file.modified;
            item.size = file.size;
            if (item.queued) continue;
            item.ready = false;
            item.queued = true;
            impl_->queue.push_back(key);
            queued_any = true;
        }
        if (queued_any) impl_->EnsureWorker();
    }
    if (queued_any) impl_->wake.notify_one();
    return already;
}

void MediaSortCache::Apply(std::vector<fs::DirEntry>& entries, const std::wstring& parent) const {
    std::lock_guard lock(impl_->mutex);
    for (auto& entry : entries) {
        if (entry.is_dir) continue;
        const std::wstring full = entry.full_path.empty() ? ChildPath(parent, entry.name) : entry.full_path;
        const auto it = impl_->items.find(path::FoldedKey(full));
        if (it == impl_->items.end() || !it->second.ready) continue;
        entry.media_duration_ms = it->second.duration_ms;
        entry.media_rating = it->second.rating;
    }
}

void MediaSortCache::Stop() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
        impl_->queue.clear();
    }
    impl_->wake.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

bool RequestMediaSort(AppState& s) {
    s.mediaSort.SetChangeNotify(ServiceWindow(s), WM_UI_KICK);
    bool waiting = false;
    ForEachPane(s, [&](Pane& pane) {
        Tab* tab = pane.ActiveTab();
        if (!tab || !ui::IsMediaSortColumn(tab->sort_column) || !tab->snapshot || tab->loading ||
            tab->pending_generation != 0 || tab->content_results || tab->search_content_active ||
            tab->current_path.empty() || fs::IsVirtualPath(tab->current_path) ||
            tab->snapshot_path != tab->current_path)
            return;
        std::vector<MediaSortRequest> files;
        files.reserve((std::min)(tab->snapshot->size(), kRequestLimit));
        for (const auto& entry : *tab->snapshot) {
            if (files.size() >= kRequestLimit) break;
            if (entry.is_dir || entry.cloud_recall || !IsMediaPropertyName(entry.name)) continue;
            MediaSortRequest req;
            req.path = entry.full_path.empty() ? ChildPath(tab->current_path, entry.name) : entry.full_path;
            req.modified = FileTimeTicks(entry.mtime);
            req.size = entry.size;
            files.push_back(std::move(req));
        }
        s.mediaSort.Request(files);
        if (s.mediaSort.Revision() != tab->media_sort_signature) waiting = true;
    });
    return waiting;
}

bool ResortForMediaSort(AppState& s) {
    bool waiting = false;
    const uint64_t now = GetTickCount64();
    const bool busy = s.scrollAnimating || s.scrollbarDragging ||
        s.renameIndex >= 0 || s.marqueeActive || s.dragPending ||
        (GetKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetKeyState(VK_RBUTTON) & 0x8000) != 0;
    ForEachPane(s, [&](Pane& pane) {
        Tab* tab = pane.ActiveTab();
        if (!tab || !ui::IsMediaSortColumn(tab->sort_column) || !tab->snapshot || tab->loading ||
            tab->pending_generation != 0 || tab->content_results || tab->search_content_active ||
            tab->current_path.empty() || fs::IsVirtualPath(tab->current_path) ||
            tab->snapshot_path != tab->current_path || !tab->held_renames.empty() ||
            tab->snapshot->size() > kLiveResortLimit)
            return;
        if (busy) { waiting = true; return; }
        const uint64_t signature = s.mediaSort.Revision();
        if (signature == tab->media_sort_signature) return;
        if (now - tab->media_sort_resorted_at < kResortIntervalMs) {
            waiting = true;
            return;
        }
        auto sorted = std::make_shared<std::vector<fs::DirEntry>>(*tab->snapshot);
        s.mediaSort.Apply(*sorted, tab->current_path);
        {
            const ScopedEntryGrouping grouping(tab->EffectiveGroup(), tab->current_path);
            const ScopedFolderSortMode folder_mode(tab->EffectiveFolderSort());
            SortEntries(*sorted, tab->sort_column, tab->sort_direction);
        }
        tab->media_sort_signature = signature;
        tab->media_sort_resorted_at = now;
        if (std::equal(sorted->begin(), sorted->end(), tab->snapshot->begin(), tab->snapshot->end(),
                       [](const fs::DirEntry& a, const fs::DirEntry& b) { return a.name == b.name; }))
            return;
        const bool all = tab->all_selected;
        std::vector<std::wstring> names;
        std::wstring focus;
        if (!all) {
            const int count = static_cast<int>(tab->EntryCount());
            for (const int index : tab->SelectedIndices())
                if (index >= 0 && index < count) names.push_back(tab->EntryAt(static_cast<size_t>(index)).name);
            if (tab->selected_index >= 0 && tab->selected_index < count)
                focus = tab->EntryAt(static_cast<size_t>(tab->selected_index)).name;
        }
        tab->SetSnapshot(std::move(sorted));
        tab->order_held = false;
        if (!all) {
            if (names.empty()) tab->ClearSelection();
            else tab->RemapSelection(names, focus);
        }
        if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
    });
    return waiting;
}

fs::SnapshotPtr WithMediaSortApplied(AppState& s, Tab& tab, fs::SnapshotPtr listing, bool resort) {
    if (!listing || !ui::IsMediaSortColumn(tab.sort_column)) return listing;
    auto copy = std::make_shared<std::vector<fs::DirEntry>>(*listing);
    s.mediaSort.Apply(*copy, tab.current_path);
    if (resort) {
        const ScopedEntryGrouping grouping(tab.EffectiveGroup(), tab.current_path);
        const ScopedFolderSortMode folder_mode(tab.EffectiveFolderSort());
        SortEntries(*copy, tab.sort_column, tab.sort_direction);
        tab.media_sort_signature = s.mediaSort.Revision();
        tab.media_sort_resorted_at = GetTickCount64();
    }
    return copy;
}

} // namespace pulse::app
