#include "media_rating.h"
#include "app_runtime.h"
#include "app_state.h"
#include "media_props.h"
#include "../common/path_utils.h"
#include "../fs/fs_enum.h"
#include <objbase.h>
#include <propsys.h>
#include <propvarutil.h>
#include <propkey.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#pragma comment(lib, "propsys.lib")

using Microsoft::WRL::ComPtr;

namespace pulse::app {
namespace {

// System.Rating on disk: 1-12 is one star, 13-37 two, 38-62 three, 63-87 four, 88-99 five.
uint32_t RatingFromStars(uint8_t stars) {
    static constexpr uint32_t kValues[] = {0, 1, 25, 50, 75, 99};
    return kValues[(std::min<size_t>)(stars, 5)];
}

// 0 clears the rating. Runs on the worker: the file's property handler opens it.
HRESULT WriteRating(const std::wstring& path, uint8_t stars) {
    const std::wstring shell = path::StripExtendedPathPrefix(path);
    ComPtr<IPropertyStore> store;
    HRESULT hr = shell.empty() ? E_INVALIDARG
        : SHGetPropertyStoreFromParsingName(shell.c_str(), nullptr, GPS_READWRITE, IID_PPV_ARGS(&store));
    if (FAILED(hr)) return hr;
    if (!store) return E_FAIL;
    PROPVARIANT value{};
    PropVariantInit(&value);   // VT_EMPTY removes the property
    if (stars) {
        hr = InitPropVariantFromUInt32(RatingFromStars(stars), &value);
        if (FAILED(hr)) return hr;
    }
    hr = store->SetValue(PKEY_Rating, value);
    PropVariantClear(&value);
    return SUCCEEDED(hr) ? store->Commit() : hr;
}

std::wstring LeafName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

struct Writer {
    struct Job {
        std::wstring path;
        uint8_t stars = 0;
    };
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::thread worker;
    bool stopping = false;
    std::atomic<uint32_t> failures{0};
    char owner = 0;   // the owner token the read-backs are queued under

    void Run() {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || !jobs.empty(); });
                if (jobs.empty()) break;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            if (FAILED(WriteRating(job.path, job.stars))) {
                ++failures;
                continue;
            }
            // Read it back through the media reader: what the file holds is what
            // the column and the panel show.
            media::Forget(job.path);
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(job.path.c_str(), GetFileExInfoStandard, &data)) continue;
            media::Job read;
            read.path = job.path;
            read.mtime = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                         data.ftLastWriteTime.dwLowDateTime;
            read.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            read.attrs = data.dwFileAttributes;
            read.kind = media::KindOfName(LeafName(job.path));
            if (read.kind != media::Kind::None) media::Request(&owner, {std::move(read)});
        }
        if (com) CoUninitialize();
    }
};

Writer& Instance() {
    static Writer* writer = new Writer();   // lives to the end of the process
    return *writer;
}

} // namespace

std::vector<std::wstring> RatableSelection(const Tab& tab) {
    std::vector<std::wstring> out;
    if (!tab.snapshot || tab.content_results || tab.current_path.empty() || fs::IsVirtualPath(tab.current_path))
        return out;
    for (const int index : tab.SelectedIndices()) {
        if (index < 0 || static_cast<size_t>(index) >= tab.EntryCount()) return {};
        const fs::DirEntry entry = tab.EntryAt(static_cast<size_t>(index));
        if (entry.is_dir || entry.cloud_recall || !CanRateName(entry.name)) return {};
        std::wstring full = EntryFullPath(tab, index);
        if (full.empty()) return {};
        out.push_back(std::move(full));
    }
    return out;
}

bool SetSelectedRating(AppState& s, uint8_t stars) {
    const Tab* tab = ActiveTab(s);
    if (!tab || stars > 5) return false;
    std::vector<std::wstring> paths = RatableSelection(*tab);
    if (paths.empty()) return false;
    Writer& writer = Instance();
    {
        std::lock_guard lock(writer.mutex);
        if (writer.stopping) return false;
        for (auto& path : paths) writer.jobs.push_back({std::move(path), stars});
        if (!writer.worker.joinable()) writer.worker = std::thread([&writer] { writer.Run(); });
    }
    writer.wake.notify_one();
    return true;
}

uint32_t TakeRatingFailures() { return Instance().failures.exchange(0); }

void StopRatingWriter() {
    Writer& writer = Instance();
    {
        std::lock_guard lock(writer.mutex);
        writer.stopping = true;
    }
    writer.wake.notify_all();
    if (writer.worker.joinable()) writer.worker.join();
}

} // namespace pulse::app
