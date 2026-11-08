// zip_compress_ui.cpp - see zip_compress_ui.h.
#include "zip_compress_ui.h"
#include "app_internal.h"
#include "app_messages.h"
#include "arrival_select.h"
#include "selection_caps.h"
#include "zip_compress.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include "../common/text_format.h"
#include "../ops/ops_manager.h"
#include <algorithm>
#include <memory>
#include <thread>
#include <vector>

namespace pulse {
namespace {

// Arrival tasks of 压缩 stay clear of the ops queue's ids.
constexpr uint64_t kZipTaskBase = 1ull << 62;
// A quick archive just shows its result; a slower one shows it is working.
constexpr ULONGLONG kBusySnackMs = 400;

struct ZipDone {
    uint64_t id = 0;
    std::wstring zip_path;
    app::ZipCompressResult result;
};

uint64_t NextZipId() {
    static uint64_t next = 0;
    return kZipTaskBase + ++next;
}

std::wstring Leaf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring Fill(std::wstring text, const wchar_t* key, const std::wstring& value) {
    const size_t at = text.find(key);
    if (at != std::wstring::npos) text.replace(at, wcslen(key), value);
    return text;
}

bool CompressTarget(AppState& s, std::vector<std::wstring>* paths, std::wstring* dir) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || tab->SelectedCount() <= 0) return false;
    if (!SelectionCapsOf(s).copy) return false;
    std::vector<std::wstring> selected = SelectedFullPaths(*tab);
    if (selected.empty()) return false;
    // One parent (search results may mix folders): the archive goes there.
    const std::wstring parent = fs::ParentPath(selected.front());
    if (parent.empty() || fs::IsVirtualPath(parent)) return false;
    for (const auto& path : selected)
        if (fs::IsVirtualPath(path) || _wcsicmp(fs::ParentPath(path).c_str(), parent.c_str()) != 0) return false;
    if (paths) *paths = std::move(selected);
    if (dir) *dir = parent;
    return true;
}

void ShowResult(AppState& s, const ZipDone& done) {
    const std::wstring name = Leaf(done.zip_path);
    s.notification_toast.ReplaceNextSnack();   // the 正在压缩 capsule becomes the result
    if (done.result.ok) {
        std::wstring detail = Fill(l10n::Get(l10n::StringId::ZipCompressDetail), L"{n}",
                                   std::to_wstring(done.result.files));
        WIN32_FILE_ATTRIBUTE_DATA data{};
        uint64_t packed = 0;
        if (GetFileAttributesExW(done.zip_path.c_str(), GetFileExInfoStandard, &data))
            packed = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        detail = Fill(std::move(detail), L"{size}", format::ByteSize(packed));
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Merge,
            Fill(l10n::Get(l10n::StringId::ZipCompressDone), L"{name}", name), std::move(detail), {});
    } else if (done.result.cancelled) {
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Merge,
            l10n::Get(l10n::StringId::ZipCompressCancelled), name, {}, {}, 3000);
    } else {
        s.notification_toast.ShowError(s.hwnd, l10n::Get(l10n::StringId::ZipCompressFailed),
            done.result.error.empty() ? name : done.result.error);
    }
}

} // namespace

bool CanCompressSelection(AppState& s) {
    // Asked on every toolbar paint: the cheap part only; CompressSelection
    // checks each item.
    const app::Tab* tab = ActiveTab(s);
    return tab && tab->snapshot && tab->SelectedCount() > 0 && !tab->current_path.empty() &&
        !fs::IsVirtualPath(tab->current_path) && !tab->net_readonly && app::ZipCompressAvailable();
}

namespace {
uint64_t StartZip(AppState& s, std::vector<std::wstring> paths, const std::wstring& dir,
                  const std::wstring& zip_path) {
    const uint64_t id = NextZipId();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    s.zipJobs.push_back({id, zip_path, cancel});
    // Select the archive once it is listed, the way a paste selects its items.
    if (app::Tab* tab = ActiveTab(s);
        tab && _wcsicmp(fs::NormalizePath(tab->current_path).c_str(), fs::NormalizePath(dir).c_str()) == 0) {
        ArmArrivalSelection(s, nullptr, id, 0);
        ExpectArrival(s, zip_path);
    }
    const HWND hwnd = s.hwnd;
    std::thread([hwnd, id, paths = std::move(paths), zip_path, cancel] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const ULONGLONG started = GetTickCount64();
        bool told = false;
        auto done = std::make_unique<ZipDone>();
        done->id = id;
        done->zip_path = zip_path;
        done->result = app::CompressToZip(paths, zip_path, cancel.get(), [&](uint64_t, uint64_t) {
            if (!told && GetTickCount64() - started >= kBusySnackMs) {
                told = true;
                PostMessageW(hwnd, WM_ZIP_COMPRESS, 0, static_cast<LPARAM>(id & 0x7FFFFFFF));
            }
        });
        if (PostMessageW(hwnd, WM_ZIP_COMPRESS, 1, reinterpret_cast<LPARAM>(done.get()))) done.release();
    }).detach();
    return id;
}
} // namespace

void CompressSelection(AppState& s) {
    std::vector<std::wstring> paths;
    std::wstring dir;
    if (!CompressTarget(s, &paths, &dir)) return;
    const std::wstring zip_path = app::ZipPathFor(paths, dir);
    StartZip(s, std::move(paths), dir, zip_path);
}

void HandleZipMessage(AppState& s, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ZIP_CANCEL) {
        for (auto& job : s.zipJobs) job.cancel->store(true);
        return;
    }
    if (wparam == 0) {
        // Still working: the snack offers 取消 until the result replaces it.
        const uint64_t low = static_cast<uint64_t>(lparam);
        const auto job = std::find_if(s.zipJobs.begin(), s.zipJobs.end(),
                                      [&](const AppState::ZipJob& j) { return (j.id & 0x7FFFFFFF) == low; });
        if (job == s.zipJobs.end() || job->cancel->load()) return;
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Merge, l10n::Get(l10n::StringId::ZipCompressing),
            Leaf(job->zip_path), {ui::SnackAction{l10n::Get(l10n::StringId::Cancel), WM_ZIP_CANCEL}}, {}, 10 * 60 * 1000);
        return;
    }
    std::unique_ptr<ZipDone> done(reinterpret_cast<ZipDone*>(lparam));
    if (!done) return;
    s.zipJobs.erase(std::remove_if(s.zipJobs.begin(), s.zipJobs.end(),
                                   [&](const AppState::ZipJob& j) { return j.id == done->id; }),
                    s.zipJobs.end());
    ops::FinishedTask finished;
    finished.task_id = done->id;
    finished.success = done->result.ok;
    NoteArrivalFinished(s, finished);
    if (done->result.ok) {
        const std::wstring parent = fs::ParentPath(done->zip_path);
        s.store.MarkDirty(parent);
        RefreshPath(s, parent, RefreshReason::OperationCompleted);
    }
    ShowResult(s, *done);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void CancelZipJobs(AppState& s) {
    for (auto& job : s.zipJobs) job.cancel->store(true);
}
}
