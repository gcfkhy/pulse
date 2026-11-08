#include "extensions/native/settings_pages_host.h"
#include "extensions/native/previews_adapter.h"
#include "pane_layout_controls.h"
#include "recycle_shell_watch.h"
// app_runtime.cpp — extracted from app_main.cpp.
#include "app_window_title.h"
#include "dialog_management_ui.h"
#include "../fs/drive_kind.h"
#include "../fs/bounded_enumeration.h"
#include "folder_statistics.h"
#include "../fs/device_path.h"
#include "../common/shell_id.h"
#include "device_registry.h"
#include "folder_alias.h"
#include "app_wall_hint.h"
#include "app_internal.h"
#include "workspace_sidebar.h"
#include "media_sort.h"
#include "app_network_page.h"
#include "shortcut_page.h"
#include "keymap.h"
#include "../ui/ui_renderer.h"
#include <memory>
#include <thread>
#include <shlobj.h>
#include <unordered_map>
#include "system_ctx_model.h"
#include "pane_layout.h"
#include "tab_spring.h"
#include "vertical_tabs.h"
#include "app_column_view.h"
#include "git_ui.h"
#include "sync_status_ui.h"
#include "mtime_hint_prefs.h"
#include "app_details_tree.h"
#include "folder_sizes_ui.h"
#include "update_status.h"
#include "download_source_probe.h"
#include "ai_assistant_settings.h"
#include "about_info.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/link_type_text.h"
#include "../ui/fluent_menu.h"
#include "../ui/preview_format_catalog.h"
#include "../ui/drag_drop.h"
#include "../ui/toolbar_picker.h"
#include "toolbar_apps.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../fs/cloud_state.h"
#include "cloud_download.h"
#include "cloud_roots.h"
#include "../ui/typography.h"
#include "../ui/ai_list_overlay.h"
#include "../ui/settings_page_ids.h"
#include "../ui/color_picker.h"
#include "filter_tag_complete.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include "../common/display_path.h"
#include "../common/diagnostics_exporter.h"
#include "snapshot_patch.h"
#include "dir_notify_queue.h"
#include "content_pack.h"
#include "content_pack_ui.h"
#include "ai_pack.h"
#include "ai_pack_ui.h"
#include "ai_settings_ui.h"
#include "ai_assistant.h"
#include "session.h"
#include "context_menu.h"
#include "batch_rename.h"
#include "link_resolve.h"
#include "duplicate_scan.h"
#include "search_query.h"
#include "resource.h"
#include "pulse_version.h"
#include "tray_reveal.h"
#include "favorites_bar.h"
#include "default_file_manager.h"
#include "window_identity_ui.h"
#include "../ops/clipboard.h"
#include "../ipc/ctx_menu_util.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include "settings_transfer_ui.h"
#include <thread>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <unordered_set>

using namespace pulse;

namespace {

struct DetailsMetaRequest {
    HWND hwnd = nullptr;
    std::wstring path;
};

class DetailsMetaWorker {
public:
    void Submit(HWND hwnd, std::wstring path) {
        if (!hwnd || path.empty()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = DetailsMetaRequest{hwnd, std::move(path)};
            if (!thread_.joinable()) thread_ = std::thread([this] { Run(); });
        }
        cv_.notify_one();
    }

private:
    void Run() {
        for (;;) {
            DetailsMetaRequest request;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return pending_.has_value(); });
                request = std::move(*pending_);
                pending_.reset();
            }
            auto* result = new DetailsMetaResult{};
            result->path = request.path;
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(request.path.c_str(), GetFileExInfoStandard, &fad)) {
                result->attrs_valid = true;
                result->created = fad.ftCreationTime;
                result->modified = fad.ftLastWriteTime;
                result->accessed = fad.ftLastAccessTime;
            }
            SHFILEINFOW sfi{};
            const std::wstring shell_path = path::StripExtendedPathPrefix(request.path);
            if (SHGetFileInfoW(shell_path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_TYPENAME) &&
                sfi.szTypeName[0]) {
                result->type_name = sfi.szTypeName;
            }
            result->meta = app::FetchDetailsMeta(request.path);
            if (!PostMessageW(request.hwnd, WM_DETAILS_META, 0, reinterpret_cast<LPARAM>(result)))
                delete result;
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<DetailsMetaRequest> pending_;
    std::thread thread_;
};

DetailsMetaWorker& GetDetailsMetaWorker() {
    // The worker is intentionally process-lifetime: Windows may leave a Shell
    // metadata call blocked on an offline provider during window teardown.
    static auto* worker = new DetailsMetaWorker();
    return *worker;
}

void RefreshDuplicateGroupViews(AppState& s) {
    constexpr size_t kMaxGroups = 80;
    constexpr size_t kMaxFiles = 40;
    s.dup_view_cache.clear();
    const size_t group_n = (std::min)(s.duplicateScan.groups.size(), kMaxGroups);
    s.dup_view_cache.reserve(group_n);
    for (size_t g = 0; g < group_n; ++g) {
        const auto& group = s.duplicateScan.groups[g];
        ui::DuplicateGroupView view;
        wchar_t title[128]{};
        swprintf_s(title, l10n::Get(l10n::StringId::DupGroupFormat).c_str(),
                   format::ByteSize(group.size).c_str(),
                   static_cast<int>(group.files.size()));
        view.title = title;
        const size_t file_n = (std::min)(group.files.size(), kMaxFiles);
        view.files.reserve(file_n);
        for (size_t f = 0; f < file_n; ++f) {
            ui::DuplicateFileView file;
            file.name = group.files[f].name;
            file.path = group.files[f].path;
            FILETIME time{};
            time.dwLowDateTime = static_cast<DWORD>(group.files[f].modified);
            time.dwHighDateTime = static_cast<DWORD>(group.files[f].modified >> 32);
            file.detail = group.files[f].path + L" · " + format::LocalFileTime(time);
            file.keep = f == group.keep_index;
            view.files.push_back(std::move(file));
        }
        s.dup_view_cache.push_back(std::move(view));
    }
    s.dup_view_epoch = s.duplicateScan.result_epoch;
    s.dup_view_language = s.appPrefs.language;
}

} // namespace

namespace pulse {
namespace {
app::UpdateProgress UpdateProgressForView(const AppState& s) {
    if (s.shot.active) {
        using app::UpdatePhase;
        const auto& phase = s.shot.update_state;
        if (phase == L"connecting") return {UpdatePhase::Connecting};
        if (phase == L"downloading") return {UpdatePhase::Downloading, 3 * 1024 * 1024, 8 * 1024 * 1024};
        if (phase == L"downloading-unknown") return {UpdatePhase::Downloading, 3 * 1024 * 1024, 0};
        if (phase == L"verifying") return {UpdatePhase::Verifying};
        if (phase == L"waiting") return {UpdatePhase::WaitingOperations};
        if (phase == L"launching") return {UpdatePhase::Launching};
        if (phase == L"installing") return {UpdatePhase::Installing};
        return {};
    }
    return s.update_installer.Progress();
}
}

void PrefetchDetailsMeta(HWND hwnd, const std::wstring& path) {
    GetDetailsMetaWorker().Submit(hwnd, path);
}
void RememberLayoutFocus(AppState& s) {
    for (auto& owned : s.window_tabs.items) {
        if (!owned) continue;
        int focused = owned->focused_index;
        bool saw_focus = false;
        int target_here = -1;
        for (size_t i = 0; i < owned->panes.size(); ++i) {
            if (owned->panes[i].get() == s.pane) {
                focused = static_cast<int>(i);
                saw_focus = true;
            }
            if (owned->panes[i].get() == s.targetPane)
                target_here = static_cast<int>(i);
        }
        if (saw_focus) owned->focused_index = focused;
        if (s.targetPane) {
            if (target_here >= 0) owned->target_index = target_here;
        } else if (saw_focus) {
            owned->target_index = -1;
        }
    }
}

std::vector<std::wstring> VisibleFolderPaths(const AppState& s) {
    std::vector<std::wstring> paths;
    const app::LayoutTab* tab = s.window_tabs.Active();
    if (!tab) return paths;
    std::vector<app::Pane*> vis;
    if (tab->root) tab->root->CollectPanes(vis);
    else {
        vis.reserve(tab->panes.size());
        for (const auto& pane : tab->panes) {
            if (pane) vis.push_back(pane.get());
        }
    }
    for (app::Pane* pane : vis) {
        const app::Tab* view = pane ? pane->ActiveTab() : nullptr;
        if (!view || view->current_path.empty() || fs::IsVirtualPath(view->current_path))
            continue;
        paths.push_back(fs::NormalizePath(view->current_path));
        AppendDetailsTreeWatchPaths(*view, paths);
    }
    return paths;
}

void SyncVisibleWatches(AppState& s) {
    // The watches are shared: they cover what every window shows (M6e), and
    // report to the shared queue, not to a window that may close first.
    std::vector<std::wstring> paths;
    app::ForEachWindowSharingCore(s, [&](AppState& window) {
        for (std::wstring& path : VisibleFolderPaths(window)) paths.push_back(std::move(path));
    });
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    AppCore& core = s.core;
    const HWND service = ServiceWindow(s);
    s.watches.Sync(paths, [&core, service](const std::wstring& path, bool overflow, std::vector<fs::DirNotifyEvent> events) {
        std::lock_guard<std::mutex> lock(core.notify_mu);
        const bool first = core.notify_queue.empty();
        app::QueueDirNotify(core.notify_queue, path, overflow, std::move(events));
        // The tick drains the whole queue, so one wake per batch is enough.
        if (first) PostMessageW(service, WM_UI_KICK, 0, 0);
    });
}

void BindCurrentLayout(AppState& s) {
    // Settings has no tab in the strip (its title-bar button stands in): once
    // another tab is active the settings tab goes away. Page, scroll and
    // expanded cards live in s.settings, so reopening comes back the same.
    if (!s.tabDragging) {
        for (size_t i = s.window_tabs.items.size(); i-- > 0;) {
            app::LayoutTab& layout = *s.window_tabs.items[i];
            if (i == s.window_tabs.active || layout.pinned || layout.panes.size() != 1 ||
                s.window_tabs.items.size() <= 1 || !IsSettingsTab(layout.ActiveFolder()))
                continue;
            s.window_tabs.CloseTab(i);
        }
    }
    app::LayoutTab* tab = s.window_tabs.Active();
    if (!tab || tab->panes.empty()) {
        s.pane = nullptr;
        s.targetPane = nullptr;
        SyncVisibleWatches(s);
        SyncTaskbarWindowTitle(s);
        return;
    }
    tab->focused_index = std::clamp(tab->focused_index, 0,
                                    static_cast<int>(tab->panes.size()) - 1);
    s.pane = tab->panes[static_cast<size_t>(tab->focused_index)].get();
    if (tab->target_index >= 0 &&
        tab->target_index < static_cast<int>(tab->panes.size())) {
        s.targetPane = tab->panes[static_cast<size_t>(tab->target_index)].get();
    } else {
        s.targetPane = nullptr;
    }
    if (!tab->root) app::RebuildLayoutRoot(*tab);
    for (size_t i = 0; i < tab->panes.size(); ++i) {
        tab->panes[i]->focused = (static_cast<int>(i) == tab->focused_index);
        tab->panes[i]->target = (tab->panes[i].get() == s.targetPane);
    }
    if (app::Tab* view = s.pane->ActiveTab()) {
        s.scrollTargetY = view->scroll_y;
        s.scrollAnimating = false;
    }
    SyncVisibleWatches(s);
    SyncTaskbarWindowTitle(s);
}
// Live text of the rename field; icon grids size the field to wrap it.
// Inline rename (P1f): the note under the field and the width it has grown to.
static void FillRenameHint(const AppState& s, ui::PaneViewModel& pane) {
    pane.rename_field_w = s.renameIndex >= 0 ? s.renameFieldW : 0.0f;
    const ULONGLONG now = GetTickCount64();
    if (s.renameIndex >= 0 && s.renameShakeAt != 0 && now - s.renameShakeAt < 360)
        pane.rename_shake_ms = static_cast<int>(now - s.renameShakeAt);
    if (s.renameIndex < 0 || s.renameHint.empty()) return;
    if (s.renameHintUntil != 0 && now >= s.renameHintUntil) return;
    pane.rename_hint = s.renameHint;
    pane.rename_hint_ms = s.renameHintUntil == 0 ? -1 : static_cast<int>(s.renameHintUntil - now);
}

static std::wstring RenameEditText(const AppState& s) {
    if (s.renameIndex < 0 || !s.hwndRenameEdit || !IsWindow(s.hwndRenameEdit)) return {};
    const int len = GetWindowTextLengthW(s.hwndRenameEdit);
    if (len <= 0) return {};
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    text.resize(static_cast<size_t>(std::max(0, GetWindowTextW(s.hwndRenameEdit, text.data(), len + 1))));
    return text;
}
// Whether a launch argument names a file, without stalling the UI thread on
// a share (or mapped drive) that does not answer: GetFileAttributes on an unreachable server
// blocks for ~13 s, and startup asked twice (ResolveOpenFolderPath, then
// SelectLaunchedFile), so the window took ~27 s to appear. A share gets a
// second; past that the path opens as a folder and the listing says why.
static DWORD QueryLaunchArgumentAttributes(const std::wstring& path);

// One external open asks up to three times (OpenExternalFolder, then
// OpenFolderInNewTab and SelectLaunchedFile): the answer is kept for a few
// seconds so a share that does not answer costs one wait, not three.
// A drive root is a folder without asking: the first access to a sleeping
// disk waits for it to spin up.
static DWORD LaunchArgumentAttributes(const std::wstring& path) {
    if (path.empty()) return INVALID_FILE_ATTRIBUTES;
    if ((path.size() == 2 || (path.size() == 3 && (path[2] == L'\\' || path[2] == L'/'))) && path[1] == L':' &&
        (path[0] | 0x20) >= L'a' && (path[0] | 0x20) <= L'z')
        return FILE_ATTRIBUTE_DIRECTORY;
    struct Cached { std::wstring path; DWORD attrs = INVALID_FILE_ATTRIBUTES; ULONGLONG tick = 0; };
    thread_local Cached cached;
    const ULONGLONG now = GetTickCount64();
    if (cached.tick && now - cached.tick < 3000 && cached.path == path) return cached.attrs;
    const DWORD attrs = QueryLaunchArgumentAttributes(path);
    cached = {path, attrs, GetTickCount64()};
    return attrs;
}

static DWORD QueryLaunchArgumentAttributes(const std::wstring& path) {
    const bool remote = fs::IsUncPath(path) || fs::IsRemoteLikeDrive(path);
    if (!remote) return GetFileAttributesW(path.c_str());
    struct Query {
        std::wstring path;
        DWORD attrs = INVALID_FILE_ATTRIBUTES;
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~Query() { if (done) CloseHandle(done); }
    };
    auto query = std::make_shared<Query>();
    query->path = path;
    if (!query->done) return INVALID_FILE_ATTRIBUTES;
    try {
        std::thread([query] {
            query->attrs = GetFileAttributesW(query->path.c_str());
            SetEvent(query->done);
        }).detach();
    } catch (...) {
        return INVALID_FILE_ATTRIBUTES;
    }
    return WaitForSingleObject(query->done, 1000) == WAIT_OBJECT_0 ? query->attrs : INVALID_FILE_ATTRIBUTES;
}

std::wstring ResolveOpenFolderPath(std::wstring path, bool reveal) {
    while (!path.empty() && (path.front() == L'"' || path.back() == L'"')) {
        if (path.front() == L'"') path.erase(path.begin());
        if (!path.empty() && path.back() == L'"') path.pop_back();
    }
    if (path.empty()) return {};
    if (reveal) {
        // A result directory may carry a trailing separator, including extended paths.
        while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/') &&
               path[path.size() - 2] != L':') path.pop_back();
    }
    const DWORD attrs = LaunchArgumentAttributes(path);
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        (reveal || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)) {
        path = fs::ParentPath(path);
    }
    return fs::NormalizePath(path);
}

void SelectLaunchedFile(AppState& s, const std::wstring& raw, bool reveal) {
    std::wstring path = raw;
    while (!path.empty() && (path.front() == L'"' || path.back() == L'"')) {
        if (path.front() == L'"') path.erase(path.begin());
        if (!path.empty() && path.back() == L'"') path.pop_back();
    }
    if (reveal) {
        // A result directory may carry a trailing separator, including extended paths.
        while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/') &&
               path[path.size() - 2] != L':') path.pop_back();
    }
    const DWORD attrs = LaunchArgumentAttributes(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || (!reveal && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)) return;
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring leaf = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (leaf.empty()) return;
    if (app::Tab* tab = ActiveTab(s)) SelectNameInTab(s, *tab, leaf);
}

void OpenFolderInNewTab(AppState& s, const std::wstring& raw, bool reveal) {
    if (app::IsThisPcArgument(raw)) {
        // The This PC verb (设为默认文件管理器): This PC is the empty path,
        // which only OpenTabAt keeps (NewTab turns it into C:\). FindFolderTab
        // skips it, so each request opens a tab, as Explorer opens a window.
        if (!IsWindowVisible(s.hwnd) && TakeFreshStart(s)) StartFreshAt(s, L"");
        else OpenTabAt(s, L"");
        RevealWindow(s);
        return;
    }
    const std::wstring path = ResolveOpenFolderPath(raw, reveal);
    // Back from the tray with "open the default location" at startup: an
    // explicit folder starts over on its own, as launching with it would.
    if (!path.empty() && !IsWindowVisible(s.hwnd) && TakeFreshStart(s)) StartFreshAt(s, path);
    RevealWindow(s);
    if (!path.empty() && !ActivateExistingFolderTab(s, path)) NewTab(s, path);
    else InvalidateRect(s.hwnd, nullptr, FALSE);
    // A file (Pulse as a file's default app, a launcher's "open containing
    // folder") shows up selected instead of silently opening its folder (#40).
    if (!path.empty()) SelectLaunchedFile(s, raw, reveal);
}

void PostWorkerResult(AppState& s, app::WorkResult res) {
    {
        std::lock_guard<std::mutex> lock(s.resultMutex);
        s.results.push(std::move(res));
    }
    PostMessageW(ServiceWindow(s), WM_WORKER_RESULT, 0, 0);
}
D2D1_RECT_F FocusedPaneRect(const AppState& s) {
    D2D1_RECT_F content = s.renderer.ContentRect(
        static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    const auto& motion = s.paneLayoutMotion;
    if (motion.start && std::fabs(motion.content.left - content.left) < .5f &&
        std::fabs(motion.content.top - content.top) < .5f &&
        std::fabs(motion.content.right - content.right) < .5f &&
        std::fabs(motion.content.bottom - content.bottom) < .5f) {
        for (const auto& [pane, bounds] : motion.last)
            if (pane == s.pane) return bounds;
    }
    if (const auto* layout = s.window_tabs.Active(); layout && ZoomedPane(*layout)) return content;
    return app::FocusedWindowPaneRect(Root(s).get(), s.pane, content, s.scale);
}

app::Pane* PaneAtSlot(AppState& s, int index) {
    if (const auto* layout = s.window_tabs.Active())
        if (auto* zoomed = ZoomedPane(*layout)) return zoomed;
    if (!Root(s)) return s.pane;
    std::vector<app::Pane*> visible;
    Root(s)->CollectPanes(visible);
    if (index >= 0 && index < static_cast<int>(visible.size())) return visible[static_cast<size_t>(index)];
    return s.pane;
}

// Defined further below; the details-panel fill in BuildVm needs it.
std::wstring EntryFullPath(const app::Tab& tab, int index);

D2D1_RECT_F ListRect(const AppState& s) {
    D2D1_RECT_F pane = FocusedPaneRect(s);
    float extra = 0.0f;
    const app::Tab* tab = s.pane ? s.pane->ActiveTab() : nullptr;
    if (tab && !tab->banner_message.empty()) {
        // The strip grows to two lines when its message is long.
        const float drawn = s.renderer.FocusedPaneBannerHeight();
        extra = drawn > 0.0f ? drawn : 36.0f * s.scale;
    }
    std::wstring virtual_kind;
    if (tab && app::ParsePulsePath(tab->current_path, &virtual_kind, nullptr) &&
        virtual_kind == L"recent") {
        extra += 40.0f * s.scale;
    }
    if (virtual_kind == L"changes") extra += 36.0f * s.scale;
    const ui::ViewMode mode = tab ? tab->view_mode : ui::ViewMode::Details;
    if (tab && tab->column_layout) {
        ui::PaneViewModel strip;
        app::FillColumnStripView(strip.column_strip, *tab);
        pane = s.renderer.PaneBodyBounds(strip, pane);
    }
    pane.top += s.renderer.PaneHeaderHeight() + extra +
                (ui::ShowsColumnHeader(mode) ? s.renderer.ColumnHeaderHeight() : 0.0f);
    return pane;
}
bool ScrollbarGeometry(const AppState& s, const ui::PaneViewModel& pane,
                              D2D1_RECT_F& track, D2D1_RECT_F& thumb, float& maxScroll) {
    return s.renderer.PaneScrollbarGeometry(pane, FocusedPaneRect(s), track, thumb, maxScroll);
}

bool HorizontalScrollbarGeometry(const AppState& s, const ui::PaneViewModel& pane,
                                         D2D1_RECT_F& track, D2D1_RECT_F& thumb,
                                         float& maxScroll) {
    track = ListRect(s);
    const float viewW = std::max(0.0f, track.right - track.left);
    maxScroll = s.renderer.MaxScrollXForPane(pane, FocusedPaneRect(s));
    if (maxScroll <= 0.0f || viewW <= 0.0f) return false;
    // The drawn bar (the shared control); its thumb, full height to grab.
    const D2D1_RECT_F drawn = ui::fluent::ScrollbarThumbRect(
        ui::PaneHScrollbarSpec(pane, track, maxScroll, s.scale), s.scale);
    if (drawn.right <= drawn.left) return false;
    track.top = track.bottom - 12.0f * s.scale;
    thumb = D2D1::RectF(drawn.left, track.top, drawn.right, track.bottom);
    return true;
}

void RememberPath(AppState& s, const std::wstring& path) {
    if (path.empty() || fs::IsVirtualPath(path)) return;
    std::wstring n = fs::NormalizePath(path);
    if (n.empty()) return;
    s.places.RecordVisit(n);
}

void RecordRecentOpen(AppState& s, const std::wstring& path,
                             app::PlaceItemKind kind) {
    s.places.RecordRecent(path, kind);
}
// Drag spring-loading: how far the 800 ms rest on the folder row under the
// drag has come (the ring around its icon); <0 when no folder is arming.
static float DropSpringProgress(const AppState& s) {
    if (s.springRow < 0 || s.springRow != s.dropRow || !s.springStart) return -1.0f;
    return (std::min)(1.0f, static_cast<float>(GetTickCount64() - s.springStart) / 800.0f);
}

// Landing: rows a drag just dropped into `dir`, hidden while its cards fly
// to them. Time-bounded, so rows that show up late are simply shown.
static void FillLandingRows(const AppState& s, const app::Pane* owner, const std::wstring& dir,
                            ui::PaneViewModel& pane) {
    const auto& l = s.dragLanding;
    if (l.names.empty() || dir.empty() || _wcsicmp(dir.c_str(), l.dir.c_str()) != 0) return;
    if (l.pane && l.pane != owner) return;   // another pane showing the same folder
    const uint64_t now = ui::motion::NowMs();
    if (l.reveal_at ? now > l.reveal_at + 200 : now - l.since > 750) return;
    pane.landing_names.insert(l.names.begin(), l.names.end());
    pane.landing_reveal_at = l.reveal_at;
}

// Motion 5b: rows of `dir` that are on their way to the Recycle Bin.
static void FillLeavingRows(const AppState& s, const std::wstring& dir, ui::PaneViewModel& pane) {
    if (s.leavingPaths.empty() || dir.empty()) return;
    const uint64_t now = ui::motion::NowMs();
    if (s.leavingDoneAt ? now - s.leavingDoneAt > 800 : now - s.leavingSince > 8000) return;
    for (const auto& path : s.leavingPaths) {
        if (fs::ParentPath(path) != dir) continue;
        const size_t slash = path.find_last_of(L"\\/");
        pane.leaving_names.insert(slash == std::wstring::npos ? path : path.substr(slash + 1));
    }
    if (!pane.leaving_names.empty()) pane.leaving_since = s.leavingSince;
}

// Motion v2 "layout": panes added or removed by a layout change glide from
// where they were drawn (a new pane grows in from its outer edge). The same
// content area only: a window resize or sidebar/details motion snaps.
static void AnimatePaneLayout(AppState& s, const D2D1_RECT_F& content,
                              std::vector<std::pair<app::Pane*, D2D1_RECT_F>>& laid,
                              std::vector<app::SplitterLayout>& splitters) {
    auto& m = s.paneLayoutMotion;
    const auto same_rect = [](const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
        return std::fabs(a.left - b.left) < 0.5f && std::fabs(a.top - b.top) < 0.5f &&
               std::fabs(a.right - b.right) < 0.5f && std::fabs(a.bottom - b.bottom) < 0.5f;
    };
    const uint64_t now = ui::motion::NowMs();
    const void* owner = s.window_tabs.Active();
    if (m.owner != owner) { m = {}; m.owner = owner; }
    bool changed = laid.size() != m.target.size();
    for (size_t i = 0; !changed && i < laid.size(); ++i)
        changed = laid[i].first != m.target[i].first || !same_rect(laid[i].second, m.target[i].second);
    if (changed && !m.last.empty() && same_rect(content, m.content) && !s.splitterDragging &&
        ui::motion::SystemAnimationsEnabled()) {
        m.from = m.last;
        m.start = now;
    }
    m.target.clear();
    for (const auto& [pane, r] : laid) m.target.emplace_back(pane, r);
    if (!same_rect(content, m.content) || s.splitterDragging) m.start = 0;
    m.content = content;
    if (m.start) {
        const float ms = static_cast<float>(now - m.start);
        if (ms >= static_cast<float>(kPaneLayoutTransitionMs)) {
            m.start = 0;
        } else {
            const float p = PaneLayoutProgress(ms / static_cast<float>(kPaneLayoutTransitionMs));
            const D2D1_RECT_F first_final = laid.empty() ? D2D1_RECT_F{} : laid[0].second;
            for (auto& [pane, r] : laid) {
                D2D1_RECT_F f = r;
                bool found = false;
                for (const auto& [key, rect] : m.from)
                    if (key == pane) { f = rect; found = true; break; }
                if (!found) {
                    if (r.left > content.left + 1.0f) f = D2D1::RectF(r.right, r.top, r.right, r.bottom);
                    else if (r.top > content.top + 1.0f) f = D2D1::RectF(r.left, r.bottom, r.right, r.bottom);
                    else f = D2D1::RectF(r.left, r.top, r.left, r.bottom);
                }
                r = D2D1::RectF(std::round(f.left + (r.left - f.left) * p), std::round(f.top + (r.top - f.top) * p),
                                std::round(f.right + (r.right - f.right) * p),
                                std::round(f.bottom + (r.bottom - f.bottom) * p));
            }
            // One splitter between two panes follows the first pane's edge;
            // anything more involved hides its splitters until settled.
            if (splitters.size() == 1 && laid.size() == 2) {
                auto& sp = splitters[0];
                const bool vertical = sp.orientation == app::SplitOrientation::Vertical;
                const float d = vertical ? laid[0].second.right - first_final.right
                                         : laid[0].second.bottom - first_final.bottom;
                if (vertical) { sp.hit_rect.left += d; sp.hit_rect.right += d; }
                else { sp.hit_rect.top += d; sp.hit_rect.bottom += d; }
            } else {
                splitters.clear();
            }
        }
    }
    m.last.clear();
    for (const auto& [pane, r] : laid) m.last.emplace_back(pane, r);
}

void FillPaneSlots(AppState& s, ui::WindowViewModel& vm) {
    FillPaneLayoutControls(s, vm);
    D2D1_RECT_F content = s.renderer.ContentRect(
        static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    std::vector<std::pair<app::Pane*, D2D1_RECT_F>> laid;
    std::vector<app::SplitterLayout> splitters;
    if (Root(s)) app::LayoutWindowPanes(*Root(s), content, s.scale, laid, &splitters);
    if (const auto* layout = s.window_tabs.Active()) {
        if (auto* zoomed = ZoomedPane(*layout)) {
            laid.assign(1, {zoomed, content});
            splitters.clear();
        }
    }
    AnimatePaneLayout(s, content, laid, splitters);
    vm.pane_slots.clear();
    vm.splitters.clear();
    s.splitterNodes.clear();
    vm.filter_editing = s.filterEditing;
    if (app::Tab* tab = ActiveTab(s)) {
        std::wstring kind, rest;
        if (app::ParsePulsePath(tab->current_path, &kind, &rest) && kind == L"settings") {
            vm.settings_open = true;
            vm.settings_page = app::SettingsController::PageFromName(rest);
            vm.settings_scroll = s.settings.scroll();
            vm.settings_launch_on_startup = s.appPrefs.launch_on_startup;
            vm.settings_start_in_tray = s.appPrefs.start_in_tray;
            vm.settings_keep_running = s.appPrefs.keep_running_on_close;
            vm.settings_show_hidden_files = s.appPrefs.show_hidden_files;
            vm.settings_show_protected_os_files = s.appPrefs.show_protected_os_files;
            vm.settings_search_pinyin = s.appPrefs.search_pinyin;
            vm.settings_search_enabled = s.appPrefs.search_enabled;
            vm.settings_global_search_enabled = s.appPrefs.global_search_enabled;
            vm.settings_global_search_double_ctrl = s.appPrefs.global_search_double_ctrl;
            vm.settings_global_search_double_ctrl_ms = s.appPrefs.global_search_double_ctrl_ms;
            vm.settings_dialog_jump_bar = s.appPrefs.dialog_jump_bar;
            vm.settings_dialog_jump_auto = s.appPrefs.dialog_jump_auto;
            vm.settings_jump_history = s.appPrefs.jump_history;
            vm.settings_jump_history_empty = s.core.jumpHistory->Size() == 0;
            vm.settings_global_search_apps = s.appPrefs.global_search_apps;
            vm.settings_global_search_capturing = s.settings.global_search_hotkey_capturing();
            vm.settings_global_search_hotkey = s.settings.GlobalSearchHotkeyText();
            vm.settings_global_search_error = s.settings.global_search_error();
            vm.settings_content_status = ContentIndexStatusText(s);
            // Search off hides the index maintenance details on the search page.
            vm.settings_expanded = s.appPrefs.search_enabled ? s.settingsExpanded : (s.settingsExpanded & ~2u);
            FillShortcutsVm(s, vm);  // 快捷键: sidebar badge everywhere, the page view on page 7
            if (vm.settings_page == ui::kSettingsPageAi) pulse::FillAiSettingsVm(s, vm);   // 增强包 > AI 与 MCP
            pulse::FillAssistantSettingsVm(s, vm);   // AI 助手: the page, and the sidebar's 新
            if (vm.settings_page == 5) vm.settings_preview_codecs = ui::DetectPreviewCodecs(false, s.hwnd);
            if (vm.settings_page == 9) {
                const auto cache = s.settings.ThumbCache(s.hwnd);
                vm.settings_thumb_cache_phase = static_cast<uint8_t>(cache.phase);
                vm.settings_thumb_cache_bytes = cache.bytes;
                vm.settings_thumb_cache_cleared = cache.cleared;
            }
            if (vm.settings_page == 6) {
                // 系统右键菜单: the first visit starts a background scan.
                auto& sysctx = s.settings.SystemCtx();
                sysctx.SetNotifyWindow(s.hwnd);
                sysctx.EnsureLoaded();
                // 概览: Pulse's own handler timings, with the ones it skips.
                std::vector<ui::SysCtxHandlerTiming> timings;
                for (auto& [clsid, ms] : s.context_menu.ComHandlerTimes()) {
                    ui::SysCtxHandlerTiming timing;
                    timing.pulse_skipped = s.ctxMenuPrefs.ComDisabled(ipc::HandlerCatalogKey(clsid));
                    timing.clsid = std::move(clsid);
                    timing.ms = ms;
                    timings.push_back(std::move(timing));
                }
                sysctx.SetHandlerTimings(std::move(timings));
                // What each extension adds, so its row reads as menu text.
                sysctx.SetHandlerLabels(s.context_menu.ComHandlerLabels());
                // 只在 Pulse 隐藏: the rows' Pulse switch reads ContextMenuPrefs.
                sysctx.SetPulseHidden(s.ctxMenuPrefs.HiddenCatalogKeys());
                vm.settings_sysctx = sysctx.View();
            }
            {
                // 增强包 nav badge, on every page (the controller caches 2 s).
                const auto& packs = s.settings.Packs();
                vm.settings_pack_updates = int(!packs.update_version.empty()) + int(!packs.images_update_version.empty()) +
                                           int(!packs.raw_update_version.empty());
            }
            if (vm.settings_page == 8) {
                // 内容搜索 module: its install directory is read at most every 2 s.
                static uint64_t checked_at = 0;
                static int content_state = static_cast<int>(app::ContentPackState::Missing);
                const uint64_t now = GetTickCount64();
                if (!checked_at || now - checked_at >= 2000) {
                    content_state = static_cast<int>(app::QueryContentPackState());
                    checked_at = now;
                }
                vm.settings_content_pack_state = content_state;
                // The AI pack likewise, and again as soon as a download ends.
                static uint64_t ai_checked_at = 0;
                static bool ai_was_installing = false;
                static int ai_state = static_cast<int>(app::AiPackState::Missing);
                const bool ai_installing = AiPackInstalling();
                if (!ai_checked_at || now - ai_checked_at >= 2000 || ai_installing != ai_was_installing) {
                    ai_state = static_cast<int>(app::QueryAiPackState());
                    ai_checked_at = now;
                    ai_was_installing = ai_installing;
                }
                vm.settings_ai_pack_state = ai_state;
                vm.settings_ai_pack_installing = ai_installing;
                vm.settings_ai_pack_progress = ai_installing ? AiPackInstallProgress() : 0.0f;
            }
            if (vm.settings_page == 5 || vm.settings_page == 8) {
                // Cached in the controller: the disk is read at most every 2 s.
                const auto& packs = s.settings.Packs();
                vm.settings_pack_ffmpeg = packs.ffmpeg;
                vm.settings_pack_media_available = packs.media_available;
                vm.settings_pack_images_available = packs.images_available;
                vm.settings_pack_media_installed = packs.media_installed;
                vm.settings_pack_ffmpeg_enabled = packs.enabled;
                vm.settings_pack_use_custom = packs.use_custom;
                vm.settings_pack_remove_on_uninstall = packs.remove_on_uninstall;
                vm.settings_pack_auto_update = packs.auto_update;
                vm.settings_pack_installed = packs.installed;
                vm.settings_pack_bytes = packs.bytes;
                vm.settings_pack_version = packs.version;
                vm.settings_pack_custom_path = packs.custom_path;
                vm.settings_pack_detected_path = packs.detected_path;
                vm.settings_pack_root = packs.root;
                vm.settings_pack_notice = packs.notice;
                vm.settings_pack_installing = packs.installing;
                vm.settings_pack_progress = packs.progress;
                vm.settings_pack_update_version = packs.update_version;
                vm.settings_pack_images_installed = packs.images_installed;
                vm.settings_pack_images_enabled = packs.images_enabled;
                vm.settings_pack_images_version = packs.images_version;
                vm.settings_pack_images_notice = packs.images_notice;
                vm.settings_pack_images_installing = packs.images_installing;
                vm.settings_pack_images_progress = packs.images_progress;
                vm.settings_pack_images_update_version = packs.images_update_version;
                for (const auto* ext : {L".7z", L".rar"}) {
                    if (extensions::native::PreviewsAdapter::Available(s.hwnd, std::wstring(L"sample") + ext))
                        vm.settings_native_archive_extensions.emplace_back(ext);
                }
                vm.settings_pack_raw_available = packs.raw_available;
                vm.settings_pack_raw_installed = packs.raw_installed;
                vm.settings_pack_raw_enabled = packs.raw_enabled;
                vm.settings_pack_raw_version = packs.raw_version;
                vm.settings_pack_raw_notice = packs.raw_notice;
                vm.settings_pack_raw_installing = packs.raw_installing;
                vm.settings_pack_raw_progress = packs.raw_progress;
                vm.settings_pack_raw_update_version = packs.raw_update_version;
                vm.settings_pack_download_source = packs.download_source;
                for (int row = 0; row < app::kSourceProbeCount; ++row)
                    vm.settings_pack_speed_ms[row] = app::SourceProbeResult(row);
                if (vm.settings_pack_speed_ms[2] != app::kSourceProbeNotRun) vm.settings_pack_proxy = app::SourceProbeProxy();
                // Screenshot fixture for the downloading state (percent).
                wchar_t fake_progress[8]{};
                if (s.shot.active && s.isolatedTest &&
                    GetEnvironmentVariableW(L"PULSE_TEST_PACK_PROGRESS", fake_progress, ARRAYSIZE(fake_progress))) {
                    vm.settings_pack_installing = vm.settings_pack_images_installing =
                        vm.settings_pack_raw_installing = true;
                    vm.settings_pack_progress = vm.settings_pack_images_progress =
                        vm.settings_pack_raw_progress =
                        static_cast<float>(_wtoi(fake_progress)) / 100.0f;
                }
            }
            vm.settings_theme = s.themeOverride == ui::ThemeMode::Light ? 1 : s.themeOverride == ui::ThemeMode::Dark ? 2 : 0;
            const auto content_config = s.contentSearch.GetConfig();
            const auto content_status = s.contentSearch.GetStatus();
            vm.settings_content_paused = content_status.paused;
            vm.settings_content_instant = s.contentSearch.InstantMode();
            wchar_t content_summary[160]{};
            swprintf_s(content_summary,l10n::Get(l10n::StringId::SettingsContentSummaryFormat).c_str(),
                static_cast<unsigned long long>(content_config.roots.size()),static_cast<unsigned long long>(content_status.indexed_files));
            vm.settings_content_summary=content_summary;
            if (vm.settings_content_instant) vm.settings_content_summary = l10n::Get(l10n::StringId::ContentInstantReady);
            // Unscoped searches cover the user folders unless all drives was chosen (content_scope_mode.h).
            if (vm.settings_content_instant && content_config.shared_scope) {
                const bool all_drives = s.appPrefs.content_scope == 2;
                vm.settings_content_summary += l10n::IsChinese()
                    ? l10n::HantText(all_drives ? L"；范围：所有驱动器" : L"；范围：个人文件夹")
                    : std::wstring(all_drives ? L"; scope: all drives" : L"; scope: personal folders");
            }
            for (const auto& root : content_config.roots) {
                ui::WindowViewModel::ContentFolderView folder;
                folder.path = root.path;
                folder.error=content_status.error!=0;
                folder.status = l10n::Get(folder.error ? l10n::StringId::SettingsUnavailable :
                    content_status.paused ? l10n::StringId::ContentIndexPaused : l10n::StringId::ContentIndexBuilding);
                for (const auto& status : content_status.root_status) if (status.path == root.path) {
                    folder.error = status.error != 0 || !status.available;
                    folder.status = l10n::Get(folder.error ? l10n::StringId::SettingsUnavailable :
                        content_status.paused ? l10n::StringId::ContentIndexPaused :
                        status.indexing ? l10n::StringId::ContentIndexBuilding : l10n::StringId::ContentIndexReady);
                    if (status.error) folder.status += L" (" + std::to_wstring(status.error) + L")";
                    break;
                }
                if (vm.settings_content_instant) { folder.error = false; folder.status = l10n::Get(l10n::StringId::ContentInstantReady); }
                vm.settings_content_folders.push_back(std::move(folder));
            }
            vm.settings_open_folders = s.appPrefs.open_folders_in_pulse;
            vm.settings_win_e = s.appPrefs.take_over_win_e;
            vm.settings_this_pc = s.appPrefs.take_over_this_pc;
            vm.settings_recycle_bin = s.appPrefs.take_over_recycle_bin;
            vm.settings_explorer_windows = s.appPrefs.take_over_explorer_windows;
            vm.settings_default_manager =
                static_cast<int>(app::DefaultFileManagerState(s.appPrefs));
            vm.settings_default_manager_desc = app::DefaultFileManagerSummary(s.appPrefs);
            FillDialogManagementVm(s, vm);
            vm.settings_integration_enabled = s.appPrefs.integration_enabled;
            vm.settings_integration_folders = s.appPrefs.integration_folders;
            vm.settings_integration_win_e = s.appPrefs.integration_win_e;
            vm.settings_integration_this_pc = s.appPrefs.integration_this_pc;
            vm.settings_integration_recycle_bin = s.appPrefs.integration_recycle_bin;
            vm.settings_integration_experimental = s.appPrefs.take_over_explorer_windows;
            vm.settings_integration_folders_compat = s.appPrefs.integration_folders_compat;
            vm.settings_integration_state = s.settings.IntegrationState();
            vm.settings_integration_summary = s.settings.IntegrationSummary();
            vm.settings_integration_can_retry = s.settings.IntegrationCanRetry() ||
                vm.settings_integration_state == 2;
            vm.settings_integration_can_restore = s.settings.IntegrationCanRestore();
            vm.settings_integration_folders_active = s.appPrefs.open_folders_in_pulse;
            vm.settings_integration_this_pc_active = s.appPrefs.take_over_this_pc;
            vm.settings_integration_win_e_active = s.appPrefs.take_over_win_e;
            vm.settings_integration_just_enabled = s.settings.IntegrationJustEnabled();
            vm.settings_integration_other_exe = s.appPrefs.integration_other_exe;
            vm.settings_shell_tags = s.appPrefs.shell_tag_menu;
            vm.settings_alias_mark = app::AliasStore::Instance().ShowMark();
            vm.settings_ai_marks = s.settings.AssistantMarks();
            // Settings moves right of the docked 助手 like the panes do, so its
            // result card (撤销) stays beside the rows it changed.
            RECT client{};
            if (s.hwnd && GetClientRect(s.hwnd, &client))
                vm.settings_inset_left = s.renderer.AiDockWidth(static_cast<float>(client.right));
            vm.settings_blank_click_action = s.appPrefs.blank_click_action;
            vm.settings_preview_trigger = s.appPrefs.preview_trigger;
            vm.settings_hover_delay_ms = s.appPrefs.hover_preview_delay_ms;
            vm.settings_hover_card_size = s.appPrefs.hover_card_size;
            vm.settings_preview_size = s.appPrefs.preview_size;
            vm.settings_preview_remember = s.appPrefs.preview_remember_size;
            vm.settings_preview_space_closes = s.appPrefs.preview_space_closes;
            vm.settings_preview_loop = s.appPrefs.preview_loop;
            vm.settings_hold_preview_ms = s.appPrefs.hold_preview_ms;
            vm.settings_change_tracking = s.appPrefs.change_tracking_enabled;
            vm.settings_change_days = s.appPrefs.change_tracking_days;
            vm.settings_row_height = s.appPrefs.row_height;
            vm.settings_language = s.appPrefs.language == L"zh-CN" ? 1
                : s.appPrefs.language == L"zh-TW" ? 2
                : s.appPrefs.language == L"en-US" ? 3 : 0;
            wchar_t version_text[128]{};
            swprintf_s(version_text,
                l10n::Get(l10n::StringId::VersionFormat).c_str(), PULSE_VERSION_STRING);
            vm.settings_version = version_text;
            wchar_t build_text[256]{};
            swprintf_s(build_text,
                l10n::Get(l10n::StringId::BuildIdFormat).c_str(), PULSE_BUILD_ID);
            vm.settings_build_id = build_text;
            vm.settings_update_enabled = app::UpdateChecker::Enabled() ||
                s.shot.update_available;
            vm.settings_update_auto = s.appPrefs.auto_check_updates;
            vm.settings_update_checking = s.update_checker.checking();
            vm.settings_update_downloading = s.update_installer.Progress().active() &&
                !s.update_installer.installing();
            vm.settings_update_installing = s.update_installer.installing();
            DWORD update_install_error = s.update_install_error;
            if (s.shot.active) {
                const auto progress = UpdateProgressForView(s);
                vm.settings_update_downloading |= progress.active() && progress.phase != app::UpdatePhase::Installing;
                vm.settings_update_installing |= s.shot.update_state == L"installing";
                if (s.shot.update_state == L"cancelled") update_install_error = ERROR_CANCELLED;
                if (s.shot.update_state == L"failed") update_install_error = ERROR_CRC;
            }
            vm.settings_update_available = s.update_result_ready &&
                s.update_result.update_available;
            vm.settings_update_version = s.update_result.version;
            vm.settings_diagnostics_exporting = s.settings.diagnostics_pending();
            vm.settings_show_performance = s.appPrefs.show_status_performance;
            if (vm.settings_update_installing) {
                vm.settings_update_status = l10n::Get(l10n::StringId::InstallingUpdate);
            } else if (vm.settings_update_downloading) {
                vm.settings_update_status = app::UpdateProgressText(UpdateProgressForView(s));
                if (vm.settings_update_status.empty())
                    vm.settings_update_status = l10n::Get(l10n::StringId::DownloadingUpdate);
            } else if (update_install_error != ERROR_SUCCESS) {
                const auto message = update_install_error == ERROR_CANCELLED ? l10n::StringId::UpdateCancelled :
                    update_install_error == ERROR_BUSY ? l10n::StringId::UpdateBusy : l10n::StringId::UpdateInstallFailed;
                vm.settings_update_status = l10n::Get(message);
            } else if (vm.settings_update_checking) {
                vm.settings_update_status =
                    l10n::Get(l10n::StringId::CheckingUpdates);
            } else if (!vm.settings_update_enabled) {
                vm.settings_update_status = l10n::Get(l10n::StringId::UpdateDisabled);
            } else if (!s.update_result_ready) {
                vm.settings_update_status = l10n::Get(s.appPrefs.auto_check_updates ?
                    l10n::StringId::UpdateDesc : l10n::StringId::UpdateDescManual);
            } else if (s.update_result.error == app::UpdateError::UnsupportedWindows) {
                vm.settings_update_status =
                    l10n::Get(l10n::StringId::UpdateUnsupportedWindows);
            } else if (s.update_result.error != app::UpdateError::None) {
                vm.settings_update_status = l10n::Get(l10n::StringId::UpdateFailed);
            } else if (s.update_result.update_available) {
                wchar_t available[160]{};
                swprintf_s(available,
                    l10n::Get(l10n::StringId::UpdateAvailableFormat).c_str(),
                    s.update_result.version.c_str());
                vm.settings_update_status = available;
            } else {
                vm.settings_update_status = l10n::Get(l10n::StringId::UpdateUpToDate);
            }
            vm.settings_bloom = &s.bloom_accent;
            vm.settings_index_service = s.index.Connected() && s.index.ServiceMode();
            vm.settings_index_installed = s.settings.service_installed();
            // Index and network-index texts are Simplified (most come from
            // Pulse.Index.exe); localize them here, at the display boundary.
            vm.settings_index_status = l10n::ServiceText(s.index.Status());
            vm.settings_index_migrating = s.settings.migration_pending();
            if (vm.settings_page == 3) {
                const SettingsTransferView transfer = GetSettingsTransferView(s);
                vm.settings_transfer_busy = transfer.busy;
                vm.settings_transfer_pending = transfer.pending;
                vm.settings_transfer_backup = transfer.has_backup;
                vm.settings_transfer_backup_time = transfer.backup_time;
                vm.settings_about_rows = app::BuildAboutRows(vm.settings_index_service,
                                                             vm.settings_index_installed, s.scale);
                vm.settings_release_notes = &app::EmbeddedReleaseNotes();
                vm.settings_release_expanded = s.settingsReleaseExpanded;
            }
            if (s.shot.active) {
                wchar_t simulated[2]{};
                if (GetEnvironmentVariableW(L"PULSE_TEST_INDEX_MIGRATING", simulated, 2) == 1 &&
                    simulated[0] == L'1') vm.settings_index_migrating = true;
            }
            if (vm.settings_index_migrating)
                vm.settings_index_status = l10n::Get(l10n::StringId::IndexMigrating);
            vm.settings_index_path = s.index.IndexPath();
            vm.settings_index_error = l10n::ServiceText(s.settings.error());
            vm.settings_index_volumes.clear();
            vm.settings_index_excluded_paths = s.index.ExcludedPaths();
            {
                bool exclude_system = false;
                std::vector<std::wstring> groups;
                vm.settings_index_system_known = s.index.SystemExclusion(exclude_system, groups);
                vm.settings_index_exclude_system = exclude_system;
                vm.settings_index_system_groups = 0;
                for (const auto& group : groups)
                    for (size_t i = 0; i < std::size(index::kSystemExclusionGroups); ++i)
                        if (group == index::kSystemExclusionGroups[i])
                            vm.settings_index_system_groups |= 1u << i;
                vm.settings_index_system_expanded = s.settings.system_exclusion_expanded();
                wchar_t shot_expanded[4]{};
                const bool shot_force_expanded = s.shot.active &&
                    GetEnvironmentVariableW(L"PULSE_SHOT_SYSTEM_EXPANDED", shot_expanded, 4) &&
                    shot_expanded[0] == L'1';
                if (s.shot.active && vm.settings_page == 1 &&
                    (!vm.settings_index_system_known || shot_force_expanded)) {
                    // Shots show the defaults; PULSE_SHOT_SYSTEM_EXPANDED=1 opens the list
                    // even when a running service reports its own configuration.
                    vm.settings_index_system_known = true;
                    vm.settings_index_exclude_system = true;
                    vm.settings_index_system_groups = 0x17u;  // windows, temp, old, node_modules
                    vm.settings_index_system_expanded = shot_force_expanded;
                }
            }
            vm.settings_network_roots.clear();
            auto index_volumes = s.index.Volumes();
            if (s.shot.active && vm.settings_page == 1 && index_volumes.empty()) {
                index::IndexConfig defaults;
                index_volumes = index::EnumerateLocalVolumes(defaults);
                if (!vm.settings_index_installed) vm.settings_index_service = true;
                vm.settings_index_path = L"C:\\ProgramData\\Pulse\\Index";
            }
            for (const auto& volume : index_volumes) {
                ui::IndexVolumeRowView row;
                row.id = volume.id;
                row.title = volume.label.empty()
                    ? l10n::Get(l10n::StringId::LocalDisk) : volume.label;
                if (!volume.mount_point.empty()) {
                    row.title += L" (" + volume.mount_point.substr(0, 2) + L")";
                }
                row.detail = volume.file_system.empty() ? L"NTFS" : volume.file_system;
                row.detail += L" · " + l10n::Get(volume.kind == index::VolumeKind::Removable
                    ? l10n::StringId::RemovableDisk : l10n::StringId::FixedDisk);
                if (volume.indexed_items) {
                    wchar_t count[64]{};
                    swprintf_s(count, l10n::Get(l10n::StringId::ItemsCountFormat).c_str(),
                               static_cast<int>(volume.indexed_items));
                    row.detail += L" · ";
                    row.detail += count;
                }
                row.raw_state = volume.state;
                row.state = l10n::ServiceText(volume.state);
                row.checked = volume.enabled;
                row.enabled = vm.settings_index_service && volume.supported;
                row.pending = s.settings.VolumePending(volume.id);
                row.progress = volume.progress;
                vm.settings_index_volumes.push_back(std::move(row));
            }
            const auto network_roots = s.networkIndex.Roots();
            vm.settings_network_roots.reserve(network_roots.size());
            for (const auto& root : network_roots) {
                ui::NetworkRootRowView row;
                row.path = root.path;
                row.state = l10n::ServiceText(root.state);
                row.detail = l10n::ServiceText(root.error);
                row.online = root.online;
                row.building = root.building;
                vm.settings_network_roots.push_back(std::move(row));
            }
            if (s.shot.active && vm.settings_page == 1 && vm.settings_network_roots.empty()) {
                ui::NetworkRootRowView row;
                row.path = L"\\\\fileserver\\projects\\设计资料";
                row.state = l10n::ServiceText(L"已同步 · 128,420 项");
                row.online = true;
                vm.settings_network_roots.push_back(std::move(row));
            }
            static constexpr ipc::CtxMenuGroup kGroups[] = {
                ipc::CtxMenuGroup::Software, ipc::CtxMenuGroup::OpenWith,
                ipc::CtxMenuGroup::Share, ipc::CtxMenuGroup::System, ipc::CtxMenuGroup::Print
            };
            for (int g = 0; g < 5; ++g)
                vm.settings_group_on[g] = s.ctxMenuPrefs.GroupEnabled(kGroups[g]);
            vm.settings_items.clear();
            vm.settings_items.reserve(s.ctxMenuPrefs.seen.size() + app::kBuiltinMenuItemCount);
            for (const auto& seen : s.ctxMenuPrefs.seen) {
                ui::SettingsRowView row;
                row.key = seen.key;
                // Pulse's own compress row is stored in Simplified; shell rows
                // already use the system language.
                row.text = seen.key == ipc::CompressCatalogKey()
                    ? std::wstring(l10n::Cn(ipc::CompressCatalogText())) : seen.text;
                row.group = static_cast<int>(ipc::GroupOf(seen.category));
                row.on = s.ctxMenuPrefs.RowEnabled(seen.key, seen.category, seen.from_com);
                // A row the slow-extension auto-disable turned off (#77) shows
                // why it is missing from the menu; toggling it recovers.
                row.slow_disabled = s.ctxMenuPrefs.ComDisabled(seen.key);
                vm.settings_items.push_back(std::move(row));
            }
            // Pulse's own commands: card 5, after the seen catalog (see
            // SettingsController::ToggleUi).
            vm.settings_builtin_first = vm.settings_items.size();
            vm.settings_builtin_hidden = s.ctxMenuPrefs.builtin_hidden;
            vm.settings_context_tab = s.settings.ContextTab();
            static const int shot_context_tab = [] {
                wchar_t v[4]{};
                const DWORD n = GetEnvironmentVariableW(L"PULSE_SHOT_CONTEXT_TAB", v, 4);
                return n == 1 && v[0] >= L'0' && v[0] <= L'2' ? v[0] - L'0' : -1;
            }();
            if (shot_context_tab >= 0) vm.settings_context_tab = shot_context_tab;
            for (int i = 0; i < static_cast<int>(app::BuiltinMenuSurface::Count); ++i)
                vm.settings_builtin_order[i] = s.ctxMenuPrefs.BuiltinOrder(static_cast<app::BuiltinMenuSurface>(i));
            if (s.settings.menu_drag_live()) {
                vm.settings_menu_drag = s.settings.menu_drag_item();
                vm.settings_menu_drag_pos = s.settings.menu_drag_pos();
                vm.settings_menu_drag_grab = s.settings.menu_drag_grab();
            }
            for (int i = 0; i < app::kBuiltinMenuItemCount; ++i) {
                const auto item = static_cast<app::BuiltinMenuItem>(i);
                ui::SettingsRowView row;
                row.key = app::BuiltinMenuKey(item);
                row.text = app::BuiltinMenuLabel(item);
                row.group = 5;
                row.on = s.ctxMenuPrefs.BuiltinVisible(item);
                vm.settings_items.push_back(std::move(row));
            }
            if (vm.status.status_text.empty())
                vm.status.status_text = l10n::Get(l10n::StringId::Settings);

            vm.dup_scope = static_cast<int>(s.duplicateScan.scope);
            vm.dup_folder = s.duplicateScan.folder_path;
            vm.dup_min_size = s.duplicateScan.minimum_file_bytes <= 1024 ? 0
                : s.duplicateScan.minimum_file_bytes <= 1024ull * 1024ull ? 1 : 2;
            vm.dup_scanning = s.duplicateScan.scanning;
            vm.dup_hint = l10n::Get(l10n::StringId::DupHint);
            if (vm.settings_page == 4) {
            RequestDuplicateVolumeCache(s);
            const auto& volumes = s.dup_volume_cache;
            vm.dup_drives.clear();
            vm.dup_drives.reserve(volumes.size());
            std::wstring selected_drive =
                app::DuplicateScanSession::NormalizeDriveRoot(s.duplicateScan.drive_root);
            if (selected_drive.empty()) {
                for (const auto& volume : volumes) {
                    if (volume.mount_point.empty()) continue;
                    selected_drive =
                        app::DuplicateScanSession::NormalizeDriveRoot(volume.mount_point);
                    break;
                }
            }
            for (const auto& volume : volumes) {
                if (volume.mount_point.empty()) continue;
                ui::DuplicateDriveView row;
                row.root = app::DuplicateScanSession::NormalizeDriveRoot(volume.mount_point);
                row.label = row.root.size() >= 2 ? row.root.substr(0, 2) : row.root;
                if (!volume.label.empty()) {
                    row.label += L" ";
                    row.label += volume.label;
                }
                row.selected = CompareStringOrdinal(row.root.c_str(), -1,
                    selected_drive.c_str(), -1, TRUE) == CSTR_EQUAL;
                vm.dup_drives.push_back(std::move(row));
            }
            const auto roots = app::DuplicateScanSession::ResolveRoots(
                s.duplicateScan.scope, s.duplicateScan.folder_path,
                s.duplicateScan.drive_root.empty() ? selected_drive : s.duplicateScan.drive_root,
                volumes);
            vm.dup_can_scan = s.duplicateScan.scope == app::DuplicateScanScope::Folder ||
                              !roots.empty();
            vm.dup_show_progress = s.duplicateScan.scanning;
            vm.dup_progress_indeterminate =
                s.duplicateScan.phase == index::ContentSearchPhase::Enumerating ||
                s.duplicateScan.total_files == 0;
            vm.dup_progress_value = s.duplicateScan.total_files
                ? static_cast<float>(s.duplicateScan.scanned_files) /
                  static_cast<float>(s.duplicateScan.total_files)
                : 0.0f;
            vm.dup_animation = static_cast<float>(
                std::fmod(static_cast<double>(GetTickCount64()), 1952.0) / 1952.0);
            std::wstring root_label = s.duplicateScan.current_root;
            if (root_label.size() >= 2 && root_label[1] == L':')
                root_label = root_label.substr(0, 2);
            if (root_label.empty() && s.duplicateScan.scope == app::DuplicateScanScope::Drive)
                root_label = s.duplicateScan.drive_root.size() >= 2
                    ? s.duplicateScan.drive_root.substr(0, 2) : s.duplicateScan.drive_root;
            wchar_t status[192]{};
            if (s.duplicateScan.phase == index::ContentSearchPhase::Hashing &&
                s.duplicateScan.total_files) {
                swprintf_s(status, l10n::Get(l10n::StringId::DupHashingFormat).c_str(),
                           format::GroupedInt(s.duplicateScan.scanned_files).c_str(),
                           format::GroupedInt(s.duplicateScan.total_files).c_str());
            } else {
                swprintf_s(status, l10n::Get(l10n::StringId::DupListingFormat).c_str(),
                           root_label.empty() ? L"—" : root_label.c_str(),
                           format::GroupedInt(s.duplicateScan.scanned_files).c_str());
            }
            vm.dup_status = status;
            wchar_t speed[128]{};
            if (s.duplicateScan.phase == index::ContentSearchPhase::Hashing) {
                wchar_t mb[32]{};
                swprintf_s(mb, L"%.1f", s.duplicateScan.megabytes_per_second);
                swprintf_s(speed, l10n::Get(l10n::StringId::DupHashSpeedFormat).c_str(),
                           mb, format::GroupedInt(static_cast<uint64_t>(
                               s.duplicateScan.files_per_second + 0.5)).c_str());
            } else {
                swprintf_s(speed, l10n::Get(l10n::StringId::DupFilesPerSecFormat).c_str(),
                           format::GroupedInt(static_cast<uint64_t>(
                               s.duplicateScan.files_per_second + 0.5)).c_str());
            }
            vm.dup_speed = speed;
            vm.dup_empty.clear();
            if (!s.duplicateScan.scanning && s.duplicateScan.completed) {
                if (s.duplicateScan.groups.empty() &&
                    s.duplicateScan.error == ERROR_SUCCESS)
                    vm.dup_empty = l10n::Get(l10n::StringId::DupNoResults);
                if (s.duplicateScan.truncated)
                    vm.dup_empty = l10n::Get(l10n::StringId::DupTruncated);
                if (s.duplicateScan.error != ERROR_SUCCESS &&
                    s.duplicateScan.error != ERROR_CANCELLED) {
                    wchar_t error[64]{};
                    swprintf_s(error, l10n::Get(l10n::StringId::ErrorCodeFormat).c_str(),
                               s.duplicateScan.error);
                    vm.dup_empty = error;
                }
            }
            if (s.dup_view_epoch != s.duplicateScan.result_epoch ||
                s.dup_view_language != s.appPrefs.language) {
                RefreshDuplicateGroupViews(s);
            }
            vm.dup_groups = s.dup_view_cache;
            const size_t extras = s.duplicateScan.ExtraCount();
            vm.dup_show_delete_all = extras > 0 && !s.duplicateScan.scanning;
            if (vm.dup_show_delete_all) {
                vm.dup_delete_all = l10n::Get(l10n::StringId::DupDeleteAllExtras);
                vm.dup_delete_all += L" · ";
                vm.dup_delete_all += format::GroupedInt(extras);
            }
            }
        }
    }
    for (const auto& sp : splitters) {
        ui::SplitterView view;
        view.hit_rect = sp.hit_rect;
        view.parent_bounds = sp.parent_bounds;
        view.vertical = (sp.orientation == app::SplitOrientation::Vertical);
        view.ratio = sp.node ? sp.node->ratio : 0.5f;
        view.snapped = s.splitterDragging && s.splitterSnap > 0.0f &&
                       s.splitterDragIndex == static_cast<int>(vm.splitters.size());
        if (splitters.size() == 1 && laid.size() == 2) {
            // Level with the pane headers for left/right, centered for top/bottom.
            const float r = 16.0f * s.scale;
            const float cx = view.vertical ? (sp.hit_rect.left + sp.hit_rect.right) * 0.5f
                                           : (sp.parent_bounds.left + sp.parent_bounds.right) * 0.5f;
            const float cy = view.vertical ? sp.parent_bounds.top + 29.0f * s.scale
                                           : (sp.hit_rect.top + sp.hit_rect.bottom) * 0.5f;
            view.swap_rect = D2D1::RectF(cx - r, cy - r, cx + r, cy + r);
        }
        vm.splitters.push_back(view);
        s.splitterNodes.push_back(sp.node);
    }
    for (size_t i = 0; i < laid.size(); ++i) {
        ui::PaneSlotView slot;
        slot.rect = laid[i].second;
        app::Pane* p = laid[i].first;
        slot.focused = (p == s.pane);
        slot.target = (p && p == s.targetPane);
        if (p) {
            // BuildWindowViewModel already populated the focused pane. Reuse
            // it instead of rebuilding all snapshot-derived data twice.
            if (slot.focused) slot.pane = vm.pane;
            else app::FillPaneViewModel(slot.pane, *p, &s.places);
            if (app::Tab* tab = p->ActiveTab()) {
                FillChangePane(s, *tab, slot.pane, slot.rect);
                FillGitPane(s, *tab, slot.pane);
                FillSyncPane(s, *tab, slot.pane);   // SVN marks (outside Git) and OneDrive states
                const float wall_scroll_before = slot.pane.scroll_y;
                s.renderer.ReconcileWallAnchor(slot.pane, slot.rect);
                if (slot.focused && s.scrollAnimating)
                    s.scrollTargetY += slot.pane.scroll_y - wall_scroll_before;
                tab->scroll_y = slot.pane.scroll_y;
                if (slot.focused) vm.pane.scroll_y = slot.pane.scroll_y;
                if (slot.focused) {
                    vm.pane.git_marks = slot.pane.git_marks;
                    vm.pane.onedrive_status = slot.pane.onedrive_status;
                    AppendGitStatusText(*tab, vm.status.status_text);
                    AppendSvnStatusText(s, *tab, vm.status.status_text);
                }
                for (const auto& cutPath : s.cutPaths) {
                    if (fs::ParentPath(cutPath) != tab->current_path) continue;
                    const size_t slash = cutPath.find_last_of(L"\\/");
                    slot.pane.cut_names.insert(slash == std::wstring::npos
                        ? cutPath : cutPath.substr(slash + 1));
                }
                if (!slot.focused) FillLeavingRows(s, tab->current_path, slot.pane);
                slot.pane.ai_list = AiListOverlayFor(tab->current_path);   // Pulse 助手 at work here
                if (!slot.focused) FillLandingRows(s, p, tab->current_path, slot.pane);
            }
            if (static_cast<int>(i) == s.hoverPaneIndex) slot.pane.hover_index = s.hoverRow;
            // Hover shots stage the row on "the focused pane" (-1).
            else if (s.shot.active && s.hoverPaneIndex < 0 && slot.focused) slot.pane.hover_index = s.hoverRow;
            if (s.stripResizing && static_cast<int>(i) == s.stripResizePane)
                slot.pane.column_strip.resize_column = s.stripResizeColumn;
            if (s.stripHScrolling && static_cast<int>(i) == s.stripResizePane)
                slot.pane.column_strip.hscroll_pressed = true;
            if (s.dragFilesOver && !slot.pane.path.empty() && !fs::IsVirtualPath(slot.pane.path)) {
                slot.pane.drag_parent_path = fs::ParentPath(slot.pane.path);
                if (slot.pane.drag_parent_path == slot.pane.path) slot.pane.drag_parent_path.clear();
            }
            slot.pane.drop_parent = s.dropParent && (static_cast<int>(i) == s.dropPaneIndex || (s.dropPaneIndex < 0 && slot.focused));
            if (slot.pane.drop_parent && s.springStart)
                slot.pane.drop_parent_progress = (std::min)(1.0f, float(GetTickCount64() - s.springStart) / 800.0f);
            if (static_cast<int>(i) == s.dropPaneIndex) {
                slot.pane.drop_target_index = s.dropRow;
                slot.pane.header_drop = s.dropHeader;
                slot.pane.drop_spring_progress = DropSpringProgress(s);
            }
            if (slot.focused) {
                slot.pane.rename_index = s.renameIndex;
                slot.pane.rename_text = RenameEditText(s);
                FillRenameHint(s, slot.pane);
                if (s.listSelection.BandActive()) {
                    slot.pane.marquee_active = true;
                    slot.pane.marquee_rect = MarqueeDisplayRect(s);
                }
            }
        }
        vm.pane_slots.push_back(std::move(slot));
    }
    s.renderer.PruneWallViews(vm.pane_slots);
}
// ---------------------------------------------------------------------------
// Details panel helpers: star shortcuts, byte/time formatting, size walk.
// ---------------------------------------------------------------------------
// Starred projects are path records in places.json, not copies or .lnk shortcuts.
void RefreshStarredViews(AppState& s) {
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (!tab) return;
        std::wstring kind;
        if (app::ParsePulsePath(tab->current_path, &kind, nullptr) && kind == L"starred")
            LoadVirtualView(s, *tab, tab->current_path);
    });
}

void RefreshRecentViews(AppState& s) {
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (!tab) return;
        std::wstring kind;
        if (app::ParsePulsePath(tab->current_path, &kind, nullptr) && kind == L"recent")
            LoadVirtualView(s, *tab, tab->current_path);
    });
}

bool IsRecycleTab(const app::Tab* tab) {
    std::wstring kind;
    return tab && app::ParsePulsePath(tab->current_path, &kind, nullptr) && kind == L"recycle";
}

std::wstring RecycleOccupancyText(const fs::RecycleBinInfo& info) {
    if (!info.valid) return {};
    wchar_t buf[96]{};
    swprintf_s(buf, l10n::Get(l10n::StringId::RecycleOccupancyFormat).c_str(),
               std::to_wstring(info.items).c_str(),
               pulse::format::ByteSize(info.bytes).c_str());
    return buf;
}

void ApplyRecycleOccupancy(AppState& s) {
    const std::wstring detail = RecycleOccupancyText(s.recycle_info);
    for (auto& entry : s.sidebar.quick_access) {
        if (entry.path == app::MakeRecyclePath()) {
            entry.detail = detail;
            break;
        }
    }
    app::ForEachWindowSharingCore(s, [&](AppState& window) {
        ForEachPane(window, [&](app::Pane& pane) {
            app::Tab* tab = pane.ActiveTab();
            if (!IsRecycleTab(tab)) return;
            tab->banner_title = l10n::Get(l10n::StringId::RecycleBin);
            tab->banner_message = detail;
        });
        if (window.hwnd) InvalidateRect(window.hwnd, nullptr, FALSE);
    });
}

void RequestRecycleOccupancy(AppState& s) {
    const HWND hwnd = ServiceWindow(s);
    if (!hwnd) return;
    auto& gate = s.core.recycle_refresh_gate;
    const uint64_t request = gate.Begin();
    if (!request) return;
    auto& watch = s.core.recycle_shell_watch;
    if (!watch || watch->owner != hwnd) {
        if (watch) watch->Cancel();
        watch = std::make_shared<app::RecycleShellWatch>(hwnd);
    }
    s.worker.EnqueueIo([hwnd, request, watch] {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        struct Uninitialize { HRESULT result; ~Uninitialize() { if (SUCCEEDED(result)) CoUninitialize(); } } uninitialize{apartment};
        watch->Register(WM_RECYCLE_SHELL_CHANGED);
        auto* info = new fs::RecycleBinInfo{};
        fs::QueryRecycleBinInfo(*info);
        if (PostMessageW(hwnd, WM_RECYCLE_INFO, static_cast<WPARAM>(request), reinterpret_cast<LPARAM>(info)))
            return;
        delete info;
    });
}

bool ApplyQueriedRecycleInfo(AppState& s, const fs::RecycleBinInfo& info, uint64_t request) {
    if (request) {
        const auto completion = s.core.recycle_refresh_gate.Complete(request);
        if (!completion.matched) return false;
        if (!completion.current) {
            s.recycle_refresh_left = (std::max)(s.recycle_refresh_left, 1);
            if (!s.recycle_refresh_at) s.recycle_refresh_at = GetTickCount64() + 300;
            return false;
        }
    }
    if (!info.valid) return false;
    if (s.recycle_info_guard && info.items == s.recycle_ignore_items &&
        s.recycle_info.valid && s.recycle_info.items != s.recycle_ignore_items) return false;
    s.recycle_info = info;
    if (info.items != s.recycle_ignore_items) s.recycle_info_guard = false;
    ApplyRecycleOccupancy(s);
    return true;
}

void OnRecycleShellChanged(AppState& s, WPARAM wparam, LPARAM lparam) {
    PIDLIST_ABSOLUTE* pidls = nullptr;
    LONG event = 0;
    HANDLE lock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wparam), static_cast<DWORD>(lparam),
        &pidls, &event);
    if (!lock) return;
    SHChangeNotification_Unlock(lock);
    s.core.recycle_refresh_gate.Invalidate();
    s.recycle_refresh_left = 3;
    if (!s.recycle_refresh_at) s.recycle_refresh_at = GetTickCount64() + 300;
}

void ReleaseRecycleShellWatch(AppState& s, HWND hwnd) {
    auto& watch = s.core.recycle_shell_watch;
    if (watch && watch->owner == hwnd) {
        watch->Cancel();
        watch.reset();
        auto& gate = s.core.recycle_refresh_gate;
        gate.Invalidate();
        gate.active_request = 0;
        gate.visible = false;
    }
}

void RefreshRecycleViews(AppState& s, bool query_occupancy) {
    RefreshRecycleSearches(s);
    RefreshPath(s, app::MakeRecyclePath());
    if (query_occupancy) RequestRecycleOccupancy(s);
}

void ScheduleRecycleRefresh(AppState& s) {
    s.core.recycle_refresh_gate.Invalidate();
    s.recycle_ignore_items = s.recycle_info.valid ? s.recycle_info.items : 0;
    s.recycle_info_guard = true;
    s.recycle_refresh_left = 3;
    const ULONGLONG now = GetTickCount64();
    if (s.recycle_refresh_at == 0)
        s.recycle_refresh_at = now + 300;
}

void BumpRecycleOccupancy(AppState& s, int64_t delta) {
    if (!s.recycle_info.valid) {
        s.recycle_info.valid = true;
        s.recycle_info.items = 0;
        s.recycle_info.bytes = 0;
    }
    if (delta > 0) {
        s.recycle_info.items += static_cast<uint64_t>(delta);
    } else if (delta < 0) {
        const uint64_t sub = static_cast<uint64_t>(-delta);
        s.recycle_info.items = s.recycle_info.items > sub ? s.recycle_info.items - sub : 0;
        if (s.recycle_info.items == 0) s.recycle_info.bytes = 0;
    }
    ApplyRecycleOccupancy(s);
}

void ClearRecycleOccupancy(AppState& s) {
    s.recycle_info.valid = true;
    s.recycle_info.items = 0;
    s.recycle_info.bytes = 0;
    ApplyRecycleOccupancy(s);
}

void ApplyEmptyRecycleFinished(AppState& s, bool emptied) {
    // The bin is emptied in place, so the Shell count is already final: drop
    // the stale-count guard and query now. A cancelled or partial empty keeps
    // its old figure until that query reports what is left.
    s.recycle_info_guard = false;
    if (emptied) ClearRecycleOccupancy(s);
    RefreshRecycleViews(s, true);
}

bool PumpRecycleRefresh(AppState& s, ULONGLONG now) {
    auto& gate = s.core.recycle_refresh_gate;
    bool visible = false;
    app::ForEachWindowSharingCore(s, [&](AppState& window) {
        if (!window.hwnd || !IsWindowVisible(window.hwnd) || IsIconic(window.hwnd)) return;
        for (const auto& entry : window.sidebar.quick_access)
            if (entry.path == app::MakeRecyclePath()) { visible = true; break; }
        ForEachPane(window, [&](app::Pane& pane) { visible |= IsRecycleTab(pane.ActiveTab()); });
    });
    const bool became_visible = visible && !gate.visible;
    gate.visible = visible;
    if (!visible) return false;
    const bool needs_fallback = !s.core.recycle_shell_watch || !s.core.recycle_shell_watch->Active() ||
        !s.recycle_info.valid;
    // Reconcile on return to a visible window. Only a failed Shell registration
    // falls back to a minute poll; background windows never scan all bins.
    if (became_visible || (needs_fallback && now >= gate.fallback_at)) {
        gate.fallback_at = now + 60000;
        s.recycle_info_guard = false;
        RequestRecycleOccupancy(s);
    }
    if (s.recycle_refresh_left <= 0 || !s.recycle_refresh_at || now < s.recycle_refresh_at) return false;
    // A final settled query must be authoritative, including an external change
    // that happens to restore the pre-mutation count.
    if (s.recycle_refresh_left == 1) s.recycle_info_guard = false;
    RefreshRecycleViews(s, true);
    --s.recycle_refresh_left;
    if (s.recycle_refresh_left > 0)
        s.recycle_refresh_at = now + (s.recycle_refresh_left == 2 ? 900ull : 1600ull);
    else s.recycle_refresh_at = 0;
    return true;
}

bool ToggleStarred(AppState& s, const std::wstring& target,
                          app::PlaceItemKind kind) {
    if (target.empty() || fs::IsVirtualPath(target)) return false;
    const bool on = s.places.ToggleStarred(target, kind);
    s.detailsStarred = on;
    POINT cursor{};
    RECT client{};
    if (on && s.hwnd && GetCursorPos(&cursor) && ScreenToClient(s.hwnd, &cursor) &&
        GetClientRect(s.hwnd, &client) && PtInRect(&client, cursor))
        s.renderer.LaunchStarFlight(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    RefreshStarredViews(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return on;
}

void StopDetailsSizeWalk(AppState& s) {
    s.detailsSizeGeneration.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(s.detailsSizeMutex);
        s.detailsSizeRequested.clear();
        s.detailsSize = AppState::DetailsSizeResult{};
    }
    s.detailsSizeCv.notify_one();
}

void StartDetailsSizeWalk(AppState& s, const std::wstring& path) {
    const uint64_t generation =
        s.detailsSizeGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        std::lock_guard<std::mutex> lock(s.detailsSizeMutex);
        s.detailsSize = AppState::DetailsSizeResult{};
        s.detailsSize.path = path;
        s.detailsSizeRequested = path;
    }
    if (!s.detailsSizeThread.joinable()) {
        AppState* sp = &s;
        s.detailsSizeThread = std::thread([sp] {
            uint64_t processed = 0;
            for (;;) {
                std::wstring request;
                uint64_t requestGeneration = 0;
                {
                    std::unique_lock<std::mutex> lock(sp->detailsSizeMutex);
                    sp->detailsSizeCv.wait(lock, [&] {
                        return sp->detailsSizeStop ||
                            (!sp->detailsSizeRequested.empty() &&
                             sp->detailsSizeGeneration.load(std::memory_order_relaxed) != processed);
                    });
                    if (sp->detailsSizeStop) return;
                    request = sp->detailsSizeRequested;
                    requestGeneration =
                        sp->detailsSizeGeneration.load(std::memory_order_relaxed);
                    processed = requestGeneration;
                }
                uint64_t version = 0;
                for (;;) {
                    const auto cancelled = [sp, requestGeneration] {
                        return sp->detailsSizeGeneration.load(std::memory_order_relaxed) != requestGeneration;
                    };
                    try {
                        const auto result = app::SharedFolderStatistics().Read(request, cancelled, version);
                        if (cancelled()) break;
                        version = result.version;
                        {
                            std::lock_guard<std::mutex> lock(sp->detailsSizeMutex);
                            if (sp->detailsSizeRequested != request || cancelled()) break;
                            sp->detailsSize = {request, result.bytes, result.files, result.directories, true, result.partial};
                        }
                        if (sp->hwnd) InvalidateRect(sp->hwnd, nullptr, FALSE);
                    } catch (...) { break; }
                }
            }
        });
    }
    (void)generation;
    s.detailsSizeCv.notify_one();
}

void ShutdownDetailsSizeWalk(AppState& s) {
    s.detailsSizeGeneration.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(s.detailsSizeMutex);
        s.detailsSizeStop = true;
        s.detailsSizeRequested.clear();
    }
    s.detailsSizeCv.notify_one();
    if (s.detailsSizeThread.joinable()) s.detailsSizeThread.join();
}

std::wstring DetailsAttributeText(DWORD attrs) {
    struct AttributeName { DWORD bit; const wchar_t* name; };
    const AttributeName values[] = {
        { FILE_ATTRIBUTE_READONLY, pulse::l10n::Get(pulse::l10n::StringId::AttrReadOnly).c_str() },
        { FILE_ATTRIBUTE_HIDDEN, pulse::l10n::Get(pulse::l10n::StringId::AttrHidden).c_str() },
        { FILE_ATTRIBUTE_SYSTEM, pulse::l10n::Get(pulse::l10n::StringId::AttrSystem).c_str() },
        { FILE_ATTRIBUTE_COMPRESSED, pulse::l10n::Get(pulse::l10n::StringId::AttrCompressed).c_str() },
        { FILE_ATTRIBUTE_ENCRYPTED, pulse::l10n::Get(pulse::l10n::StringId::AttrEncrypted).c_str() },
        { FILE_ATTRIBUTE_REPARSE_POINT, pulse::l10n::Get(pulse::l10n::StringId::AttrReparse).c_str() },
    };
    std::wstring result;
    for (const auto& value : values) {
        if (!(attrs & value.bit)) continue;
        if (!result.empty()) result += pulse::l10n::IsChinese() ? L"、" : L", ";
        result += value.name;
    }
    return result.empty() ? pulse::l10n::Get(pulse::l10n::StringId::AttrNormal).c_str() : result;
}

static bool HoverHintIsCommand(ui::HitTestResult::Region region) {
    using R = ui::HitTestResult;
    switch (region) {
    case R::None:
    case R::Row:
    case R::Pane:
    case R::Tab:
    case R::SidebarItem:
    case R::SidebarHeader:
    case R::AddressBar:
    case R::ColumnHeader:
    case R::StatusBar:
    case R::StatusBarTask:
    case R::StatusHintAction:
        return false;
    default:
        return true;
    }
}

// The status-bar hint answers "what can I do right now?". Priority follows the
// approved mockup: drag > compare > multi-select > read-only > selection >
// search > two panes > idle. `action` receives an optional clickable follow-up.
static std::wstring ContextStatusHint(AppState& s, StatusHintAction& action) {
    using I = l10n::StringId;
    action = StatusHintAction::None;
    const app::Tab* tab = ActiveTab(s);
    if (!tab) return {};
    if (IsSettingsTab(tab)) return l10n::Get(I::Settings);
    if (!s.appPrefs.show_hints) return {};
    if (!s.dropBadge.empty())
        return l10n::Get(I::HintDrag);
    if (s.addressSearching) {
        // Typing a search: surface the keys the search box understands.
        const auto sep = L"  \u00B7  ";
        std::wstring hint = l10n::Get(I::HintKeyEnterSearch);
        if (s.addressSearchCurrent) hint += sep + l10n::Get(I::HintKeyShiftEnter);
        if (!s.addressSearchContent) hint += sep + l10n::Get(I::HintKeyAltEnter);
        hint += sep + l10n::Get(I::HintKeyTabScope);
        hint += sep + l10n::Get(IsAddressSearchResults(tab) ? I::HintKeyDownResults : I::HintKeyDownHistory);
        action = StatusHintAction::Advanced;
        return hint;
    }
    if (IsRecycleTab(tab)) return l10n::Get(I::StatusHintRecycle);
    const app::LayoutTab* layout = s.window_tabs.Active();
    if (layout && layout->compare) {
        action = layout->compare_diff_only ? StatusHintAction::ShowAll : StatusHintAction::DiffOnly;
        return l10n::Get(I::HintCompare);
    }
    std::wstring kind;
    app::ParsePulsePath(tab->current_path, &kind, nullptr);
    const int selected = tab->SelectedCount();
    if (s.filterEditing && !s.filterSelectMode) {
        // While typing a filter the selection may be hidden; explain the filter instead.
        if (s.appPrefs.search_enabled && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path))
            action = StatusHintAction::SearchSubfolders;
        return l10n::Get(I::HintFiltering);
    }
    if (tab->net_readonly) return l10n::Get(I::HintReadonly2);
    if (selected == 1) {
        const int index = tab->selected_index;
        const bool folder = tab->snapshot && index >= 0 &&
            static_cast<size_t>(index) < tab->snapshot->size() && (*tab->snapshot)[index].is_dir;
        if (kind == L"search") {
            action = StatusHintAction::OpenPath;
            return l10n::Get(I::HintSearchResultSel);
        }
        return l10n::Get(folder ? I::HintFolderSel : I::StatusHintSelected);
    }
    if (!tab->filter_text.empty() && !s.filterSelectMode) {
        // Pane filter vs. search: say that subfolders are not included.
        if (s.appPrefs.search_enabled && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path))
            action = StatusHintAction::SearchSubfolders;
        return l10n::Get(I::HintFiltering);
    }
    if (tab->current_path == kNetworkLocationsPath) {
        // 网络位置: what discovery found and when, with 重新发现 (design §06).
        std::wstring lan = NetworkLanHint(s);
        if (!lan.empty()) {
            action = StatusHintAction::LanRescan;
            return lan;
        }
    }
    if (kind == L"search") {
        action = StatusHintAction::Advanced;
        return l10n::Get(I::HintSearchSmart);
    }
    if (layout && layout->panes.size() >= 2) {
        action = StatusHintAction::Compare;
        return l10n::Get(I::HintSplit);
    }
    return l10n::Get(I::StatusIntroHint);
}

static const std::wstring& StatusHintActionText(StatusHintAction action) {
    using I = l10n::StringId;
    switch (action) {
    case StatusHintAction::Compare: return l10n::Get(I::HintActCompare);
    case StatusHintAction::DiffOnly: return l10n::Get(I::HintActDiffOnly);
    case StatusHintAction::ShowAll: return l10n::Get(I::HintActShowAll);
    case StatusHintAction::Advanced: return l10n::Get(I::HintActAdvanced);
    case StatusHintAction::Shortcuts: return l10n::Get(I::HintActShortcuts);
    case StatusHintAction::OpenPath: return l10n::Get(I::HintActOpenPath);
    case StatusHintAction::SearchSubfolders: return l10n::Get(I::HintActSearchSub);
    case StatusHintAction::LanRescan: return l10n::Get(I::HintActLanRescan);
    default: break;
    }
    static const std::wstring kEmpty;
    return kEmpty;
}

// ---- One-time teaching bubbles -------------------------------------------
// Tip ids double as tips_seen bits: 0 middle-click, 1 retired (the staging tray), 2 compare,
// 3 smart search, 4 collapsed sidebar, 5 quick preview.
namespace {
constexpr int kTeachCount = 6;
constexpr ULONGLONG kTeachHoldMs = 700;      // trigger must hold this long
constexpr ULONGLONG kTeachMinShowMs = 3000;  // before "moved on" dismissal
constexpr ULONGLONG kTeachMaxShowMs = 15000;

bool TeachBusy(const AppState& s) {
    return !s.dropBadge.empty() || s.renameIndex >= 0 || s.addressEditing || s.addressSearching ||
           s.filterEditing || !s.tagRenameId.empty() || s.listSelection.BandActive() || GetCapture() != nullptr ||
           GetForegroundWindow() != s.hwnd;
}

int TeachTrigger(AppState& s) {
    const app::Tab* tab = ActiveTab(s);
    if (!tab || IsSettingsTab(tab)) return -1;
    const app::LayoutTab* layout = s.window_tabs.Active();
    const int selected = tab->SelectedCount();
    std::wstring kind;
    app::ParsePulsePath(tab->current_path, &kind, nullptr);
    const bool compare = layout && layout->compare;
    if (layout && layout->panes.size() >= 2 && !compare) return 2;
    // Only with results: on an empty page the bubble would cover the suggestion buttons.
    if (kind == L"search") return tab->snapshot && tab->snapshot->size() > 0 ? 3 : -1;
    if (s.appPrefs.sidebar_collapsed) return 4;
    if (selected == 1 && tab->snapshot && tab->selected_index >= 0 &&
        static_cast<size_t>(tab->selected_index) < tab->snapshot->size())
        return (*tab->snapshot)[tab->selected_index].is_dir ? 0 : 5;
    return -1;
}

void HideTeachTip(AppState& s) {
    s.teachTip = -1;
    s.teachCandidate = -1;
}
} // namespace

bool UpdateTeachTip(AppState& s) {
    const ULONGLONG now = GetTickCount64();
    if (s.teachTip >= 0) {
        const ULONGLONG shown = now - s.teachShownAt;
        const bool movedOn = shown >= kTeachMinShowMs && TeachTrigger(s) != s.teachTip;
        if (!s.appPrefs.show_hints || movedOn || shown >= kTeachMaxShowMs || !s.dropBadge.empty()) {
            HideTeachTip(s);
            return true;
        }
        return false;
    }
    if (!s.appPrefs.show_hints || s.teachShownThisSession || TeachBusy(s)) {
        s.teachCandidate = -1;
        return false;
    }
    const int tip = TeachTrigger(s);
    if (tip < 0 || tip >= kTeachCount || (s.appPrefs.tips_seen & (1u << tip))) {
        s.teachCandidate = -1;
        return false;
    }
    if (tip != s.teachCandidate) {
        s.teachCandidate = tip;
        s.teachCandidateSince = now;
        return false;
    }
    if (now - s.teachCandidateSince < kTeachHoldMs) return false;
    // Shown = seen: each tip appears once, even if the user just moves on.
    s.teachTip = tip;
    s.teachShownAt = now;
    s.teachShownThisSession = true;
    s.appPrefs.tips_seen |= 1u << tip;
    s.appPrefs.Save();
    return true;
}

void HandleTeachButton(AppState& s, int button) {
    const int tip = s.teachTip;
    HideTeachTip(s);
    if (button == 2) {
        s.appPrefs.show_hints = false;
        s.appPrefs.Save();
    } else if (button == 0 && tip == 2) {
        DispatchMenuCommand(s, app::CmdCompareToggle);
    }
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void SearchFilterInSubfolders(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path)) return;
    std::wstring text = tab->filter_text;
    if (s.filterEditing && s.hwndFilterEdit) {
        wchar_t buf[512]{};
        GetWindowTextW(s.hwndFilterEdit, buf, ARRAYSIZE(buf));
        text = buf;
    }
    if (text.empty()) return;
    app::AdvancedSearchSpec spec;
    spec.name = text;
    spec.name_is_query = true;
    spec.location = app::LocationScope::CurrentFolder;
    spec.current_folder = path::StripExtendedPathPrefix(tab->current_path);
    ClearPaneFilter(s);
    NavigateTo(s, app::MakeSearchPath(app::CompileSearchQuery(spec)));
}

StatusHintAction CurrentStatusHintAction(AppState& s) {
    StatusHintAction action = StatusHintAction::None;
    ContextStatusHint(s, action);
    return action;
}

// View-model wrapper: builds the base VM and layers ops-layer status on top.
// probe_details=false: hit-test / input paths must not kick off selection
// probes (or mutate detailsSelPath); paint owns those side effects.
ui::WindowViewModel BuildVm(AppState& s, bool probe_details) {
    SyncTaskbarWindowTitle(s);
    if (!s.pane) return {};
    TickContentPackInstall(s);
    TickAiPackInstall(s);
    // A finished pack download repaints the window once; drop the cached
    // failed thumbnails so the newly supported files are decoded again.
    if (s.settings.TakeMediaPackResult()) {
        s.renderer.EvictThumbnails();
        s.quickPreview.OnMediaPackInstalled();
        s.detailsPreview.OnMediaPackInstalled();
    }
    if (s.settings.TakeImagePackResult()) {
        s.renderer.EvictThumbnails();
        s.quickPreview.OnImagePackInstalled();
        s.detailsPreview.OnImagePackInstalled();
    }
    const bool raw_installed = s.settings.TakeRawPackResult();
    if (raw_installed) {
        s.renderer.EvictThumbnails();
        s.quickPreview.OnExtraPackInstalled();
        s.detailsPreview.OnExtraPackInstalled();
    }
    if (s.quickPreview.visible() || s.detailsPreview.visible()) {
        ui::MediaPackOffer offer, image_offer;
        s.settings.FillMediaPackOffer(offer);
        s.settings.FillImagePackOffer(image_offer);
        // Fixture for the Quick Look offers while no pack is published.
        wchar_t fake[8]{};
        if (s.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_PACK_OFFER", fake, ARRAYSIZE(fake))) {
            offer.installable = image_offer.installable = true;
            if (!offer.download_bytes) offer.download_bytes = uint64_t{28} << 20;
            if (!image_offer.download_bytes) image_offer.download_bytes = uint64_t{9} << 20;
            if (GetEnvironmentVariableW(L"PULSE_TEST_PACK_PROGRESS", fake, ARRAYSIZE(fake))) {
                offer.installing = image_offer.installing = true;
                offer.progress = image_offer.progress = static_cast<float>(_wtoi(fake)) / 100.0f;
            }
        }
        s.quickPreview.SetMediaPackOffer(offer);
        s.detailsPreview.SetMediaPackOffer(offer);
        s.quickPreview.SetImagePackOffer(image_offer);
        s.detailsPreview.SetImagePackOffer(image_offer);
        ui::CloudDownloadOffer cloud;
        if (fs::IsCloudPlaceholder(s.quickPreview.item().attrs)) {
            cloud.path = s.quickPreview.item().path;
            cloud.percent = app::CloudDownloadPercent(cloud.path);
            cloud.provider = app::CloudRootLabelFor(cloud.path);
        }
        s.quickPreview.SetCloudDownloadOffer(cloud);
        cloud = {};
        if (fs::IsCloudPlaceholder(s.detailsPreview.item().attrs)) {
            cloud.path = s.detailsPreview.item().path;
            cloud.percent = app::CloudDownloadPercent(cloud.path);
            cloud.provider = app::CloudRootLabelFor(cloud.path);
        }
        s.detailsPreview.SetCloudDownloadOffer(cloud);
    }
    UpdateFolderCompare(s);
    s.changes.visible_paths.clear();
    ForEachPane(s, [&](app::Pane& pane) {
        if (auto* tab = pane.ActiveTab()) {
            tab->SetShowHiddenFiles(s.appPrefs.show_hidden_files);
            tab->SetShowProtectedOsFiles(s.appPrefs.show_protected_os_files);
        }
    });
    SyncColumnStrips(s);
    SyncDetailsTrees(s);
    UpdateWallHint(s);
    ui::WindowViewModel vm = app::BuildWindowViewModel(*s.pane, s.sidebar,
        s.pane->focused, s.maximized, s.darkMode, &s.places, s.sidebarCollapsedMask,
        SidebarHiddenMaskWithBar(s), s.starredExpanded, &s.sidebarOrder,
        s.sidebarQuickAccessHiddenMask);
    FillWorkspaceSidebarState(s, vm);
    if (const app::Tab* tab = ActiveTab(s)) {
        FillWallHint(*tab, vm.pane);  // photo wall suggestion
        // A writable phone folder takes 新建文件夹 (CreateDeviceFolder), as its
        // background menu and Ctrl+Shift+N already do; the toolbar follows.
        if (IsDeviceTab(tab) && app::DeviceRegistry::Instance().FolderWritable(tab->current_path))
            vm.pane.can_create_folder = true;
    }
    app::FillWindowTabStrip(vm, s.window_tabs);
    if(s.window_tabs.active<vm.tabs.size()) {
        const auto* layout=s.window_tabs.Active();
        if(layout&&ZoomedPane(*layout)) vm.tabs[s.window_tabs.active].title += L" · "+l10n::Get(l10n::StringId::AiCtlExpand);
    }
    // Rail: devices found on this network that no window has shown yet.
    if (const int badge = NetworkRailBadge(s); badge > 0) {
        for (auto& group : vm.sidebar)
            if (group.id == static_cast<int>(app::SidebarSectionId::Networks)) group.badge_count = badge;
    }
    // Open settings shows as the selected settings button, not as a tab.
    if (s.window_tabs.active < vm.tabs.size() && IsSettingsTab(ActiveTab(s)))
        vm.tabs[s.window_tabs.active].hidden = true;
    if (WindowIdentityShown(s) && app::ValidWindowColor(s.identity.color)) {  // M7c
        vm.window_identity = true;
        vm.window_identity_name = WindowDisplayName(s);
        vm.window_identity_rgb = app::kWindowColors[s.identity.color];
    }
    vm.show_pinned_tab_names = s.appPrefs.show_pinned_tab_names;
    vm.settings_list_smart_date = s.appPrefs.list_smart_date;
    vm.settings_list_zebra_rows = s.appPrefs.list_zebra_rows;
    vm.settings_list_tree_sticky = s.appPrefs.list_tree_sticky;
    vm.settings_menu_acrylic = s.appPrefs.menu_acrylic;
    vm.settings_list_size_bar = s.appPrefs.list_size_bar;
    vm.settings_list_tag_names = s.appPrefs.list_tag_name_color;
    vm.settings_list_selection_outline = s.appPrefs.list_selection_outline;
    vm.settings_list_thumbnail_badges = s.appPrefs.list_thumbnail_badges;
    vm.settings_media_columns = s.appPrefs.media_columns;
    vm.settings_mtime = app::MtimeHintFromPrefs(s.appPrefs);
    vm.settings_mtime.tree = s.appPrefs.list_mtime_tree;   // the switch shows the pref itself
    vm.settings_sync_onedrive = s.appPrefs.sync_onedrive;
    vm.settings_sync_svn = s.appPrefs.sync_svn;
    vm.settings_single_click = s.appPrefs.single_click_open;
    vm.settings_name_highlight = s.appPrefs.name_highlight;
    vm.settings_hover_select = s.appPrefs.hover_select;
    vm.settings_item_checkboxes = s.appPrefs.item_checkboxes;
    vm.settings_vertical_tabs = s.appPrefs.vertical_tabs;
    vm.tab_layout = s.appPrefs.tab_layout;
    vm.tab_max_rows = s.appPrefs.tab_max_rows;
    vm.settings_lan_discovery = s.appPrefs.lan_discovery;
    vm.settings_network_recent = s.appPrefs.network_recent_enabled;
    vm.settings_hidden_shares = s.appPrefs.show_hidden_shares;
    vm.settings_lan_cache_mode = s.appPrefs.lan_cache_mode;
    vm.settings_show_hints = s.appPrefs.show_hints;
    vm.settings_tips_seen = s.appPrefs.tips_seen != 0;
    vm.settings_folder_sort = s.appPrefs.folder_sort_mode;
    vm.settings_startup_open = s.appPrefs.startup_open;
    vm.settings_notify_icon = s.appPrefs.notify_icon_mode;
    vm.settings_new_tab_open = s.appPrefs.new_tab_open;
    vm.settings_open_pulse_icon = s.appPrefs.open_pulse_icon;
    vm.settings_new_window_shows = s.appPrefs.new_window_shows;
    vm.settings_arrange_include_minimized = s.appPrefs.arrange_include_minimized;
    vm.settings_arrange_gap = s.appPrefs.arrange_gap;
    vm.settings_open_this_pc = s.appPrefs.open_this_pc;
    vm.settings_open_win_e = s.appPrefs.open_win_e;
    vm.settings_external_open = s.appPrefs.external_open;
    vm.settings_close_last_tab = s.appPrefs.close_window_with_last_tab;
    vm.settings_close_tab_double_click = s.appPrefs.close_tab_on_double_click;
    vm.settings_confirm_delete = s.appPrefs.confirm_recycle_delete;
    vm.settings_home_folder = s.appPrefs.home_folder;
    vm.settings_text_render = s.appPrefs.text_render;
    vm.settings_ui_font_scale = s.appPrefs.ui_font_scale;
    std::copy_n(s.settings.custom_accent(), 3, vm.settings_accent_hsl);
    vm.sidebar_scroll = s.sidebarScroll;
    vm.sidebar_scrollbar_opacity = s.sidebarScrollbarFade.Opacity();
    vm.sidebar_scrollbar_expand = s.sidebarScrollbarFade.Expand();
    vm.settings_scrollbar_opacity = s.settingsScrollbarFade.Opacity();
    vm.settings_scrollbar_expand = s.settingsScrollbarFade.Expand();
    if (s.groupDragActive) {
        vm.sidebar_group_drag_id = s.groupDragId;
        if (s.groupGapVisible) vm.sidebar_group_gap_line_y = s.groupGapLineY;
    }
    if (s.pinDragActive) {
        vm.sidebar_pin_drag_index = s.pinDragRun;
        if (s.pinGapVisible) vm.sidebar_pin_gap_line_y = s.pinGapLineY;
        POINT cursor{};
        if (s.hwnd && GetCursorPos(&cursor) && ScreenToClient(s.hwnd, &cursor))
            vm.sidebar_pin_drag_dy = static_cast<float>(cursor.y - s.pinDragStartPt.y);
    }
    if (app::Tab* sel_tab = ActiveTab(s); sel_tab && sel_tab->content_results && sel_tab->SelectedCount() > 1) {
        wchar_t count[64]{};
        swprintf_s(count, l10n::Get(l10n::StringId::SelectedCountFormat).c_str(), sel_tab->SelectedCount());
        const std::optional<uint64_t> bytes = ContentSelectionSize(*sel_tab);
        vm.status.selection_text = std::wstring(count) + L"  \u00B7  " +
            (bytes ? pulse::format::ByteSize(*bytes, true) : l10n::Get(l10n::StringId::LoadingEllipsis));
    }
    vm.status.can_undo = s.ops.CanUndo();
    vm.status.can_redo = s.ops.CanRedo();
    vm.status.history_open = s.undoHistoryOpen;
    vm.status.history_offset = s.undoHistoryOffset;
    vm.status.undo_label = l10n::Get(l10n::StringId::OpHistory);
    const auto undo_history = s.ops.UndoHistorySummary();
    for (const auto& item : undo_history) {
        const auto& e = item;
        ui::UndoHistoryRowView row;
        row.task_id = e.task_id; row.undone = item.undone; row.supported = e.supported;
        if (e.recorded_at != 0) {
            FILETIME utc{static_cast<DWORD>(e.recorded_at & 0xFFFFFFFFu), static_cast<DWORD>(e.recorded_at >> 32)};
            FILETIME local{};
            SYSTEMTIME clock{};
            if (FileTimeToLocalFileTime(&utc, &local) && FileTimeToSystemTime(&local, &clock)) {
                wchar_t stamp[16]{};
                swprintf_s(stamp, L"%02u:%02u", static_cast<unsigned>(clock.wHour), static_cast<unsigned>(clock.wMinute));
                row.time = stamp;
            }
        }
        auto verb = l10n::StringId::Operation;
        switch (e.type) {
        case ops::OpType::Copy: row.icon_glyph = L"\xE8C8"; verb = l10n::StringId::TrayIntentCopy; break;
        case ops::OpType::Move: row.icon_glyph = L"\xE7C2"; verb = l10n::StringId::TrayIntentMove; break;
        case ops::OpType::Rename: row.icon_glyph = L"\xE8AC"; verb = l10n::StringId::Rename; break;
        case ops::OpType::BatchRename: row.icon_glyph = L"\xE8AC"; verb = l10n::StringId::BatchRename; break;
        case ops::OpType::CreateFolder: row.icon_glyph = L"\xE8F4"; verb = l10n::StringId::NewFolder; break;
        case ops::OpType::CreateTextFile: verb = l10n::StringId::NewTextDocument; break;
        case ops::OpType::RestoreRecycle: row.icon_glyph = L"\xE7A7"; verb = l10n::StringId::Restore; break;
        case ops::OpType::RealDelete: row.icon_glyph = L"\xE74D"; verb = l10n::StringId::PermanentlyDelete; break;
        default: row.icon_glyph = L"\xE74D"; verb = l10n::StringId::Delete; break;
        }
        row.label = e.label.empty() ? l10n::Get(verb) + L" · " + std::to_wstring(e.source_count) : e.label;
        const auto& first_path = item.PreviewPath();
        const size_t path_count = item.PreviewCount();
        if (path_count != 0) {
            const auto leaf = [](const std::wstring& value) {
                std::wstring short_path = path::FriendlyPathText(value);
                while (short_path.size() > 3 && (short_path.back() == L'\\' || short_path.back() == L'/')) short_path.pop_back();
                const auto slash = short_path.find_last_of(L"\\/");
                return slash == std::wstring::npos || slash + 1 == short_path.size() ? short_path : short_path.substr(slash + 1);
            };
            const auto folder = fs::ParentPath(first_path);
            row.detail = leaf(folder);
            if (e.source_count != 0 && e.destination_count != 0 &&
                (e.type == ops::OpType::Copy || e.type == ops::OpType::Move)) {
                const auto source_folder = fs::ParentPath(e.first_source);
                const auto destination_folder = fs::ParentPath(e.first_destination);
                row.detail = leaf(source_folder) + L" → " + leaf(destination_folder);
            } else if (path_count == 1) row.detail += L" · " + leaf(first_path);
            if (auto* active = ActiveTab(s)) row.elsewhere = _wcsicmp(fs::ParentPath(first_path).c_str(), fs::NormalizePath(active->current_path).c_str()) != 0;
        }
        if (item.redo_cleared) row.detail += L" · " + l10n::Get(l10n::StringId::HistoryRedoCleared);
        if (!item.undone && e.supported && vm.status.undo_label == l10n::Get(l10n::StringId::OpHistory)) {
            vm.status.undo_label = l10n::Get(l10n::StringId::Undo) + L" " + l10n::Get(verb) +
                L" · " + std::to_wstring(e.source_count);
        }
        vm.status.history.push_back(std::move(row));
    }
    std::stable_partition(vm.status.history.begin(), vm.status.history.end(), [](const auto& row) { return row.undone; });
    vm.status.history_offset = std::clamp(vm.status.history_offset, 0, std::max(0, static_cast<int>(vm.status.history.size()) - 1));
    ops::OpStatus st = s.ops.Status();
    vm.status.task_id = st.task_id;
    if (st.active || !st.last_error.empty() || !st.summary.empty()) {
        vm.status.task_text = st.last_error.empty() ? st.summary
            : st.summary + L" — " + st.last_error;
        vm.status.task_progress = st.active ? st.percent : -1.0f;
        vm.status.task_active = st.active;
        vm.status.task_failed = !st.last_error.empty();
        vm.status.task_paused = st.active && (st.phase == ops::OpPhase::Paused || st.pause_requested);
        if (st.active) vm.status.task_queued = static_cast<uint32_t>(s.ops.QueuedTransferCount());
        if (vm.status.task_paused) {
            vm.status.task_text = l10n::Get(st.type == ops::OpType::Copy ? l10n::StringId::OpCopyPaused
                : st.type == ops::OpType::Move ? l10n::StringId::OpMovePaused
                : st.type == ops::OpType::RealDelete || st.type == ops::OpType::RecycleDelete
                    ? l10n::StringId::OpDeletePaused
                : st.type == ops::OpType::RestoreRecycle ? l10n::StringId::OpRestorePaused : l10n::StringId::OpPaused);
        }
        if (!st.active && st.phase == ops::OpPhase::Completed && st.last_error.empty()) {
            // The finished pill offers 撤销 / 打开.
            vm.status.task_can_undo = s.ops.CanUndoTask(st.task_id);
            vm.status.task_can_open = (st.type == ops::OpType::Copy || st.type == ops::OpType::Move) &&
                                      !st.destination_dir.empty();
        }
        if (!st.active && st.phase == ops::OpPhase::Failed && st.failed_items > 0 && !st.failures.empty()) {
            // Partial failure: what did not go through, and where to look.
            vm.status.task_failed_items = st.failed_items;
            const std::wstring count = pulse::format::GroupedInt(st.failed_items);
            wchar_t pill[160]{};
            swprintf_s(pill, l10n::Get(l10n::StringId::OpPillFailed).c_str(), count.c_str());
            vm.status.task_text = pill;
        }
        if(s.contentSelectionAction) vm.status.selection_text=l10n::Get(l10n::StringId::OpPreparingList);
    }
    app::ApplyUpdateStatus(vm.status, UpdateProgressForView(s), st.active);
    {
        std::wstring idx = l10n::ServiceText(s.index.Status());
        app::Tab* active = ActiveTab(s);
        std::wstring virtual_kind;
        std::wstring virtual_rest;
        const bool search_visible = active &&
            app::ParsePulsePath(active->current_path, &virtual_kind, &virtual_rest) &&
            virtual_kind == L"search";
        if (!idx.empty() && !vm.status.query_active && (s.paletteSearching || search_visible)) {
            if (!vm.status.status_text.empty()) vm.status.status_text += L"  ·  ";
            vm.status.status_text += idx;
        }
    }
    if (s.showFps) {
        wchar_t perf[256];
        swprintf_s(perf, l10n::Get(l10n::StringId::PerformanceFormat).c_str(),
            s.lastFrameMs, s.lastFps, s.timing.enum_ms, s.timing.sort_ms,
            s.processCpuPercent, s.workingSetMb);
        wchar_t breakdown[96];
        swprintf_s(breakdown, L"  \u00B7  D %.1f ms  \u00B7  P %.1f ms",
            s.timing.draw_ms, s.timing.present_ms);
        vm.status.performance_text = std::wstring(perf) + breakdown;
        wchar_t lumaStatsEnabled[8]{};
        const bool showLumaStats = GetEnvironmentVariableW(
            L"PULSE_LUMATEXT_STATS", lumaStatsEnabled, ARRAYSIZE(lumaStatsEnabled)) > 0;
        wchar_t compactPerf[160];
        const auto* lumaStats = showLumaStats ? s.compositor.GetLumaTextStats() : nullptr;
        if (lumaStats && s.compositor.LumaTextEnabled()) {
            swprintf_s(compactPerf,
                L"DEV %.1f ms \u00B7 D %.1f \u00B7 P %.1f \u00B7 %.0f FPS \u00B7 LT %llu/%llu E%llu \u00B7 %.0f MB",
                s.lastFrameMs, s.timing.draw_ms, s.timing.present_ms, s.lastFps,
                static_cast<unsigned long long>(lumaStats->surface_cache_hits),
                static_cast<unsigned long long>(lumaStats->surface_cache_misses),
                static_cast<unsigned long long>(lumaStats->surface_cache_evictions),
                s.workingSetMb);
        } else {
            swprintf_s(compactPerf, L"DEV  %.1f ms  \u00B7  D %.1f  \u00B7  P %.1f  \u00B7  %.0f FPS  \u00B7  %.0f MB",
                s.lastFrameMs, s.timing.draw_ms, s.timing.present_ms,
                s.lastFps, s.workingSetMb);
        }
        vm.status.performance_compact_text = compactPerf;
    } else if (!s.appPrefs.status_hints_learned && s.appPrefs.show_hints &&
               undo_history.empty() && !st.active && st.task_id == 0 &&
               vm.status.task_text.empty()) {
        const auto region = static_cast<ui::HitTestResult::Region>(s.hoverRegion);
        if (HoverHintIsCommand(region) && !s.addressSearching && !s.filterEditing) {
            // Tooltips may carry extra lines; the status bar only has room for the first.
            vm.status.hint_text = TooltipForHover(s);
            const size_t nl = vm.status.hint_text.find(L'\n');
            if (nl != std::wstring::npos) vm.status.hint_text.resize(nl);
        }
        if (vm.status.hint_text.empty()) {
            StatusHintAction action = StatusHintAction::None;
            vm.status.hint_text = ContextStatusHint(s, action);
            // The search-box keys are not keymap actions (Alt+Enter there is not 属性).
            if (!s.addressSearching) vm.status.hint_text = app::RemapShortcutText(vm.status.hint_text);
            if (action != StatusHintAction::None) {
                vm.status.hint_action = static_cast<int>(action);
                vm.status.hint_action_text = StatusHintActionText(action) + L" \u2192";
            }
        }
    }
    if (!s.status_feedback.empty()) {
        vm.status.hint_text = s.status_feedback;
        vm.status.hint_action = 0;
        vm.status.hint_action_text.clear();
    }
    pulse::FillAiStatusVm(vm);   // the AI chip at the left end of the status bar
    // 1B-2 overlays: cut rows, drag feedback, breadcrumb hover.
    if (!s.cutPaths.empty()) {
        app::Tab* tab = ActiveTab(s);
        if (tab && !tab->current_path.empty()) {
            for (const auto& cutPath : s.cutPaths) {
                if (fs::ParentPath(cutPath) != tab->current_path) continue;
                const size_t slash = cutPath.find_last_of(L"\\/");
                vm.pane.cut_names.insert(slash == std::wstring::npos
                    ? cutPath : cutPath.substr(slash + 1));
            }
        }
    }
    if (!s.leavingPaths.empty()) {
        if (app::Tab* tab = ActiveTab(s)) FillLeavingRows(s, tab->current_path, vm.pane);
    }
    if (app::Tab* tab = ActiveTab(s)) vm.pane.ai_list = AiListOverlayFor(tab->current_path);   // Pulse 助手
    if (!s.dragLanding.names.empty()) {
        if (app::Tab* tab = ActiveTab(s)) FillLandingRows(s, s.pane, tab->current_path, vm.pane);
    }
    if (s.dragFilesOver && !vm.pane.path.empty() && !fs::IsVirtualPath(vm.pane.path)) {
        vm.pane.drag_parent_path = fs::ParentPath(vm.pane.path);
        if (vm.pane.drag_parent_path == vm.pane.path) vm.pane.drag_parent_path.clear();
    }
    vm.pane.drop_parent = s.dropParent;
    if (s.dropParent && s.springStart)
        vm.pane.drop_parent_progress = (std::min)(1.0f, float(GetTickCount64() - s.springStart) / 800.0f);
    vm.pane.drop_target_index = s.dropRow;
    vm.pane.drop_spring_progress = DropSpringProgress(s);
    vm.pane.rename_index = s.renameIndex;
    vm.pane.rename_text = RenameEditText(s);
    FillRenameHint(s, vm.pane);
    SyncSearchBarWidth(s);
    vm.address_editing = s.addressEditing;
    vm.address_searching = s.addressSearching;
    vm.address_suggestions = s.addressSuggest;
    vm.address_suggestion_header = s.addressSuggestHeader;
    vm.address_suggestion_selected = s.addressSuggestSel;
    FillFilterTagSuggestionsVm(s, vm);
    vm.address_search_current = s.addressSearchCurrent;
    vm.address_search_has_text = s.addressSearching && s.hwndAddressEdit &&
                                GetWindowTextLengthW(s.hwndAddressEdit) > 0;
    vm.address_search_animation = s.addressSearchAnimation;
    vm.address_scope_animation = s.addressScopeAnimation;
    FillAddressSearchView(s, vm);
    vm.splitter_pressed = s.splitterDragging;
    vm.details_resize_pressed = s.detailsPanelResizing;
    if (s.listSelection.BandActive()) {
        vm.pane.marquee_active = true;
        vm.pane.marquee_rect = MarqueeDisplayRect(s);
    }
    vm.breadcrumb_hover = s.breadcrumbHover;
    vm.breadcrumb_drop = s.dropBreadcrumb;
    vm.drag_folders_active = s.dragFilesOver;
    vm.sidebar_drop_index = s.dropSidebar;
    vm.tab_drop_index = s.tabDropIndex;
    vm.tab_file_drop = s.springTab;
    if (s.springTab >= 0)
        vm.tab_file_drop_progress =
            static_cast<float>(GetTickCount64() - s.springTabStart) / static_cast<float>(app::kTabSpringMs);
    vm.content_drop = s.contentDrop;
    vm.content_drop_as_tab = s.contentDropAsTab;
    vm.content_drop_rects = s.contentDropRects;
    vm.drag_ghost = s.dragGhost;
    // Sidebar tags: mark the row being renamed.
    int tagsGroup = -1;
    for (int g = 0; g < static_cast<int>(vm.sidebar.size()); ++g) {
        if (vm.sidebar[g].id == static_cast<int>(app::SidebarSectionId::Tags)) { tagsGroup = g; break; }
    }
    if (tagsGroup >= 0) {
        auto& items = vm.sidebar[tagsGroup].items;
        if (!s.tagRenameId.empty()) {
            const std::wstring editing_path = app::MakeTagPath(s.tagRenameId);
            for (auto& item : items) item.editing = item.path == editing_path;
        }
    }
    // Title-bar tabs: emit the tentative drag order and slide offsets.
    if (s.pane && !vm.tabs.empty()) {
        const bool dragging = s.tabDragging && s.tabOrder.size() == vm.tabs.size();
        if (dragging) {
            std::vector<ui::TabView> reordered;
            reordered.reserve(vm.tabs.size());
            for (int idx : s.tabOrder) {
                if (idx >= 0 && idx < static_cast<int>(vm.tabs.size()))
                    reordered.push_back(std::move(vm.tabs[static_cast<size_t>(idx)]));
            }
            if (reordered.size() == vm.tabs.size()) vm.tabs = std::move(reordered);
            for (int pos = 0; pos < static_cast<int>(s.tabOrder.size()); ++pos) {
                if (s.tabOrder[pos] == s.tabDragIndex) { vm.tab_drag_index = pos; break; }
            }
            vm.tab_drag_x = s.tabDragFloatLeft;
            POINT cursor{};
            if (s.window_tabs.items.size() >= 2 && !s.tabDragFromChip && s.hwnd &&
                GetCursorPos(&cursor) && ScreenToClient(s.hwnd, &cursor)) {
                const float dy = static_cast<float>(cursor.y - s.tabDragStartPt.y);
                const float limit = 24.0f * s.scale;  // kTabDetachDistanceDip
                vm.tab_detach_dy = std::clamp(dy, -limit, limit);
                vm.tab_detach_pull = std::clamp(std::fabs(dy) / limit, 0.0f, 1.0f);
            }
            vm.tab_drag_count = s.tabDragRunLen;
            vm.tab_drag_chip = s.tabDragFromChip;
            if (s.tabDragRunLen > 1) vm.tab_drag_index = s.tabDragRunPos;
            const int origActive = static_cast<int>(s.window_tabs.active);
            for (int pos = 0; pos < static_cast<int>(s.tabOrder.size()); ++pos) {
                const bool isActive = s.tabOrder[pos] == origActive;
                vm.tabs[static_cast<size_t>(pos)].active = isActive;
                if (isActive) vm.active_tab = pos;
            }
        }
        for (int pos = 0; pos < static_cast<int>(vm.tabs.size()); ++pos) {
            const int orig = dragging ? s.tabOrder[pos] : pos;
            if (orig < 0 || orig >= static_cast<int>(s.window_tabs.items.size())) continue;
            const app::LayoutTab* key = s.window_tabs.items[static_cast<size_t>(orig)].get();
            const auto off = s.tabOffsets.find(key);
            if (off != s.tabOffsets.end())
                vm.tabs[static_cast<size_t>(pos)].x_offset = off->second;
            vm.tabs[static_cast<size_t>(pos)].flash = TabFlashAmount(s, key);
        }
        for (size_t gi = 0; gi < vm.tab_groups.size(); ++gi) {
            const auto off = s.chipOffsets.find(vm.tab_groups[gi].id);
            if (off != s.chipOffsets.end()) vm.tab_groups[gi].x_offset = off->second;
        }
    }
    // Vertical tabs join the sidebar before its scroll range is measured.
    ApplyVerticalTabs(s, vm);
    FillFavoritesBar(s, vm);
    // Clamp only after the sidebar is populated, using the same model as draw/hit-test.
    s.sidebarScroll = std::clamp(s.sidebarScroll, 0.0f, s.renderer.SidebarMaxScroll(
        vm, static_cast<float>(s.compositor.Width()),
        static_cast<float>(s.compositor.Height())));
    vm.sidebar_scroll = s.sidebarScroll;
    // Right details panel: selection info + size walk + tag chips.
    vm.details_visible = s.showDetailsPanel;
    vm.layout_preset = static_cast<int>(LayoutOf(s));
    std::wstring sizeTarget; // folder that should be walking ("" = none)
    if (s.showDetailsPanel) {
        ui::DetailsPanelView& dv = vm.details;
        dv.scroll_y = s.detailsScroll;
        dv.preview_only = s.detailsPreviewOnly;
        dv.preview_enabled = s.detailsPreviewEnabled;
        dv.preview_expansion = s.detailsPreviewExpansion;
        dv.collapsed_mask = s.detailsCollapsedMask;
        app::Tab* tab = ActiveTab(s);
        const int selCount = tab ? tab->SelectedCount() : 0;
        if (tab && selCount >= 1) {
            dv.has_selection = true;
            dv.multi_count = selCount;
        }
        if (dv.has_selection && selCount == 1 && tab->snapshot &&
            tab->selected_index >= 0 &&
            tab->selected_index < static_cast<int>(tab->EntryCount())) {
            const fs::DirEntry& e = tab->EntryAt(static_cast<size_t>(tab->selected_index));
            const bool penetrated = !e.link_target.empty();
            dv.name = e.name;
            dv.is_dir = penetrated ? e.link_target_is_dir : e.is_dir;
            dv.attrs = e.attrs;
            dv.link_kind = fs::ClassifyLink(e.attrs, e.reparse_tag);
            dv.is_link = dv.link_kind != fs::LinkKind::None || (!e.is_dir && ui::IsShortcutName(e.name));
            if (penetrated && e.link_target_is_dir) dv.attrs |= FILE_ATTRIBUTE_DIRECTORY;
            dv.size_value = penetrated ? e.link_target_size : e.size;
            const FILETIME& shown_mtime = penetrated ? e.link_target_mtime : e.mtime;
            dv.modified_value = (static_cast<uint64_t>(shown_mtime.dwHighDateTime) << 32) |
                                shown_mtime.dwLowDateTime;
            dv.view_generation = tab->view_generation;
            dv.path = penetrated ? e.link_target
                                 : EntryFullPath(*tab, tab->selected_index);
            // 下载并预览 (C4b): only the entry itself; a shortcut's target is
            // not what the well would download.
            dv.cloud_online = !penetrated && !dv.is_dir && fs::IsCloudPlaceholder(e.attrs);
            if (dv.cloud_online) {
                dv.cloud_download_percent = app::CloudDownloadPercent(dv.path);
                dv.cloud_provider = app::CloudRootLabelFor(dv.path);
            }
            // Directory refreshes replace the snapshot while the selected
            // path stays the same. Keep the details panel's cached metadata
            // aligned with the refreshed entry instead of showing its old
            // modification time indefinitely. For shortcuts, compare the
            // resolved target path shown in the details panel.
            if (s.detailsSelPath == dv.path &&
                (shown_mtime.dwHighDateTime != 0 || shown_mtime.dwLowDateTime != 0)) {
                s.detailsModified = shown_mtime;
            }
        }
        if (dv.has_selection && selCount > 1 && tab->snapshot) {
            uint64_t knownSize = 0;
            int files = 0, folders = 0;
            std::optional<uint64_t> contentSize;
            if(tab->content_results) {
                files=selCount;contentSize=ContentSelectionSize(*tab);knownSize=contentSize.value_or(0);
            } else {
                tab->SelectionSizeSummary(&knownSize, &files, &folders);
            }
            wchar_t composition[96]{};
            if (files && folders)
                swprintf_s(composition,
                    l10n::Get(l10n::StringId::FilesFoldersFormat).c_str(), files, folders);
            else if (files)
                swprintf_s(composition,
                    l10n::Get(l10n::StringId::FilesOnlyFormat).c_str(), files);
            else
                swprintf_s(composition,
                    l10n::Get(l10n::StringId::FoldersOnlyFormat).c_str(), folders);
            dv.type_text = composition;
            dv.size_text = tab->content_results && !contentSize ? l10n::Get(l10n::StringId::LoadingEllipsis) : pulse::format::ByteSize(knownSize, true);
            dv.location_text = fs::IsVirtualPath(tab->current_path)
                ? l10n::Get(l10n::StringId::MultipleLocations)
                : pulse::path::FriendlyPathText(tab->current_path);
        }
        if (dv.has_selection && dv.multi_count == 1 && !dv.path.empty()) {
            if (probe_details && s.detailsSelPath != dv.path) {
                s.detailsSelPath = dv.path;
                s.detailsScroll = 0.0f;
                dv.scroll_y = 0.0f;
                // Clear stale facts immediately; GetFileAttributesEx /
                // SHGetFileInfo / security APIs run off-thread (WM_DETAILS_META).
                s.detailsSelValid = false;
                s.detailsTypeName.clear();
                s.detailsMetaPath.clear();
                s.detailsOwner.clear();
                s.detailsPermissions.clear();
                s.detailsDrive.clear();
                s.detailsFileSystem.clear();
                s.detailsFreeSpace.clear();
                if (!fs::IsVirtualPath(dv.path))
                    PrefetchDetailsMeta(s.hwnd, dv.path);
                // 时长 and 分级 are read in the background; the panel fills in when they arrive.
                if (!dv.is_dir && !fs::IsVirtualPath(dv.path) && app::HasMediaProperties(dv.name)) {
                    app::MediaSortRequest request;
                    request.path = dv.path;
                    request.modified = dv.modified_value;
                    request.size = dv.size_value;
                    s.mediaSort.SetChangeNotify(ServiceWindow(s), WM_UI_KICK);
                    s.mediaSort.Request({request});
                }
            }
            s.detailsStarred = s.places.IsStarred(dv.path);
            dv.starred = s.detailsStarred;
            if (s.detailsSelValid) {
                dv.created_text = pulse::format::LocalFileTime(s.detailsCreated);
                dv.modified_text = pulse::format::LocalFileTime(s.detailsModified);
                dv.accessed_text = pulse::format::LocalFileTime(s.detailsAccessed);
            }
            dv.location_text = pulse::path::FriendlyPathText(fs::ParentPath(dv.path));
            dv.attributes_text = DetailsAttributeText(dv.attrs);
            dv.type_text = dv.link_kind != fs::LinkKind::None
                ? ui::LinkTypeText(dv.link_kind) : s.detailsTypeName;
            if (!dv.is_dir) {
                dv.subtitle_text = dv.type_text;
                if (!dv.subtitle_text.empty()) dv.subtitle_text += L" · ";
                dv.subtitle_text += pulse::format::ByteSize(dv.size_value, true);
                dv.size_text = pulse::format::ByteSize(dv.size_value, true) + L" (" +
                               pulse::format::GroupedInt(dv.size_value) +
                               pulse::l10n::Get(pulse::l10n::StringId::Bytes).c_str();
            }
            if (!dv.is_dir && !dv.cloud_online && !fs::IsVirtualPath(dv.path) && app::HasMediaProperties(dv.name)) {
                dv.can_rate = app::CanRateName(dv.name);
                app::MediaSortValue media;
                if (s.mediaSort.Lookup(dv.path, media)) {
                    dv.rating = media.rating;
                    dv.duration_text = pulse::format::PlayingTime(media.duration_ms);
                }
            }
            if (s.detailsMetaPath == dv.path) {
                dv.owner_text = s.detailsOwner;
                dv.permissions_text = s.detailsPermissions;
                dv.drive_text = s.detailsDrive;
                dv.fs_text = s.detailsFileSystem;
                dv.free_space_text = s.detailsFreeSpace;
            }
            s.renderer.CachedPreviewProperties(dv.path, dv.modified_value, dv.size_value,
                                               dv.preview_properties);
            dv.preset_tags.clear();
            for (int idx = 0; idx < static_cast<int>(s.places.tags.size()); ++idx) {
                ui::DetailsPanelView::TagChip chip;
                chip.name = s.places.tags[static_cast<size_t>(idx)].name;
                chip.color = ui::HexColor(s.places.tags[static_cast<size_t>(idx)].rgb);
                chip.tag_index = idx;
                chip.assigned = s.places.PathHasTag(dv.path, idx);
                dv.preset_tags.push_back(std::move(chip));
            }
            if (dv.is_dir) sizeTarget = dv.path;
        }
    }
    std::wstring requestedSizePath;
    {
        std::lock_guard<std::mutex> lock(s.detailsSizeMutex);
        requestedSizePath = s.detailsSizeRequested;
    }
    if (!sizeTarget.empty()) {
        if (requestedSizePath != sizeTarget) StartDetailsSizeWalk(s, sizeTarget);
        std::lock_guard<std::mutex> lock(s.detailsSizeMutex);
        if (s.detailsSize.done && s.detailsSize.path == sizeTarget) {
            vm.details.size_text = pulse::format::ByteSize(s.detailsSize.size, true);
            wchar_t contains[96];
            swprintf_s(contains, l10n::Get(l10n::StringId::ContainsFormat).c_str(),
                       s.detailsSize.files, s.detailsSize.dirs);
            vm.details.contains_text = contains;
            if (s.detailsSize.partial) {
                const auto incomplete = l10n::Get(l10n::StringId::Incomplete);
                vm.details.size_text += incomplete;
                vm.details.contains_text += incomplete;
            }
        } else {
            vm.details.size_pending = true;
        }
    } else if (!requestedSizePath.empty()) {
        StopDetailsSizeWalk(s);
    }
    vm.drag_badge = s.dropBadge;
    vm.drag_badge_x = s.dropBadgeX;
    vm.drag_badge_y = s.dropBadgeY;
    vm.drag_badge_move = s.dropBadgeMove && !s.dropBadge.empty();
    vm.hover_region = s.hoverRegion;
    vm.hover_control_index = s.hoverControlIndex;
    vm.hover_sub_index = s.hoverSubIndex;
    if (s.shot.active && s.isolatedTest) {
        // Hidden checks of the 右键菜单 preview: PULSE_SHOT_MENU_HOVER=<item>
        // (or f1 / f2 for the fixed rows) hovers a row; PULSE_SHOT_MENU_DRAG=
        // <item>,<px> holds it with the pointer at that client coordinate.
        wchar_t v[32]{};
        if (GetEnvironmentVariableW(L"PULSE_SHOT_MENU_HOVER", v, ARRAYSIZE(v)) > 0) {
            vm.hover_region = static_cast<int>(ui::HitTestResult::SettingsToggle);
            vm.hover_control_index = v[0] == L'f' ? ui::kSettingsMenuFixedHit + _wtoi(v + 1)
                                                  : ui::kSettingsMenuRowHit + _wtoi(v);
        }
        if (GetEnvironmentVariableW(L"PULSE_SHOT_MENU_DRAG", v, ARRAYSIZE(v)) > 0) {
            wchar_t* end = nullptr;
            vm.settings_menu_drag = static_cast<int>(wcstol(v, &end, 10));
            if (end && *end == L',') vm.settings_menu_drag_pos = static_cast<float>(wcstod(end + 1, nullptr));
        }
    }
    vm.hover_pane_index = s.hoverPaneIndex;
    vm.column_resize_pressed = s.columnResizing;
    if (s.columnHeaderDrag.active) {
        vm.column_drag_pane = s.columnHeaderDrag.pane;
        vm.column_drag_from = s.columnHeaderDrag.from;
        vm.column_drag_slot = s.columnHeaderDrag.slot;
        vm.column_drag_line_x = s.columnHeaderDrag.line_x;
        vm.column_drag_cursor_x = s.columnHeaderDrag.cursor_x;
        vm.column_drag_grab_dx = s.columnHeaderDrag.grab_dx;
    }
    // No hover tips while a marquee owns the pointer.
    vm.tooltip_text = s.listSelection.BandActive() ? std::wstring{} : s.tooltipText;
    if (s.teachTip >= 0) {
        using I = l10n::StringId;
        // Tip 1 (the staging tray) is retired: TeachTrigger never returns it.
        static constexpr I kTitles[] = {I::TeachMiddleTitle, I::TeachMiddleTitle, I::TeachCompareTitle,
                                        I::TeachSearchTitle, I::TeachSidebarTitle, I::TeachPreviewTitle};
        static constexpr I kBodies[] = {I::TeachMiddleBody, I::TeachMiddleBody, I::TeachCompareBody,
                                        I::TeachSearchBody, I::TeachSidebarBody, I::TeachPreviewBody};
        vm.teach.visible = true;
        vm.teach.title = l10n::Get(kTitles[s.teachTip]);
        vm.teach.body = l10n::Get(kBodies[s.teachTip]);
        vm.teach.primary = s.teachTip == 2 ? l10n::Get(I::TeachTry) + L" \u2192" : l10n::Get(I::TeachGotIt);
        vm.teach.never = l10n::Get(I::TeachNever);
    }
    vm.tooltip_x = static_cast<float>(s.hoverPoint.x);
    vm.tooltip_y = static_cast<float>(s.hoverPoint.y);
    pulse::FillAiAssistantVm(s, vm);   // Pulse 助手 and its chip (after FillAiStatusVm, may set a tip)
    // Tab wrapping changes the toolbar origin; settle it before pane hit geometry.
    RECT tab_client{};
    if (GetClientRect(s.hwnd, &tab_client)) s.renderer.UpdateTabLayout(vm, static_cast<float>(tab_client.right));
    FillPaneSlots(s, vm);
    {
        // File-list scrollbars: the hovered pane's (or the dragged, focused one's) expands.
        const bool dragging = s.scrollbarDragging && !s.scrollbarSidebar && !s.scrollbarSettings;
        const float expand = s.paneScrollbarFade.Expand(), opacity = s.paneScrollbarFade.Opacity();
        vm.pane.scrollbar_opacity = opacity;
        vm.pane.scrollbar_expand = vm.pane_slots.empty() ? expand : 0.0f;
        for (size_t i = 0; i < vm.pane_slots.size(); ++i) {
            ui::PaneSlotView& slot = vm.pane_slots[i];
            const bool hot = dragging ? slot.focused
                : (s.paneScrollbarHot && s.paneScrollbarHotPane == static_cast<int>(i));
            slot.pane.scrollbar_opacity = opacity;
            slot.pane.scrollbar_expand = hot ? expand : 0.0f;
            if (slot.focused) vm.pane.scrollbar_expand = slot.pane.scrollbar_expand;
        }
    }
    FillChangePopover(s, vm);
    FillFolderSizes(s, vm);
    vm.window_effect = ui::WindowEffectFromId(s.appPrefs.window_effect);
    vm.background_image = s.appPrefs.background_image;
    vm.wallpaper_look = s.appPrefs.wallpaper_look;
    vm.wallpaper_blur = s.appPrefs.wallpaper_blur;
    vm.frame_transparency = s.appPrefs.frame_transparency;
    // Also set here (not only when painting) so hit testing sees the
    // same enabled 侧边栏与标题栏 slider as the frame that drew it.
    vm.backdrop_enabled = !(s.shot_high_contrast || ui::IsHighContrast()) &&
                          s.compositor.UsesTransparentComposition() && s.backdropActive;
    vm.safe_mode = s.safeMode;
    {
        // Toolbar "Group" button: views that draw group headers, in groupable
        // places only (same rule as the Sort menu's group section).
        int toolbar_group = -1;
        if (const app::Tab* gt = ActiveTab(s)) {
            std::wstring kind;
            app::ParsePulsePath(gt->current_path, &kind, nullptr);
            const bool can_group = !gt->content_results &&
                ((!gt->current_path.empty() && !fs::IsVirtualPath(gt->current_path)) || kind == L"recent" ||
                 kind == L"search" || kind == L"saved-search" || kind == L"tag" || kind == L"tags" ||
                 kind == L"recycle");
            const bool groups_shown = ui::ShowsGroups(gt->view_mode) &&
                                      !(gt->wall && ui::IsZoomIconMode(gt->view_mode));
            if (can_group && groups_shown && !vm.pane.compare_active) toolbar_group = gt->group_by;
        }
        s.renderer.SetToolbarGroup(toolbar_group);
        // A hidden filter or group control still shows while it is in effect.
        s.renderer.SetToolbarView(s.appPrefs.toolbar_show_sort,
                                  s.appPrefs.toolbar_show_filter || !vm.pane.filter_text.empty(),
                                  s.appPrefs.toolbar_show_group || toolbar_group > 0,
                                  app::ToolbarViewLayoutOrder(s.appPrefs.toolbar_view_order));
    }
    // Toolbar follows the pointer: a file drag over the window swaps the
    // selection chip and the pinned commands for the pinned apps.
    const bool drop_mode = s.toolbarDropMode;
    if (vm.pane.selected_count > 0 && !drop_mode) vm.selection_actions = SelectionChipActions(s);
    s.renderer.SetToolbarSelection(drop_mode ? 0 : vm.pane.selected_count,
                                   static_cast<int>(vm.selection_actions.size()));
    if (!drop_mode) vm.toolbar_pins = ToolbarPinActions(s);
    vm.toolbar_apps = ToolbarAppPins(s);
    vm.toolbar_drop_mode = drop_mode;
    if (drop_mode) {
        vm.toolbar_drop_app = s.dropToolbarApp;
        vm.toolbar_drop_force = s.dropToolbarForce;
        vm.toolbar_drop_pin = s.dropToolbarPin;
        if (!s.toolbarDropProgram.empty()) {
            vm.toolbar_drop_label = l10n::Get(l10n::StringId::ToolbarPinApp);
            const size_t at = vm.toolbar_drop_label.find(L"{app}");
            const size_t slash = s.toolbarDropProgram.find_last_of(L"\\/");
            if (at != std::wstring::npos)
                vm.toolbar_drop_label.replace(at, 5, fs::StripLnkSuffix(
                    slash == std::wstring::npos ? s.toolbarDropProgram : s.toolbarDropProgram.substr(slash + 1)));
        } else if (!vm.toolbar_apps.empty()) {
            vm.toolbar_drop_label = l10n::Get(l10n::StringId::ToolbarDropToOpen);
        }
    }
    s.renderer.SetToolbarPrefs(s.appPrefs.toolbar_new_primary, s.appPrefs.toolbar_last_new == 1,
                               static_cast<int>(vm.toolbar_pins.size() + vm.toolbar_apps.size()));
    s.renderer.SetToolbarApps(static_cast<int>(vm.toolbar_pins.size()), drop_mode, vm.toolbar_drop_label);
    uint64_t separators = app::ToolbarSeparatorMask(s.appPrefs.toolbar_tools);
    // The apps' own separators (bit: before the app after the marked one).
    const size_t app_from = vm.toolbar_pins.size();
    for (size_t k = 0; k + 1 < vm.toolbar_apps.size() && k < s.appPrefs.toolbar_apps.size(); ++k)
        if (app_from + k + 1 < 64 &&
            app::ToolbarAppBreakAfter(s.appPrefs.toolbar_app_breaks, s.appPrefs.toolbar_apps[k]))
            separators |= uint64_t{1} << (app_from + k + 1);
    s.renderer.SetToolbarSeparators(drop_mode ? 0 : separators);
    s.renderer.SetToolbarRightFrom(drop_mode ? -1 : app::ToolbarRightFrom(s.appPrefs.toolbar_tools));
    s.renderer.SetToolbarViewSlots(drop_mode ? std::array<int, 3>{-1, -1, -1} : app::ToolbarViewSlots(s.appPrefs.toolbar_tools));
    FillSettingsSearchVm(s, vm);
    if (s.undoHistoryOpen && s.hoverRegion == ui::HitTestResult::UndoHistoryRow && s.hoverControlIndex >= 0 &&
        static_cast<size_t>(s.hoverControlIndex) < vm.status.history.size()) {
        const uint64_t id = vm.status.history[s.hoverControlIndex].task_id;
        const auto paths = s.ops.UndoHistoryPreviewPaths(id);
        vm.pane.history_preview_paths = paths;
        for (auto& slot : vm.pane_slots) slot.pane.history_preview_paths = paths;
    }
    extensions::native::SyncSettingsPagesHost(s, vm);
    return vm;
}

// True while the sidebar is collapsed to its icon rail: rows carry no text, so
// every hover needs a name hint. EffectiveSidebarWidth returns pixels (the
// upstream DIP-scaling fix), which is what the rail test expects.
bool SidebarRailActive(const AppState& s) {
    // SidebarRect, not EffectiveSidebarWidth: the hover-peek overlay has text.
    const D2D1_RECT_F sb = s.renderer.SidebarRect(static_cast<float>(s.compositor.Width()),
                                                  static_cast<float>(s.compositor.Height()));
    return ui::SidebarRailLayout(sb.right - sb.left, s.scale);
}

void ApplyHoverTarget(AppState& s, const ui::HitTestResult& hit) {
    s.hoverRegion = static_cast<int>(hit.region);
    s.hoverControlIndex = hit.index;
    s.hoverSubIndex = hit.sub_index;
    s.hoverPath = hit.path;
    s.hoverLabel = hit.label;
    s.hoverSince = GetTickCount64();
    s.tooltipText.clear();
}

std::wstring TooltipForHover(AppState& s) {
    using R = ui::HitTestResult;
    using I = l10n::StringId;
    // Tooltips name keys; show the ones the user set (keymap.h).
    auto text = [](I id) { return app::RemapShortcutText(l10n::Get(id)); };
    switch (static_cast<R::Region>(s.hoverRegion)) {
    case R::RowFolderSize: {
        auto* pane = PaneAtSlot(s, s.hoverPaneIndex);
        auto* tab = pane ? pane->ActiveTab() : ActiveTab(s);
        if (!tab || s.hoverControlIndex < 0 || s.hoverControlIndex >= static_cast<int>(tab->EntryCount())) return L"";
        return DescribeFolderSize(s, EntryFullPath(*tab, s.hoverControlIndex));
    }
    case R::AddressSearch: return text(s.appPrefs.show_hints ? I::TipxSearch : I::Search);
    case R::AddressSearchScope: {
        const auto* tab = ActiveTab(s);
        const bool current = !s.addressSearching && IsAddressSearchResults(tab)
            ? tab->search_input_current : s.addressSearchCurrent;
        return text(current ? I::LocationCurrent : I::LocationIndexed);
    }
    case R::AddressSearchMode: {
        // Say both the current state and what a click will do.
        const auto* tab = ActiveTab(s);
        const bool content = !s.addressEditing && IsAddressSearchResults(tab)
            ? tab->search_input_content : s.addressSearchContent;
        return text(content ? I::TipSearchModeContent : I::TipSearchModeName);
    }
    case R::AddressSearchContent: return text(I::SearchModeContent);
    case R::AddressSearchOptions: return text(I::SearchOptions);
    case R::ContentIndexManage:
    case R::SettingsContentIndex: return text(I::ContentIndexManage);
    case R::NetworkIndexAdd: return text(I::NetworkLiveAdd);
    case R::PaneBannerButton:  // the buttons carry their words; only the close glyph needs one
        return s.hoverControlIndex == static_cast<int>(ui::PaneBannerAction::Dismiss) ? text(I::Close) : L"";
    case R::AddressSearchClear: return text(I::Clear);
    case R::AddressSearchClose: return text(I::Back);
    case R::TabClose: return text(I::TooltipCloseTab);
    case R::Tab: {
        if (!s.pane) return L"";
        int i = s.hoverControlIndex;
        if (s.tabDragging && i >= 0 && i < static_cast<int>(s.tabOrder.size()))
            i = s.tabOrder[static_cast<size_t>(i)];
        if (i < 0 || i >= static_cast<int>(s.window_tabs.items.size()) ||
            !s.window_tabs.items[static_cast<size_t>(i)])
            return L"";
        const auto& layout_tab = *s.window_tabs.items[static_cast<size_t>(i)];
        std::wstring label = app::LayoutTabTitle(layout_tab);
        const auto* folder = layout_tab.ActiveFolder();
        if (folder && !folder->current_path.empty())
            label += L" — " + pulse::path::StripExtendedPathPrefix(folder->current_path);
        return label;
    }
    case R::TabNew: return text(s.appPrefs.show_hints ? I::TipxNewTab : I::TooltipNewTab);
    case R::ThemeToggle: return text(I::TooltipToggleTheme);
    case R::StatusUndo: return s.ops.CanUndo() ? s.ops.UndoLabel() + L"  Ctrl+Z" : text(I::HistoryLocked);
    case R::StatusRedo: return text(I::HistoryRedo) + L"  Ctrl+Y";
    case R::StatusUndoHistory: return text(I::OpHistory);
    case R::StatusBarAi: return text(I::AiChipTip);   // Pulse 助手  Ctrl+J
    case R::WindowManageButton: return text(I::WindowManage);
    case R::SettingsButton: return text(I::Settings);
    case R::SettingsFind: return text(I::SettingsFind);
    case R::SettingsNav: {
        if (s.hoverControlIndex == 5) return l10n::Get(l10n::StringId::QuickPreview).c_str();
        if (s.hoverControlIndex == 2) return l10n::Get(l10n::StringId::ContextMenu).c_str();  // one entry for both pages
        const I names[]={I::SettingsGeneral,I::SettingsSearchIndex,I::SettingsContextMenu,I::SettingsAboutDiagnostics,I::SettingsDuplicates};
        return s.hoverControlIndex>=0 && s.hoverControlIndex<5 ? text(names[s.hoverControlIndex]) : L"";
    }
    case R::SettingsEffect: {
        if (s.hoverControlIndex >= 0 && s.hoverControlIndex < ui::kWindowEffectCount) {
            static constexpr I effects[] = {
                I::EffectNone, I::EffectAcrylic, I::EffectMica, I::EffectMicaAlt,
            };
            wchar_t label[128]{};
            swprintf_s(label, text(I::WindowEffectFormat).c_str(),
                       text(effects[s.hoverControlIndex]).c_str());
            return label;
        }
        return text(I::SettingsWindowEffect);
    }
    case R::SettingsWallpaper:
        return text(s.hoverControlIndex == 1
            ? I::TooltipClearBackground : I::TooltipChooseBackground);
    case R::SettingsDensity: return text(I::SettingsRowHeight);
    case R::SettingsFolderSort: return text(I::SettingsFolderSort);
    case R::SettingsStartupOpen: return text(I::SettingsStartupOpen);
    case R::SettingsNotifyIcon: return text(I::SettingsNotifyIcon);
    case R::SettingsNewTabOpen: return text(I::SettingsNewTabOpen);
    case R::SettingsExternalOpen: return text(I::SettingsExternalOpen);
    case R::SettingsBlankClick: return text(I::SettingsBlankClickBack);
    case R::SettingsHomeFolder:
        return text(s.hoverControlIndex == 1 ? I::ThisPc : I::SettingsHomeFolderPick);
    case R::SettingsThumbCache: return text(I::ThumbCacheTooltip);
    case R::SettingsTextRender: return text(I::SettingsTextRender);
    case R::SettingsUiFontSize: return text(I::SettingsUiFontSize);
    case R::SettingsDoubleCtrlGap: return text(I::GlobalSearchDoubleCtrlGap);
    case R::SettingsWallpaperLook: return text(I::SettingsWallpaperLook);
    case R::SettingsWallpaperBlur: return text(I::SettingsWallpaperBlur);
    case R::SettingsFrameLook: return text(I::SettingsFrameLook);
    case R::SettingsAccent:
        return text(s.hoverControlIndex == 0 ? I::TooltipFollowAccent : I::SettingsThemeColor);
    case R::SettingsToggle:
        if (s.hoverControlIndex == 3) return text(I::SettingsOpenFolders);
        if (s.hoverControlIndex == 4) return text(I::SettingsShowPerformance);
        return L"";
    case R::SettingsIndexExcludeAction:
        if (s.hoverControlIndex == 1) return text(I::SettingsSystemFoldersTip);
        return s.hoverControlIndex == 0 ? text(I::TooltipAddExclusion) : L"";
    case R::SettingsIndexExcludeRemove: return text(I::TooltipRemoveExclusion);
    case R::SettingsDiagnosticsAction: {
        static constexpr I actions[] = {
            I::OpenDiagnostics, I::ClearDiagnostics, I::ExportDiagnostics,
        };
        return s.hoverControlIndex >= 0 && s.hoverControlIndex < 3
            ? text(actions[s.hoverControlIndex]) : L"";
    }
    case R::SettingsUpdateAction:
        return text(s.hoverControlIndex == 1 ? I::DownloadUpdate : I::CheckForUpdates);
    case R::Minimize: return text(I::Minimize);
    case R::Maximize: return text(s.maximized ? I::Restore : I::Maximize);
    case R::Close: return text(I::Close);
    case R::NavBack: return text(I::Back);
    case R::NavForward: return text(I::Forward);
    case R::NavUp: return text(I::Up);
    case R::NavRefresh: return text(I::Refresh);
    case R::NewButton:
        if (const app::Tab* tab = ActiveTab(s); tab && IsConnectServerPage(tab->current_path))
            return text(I::ConnectServerHint);
        return ToolbarNewCreatesFolder(s) ? text(I::NewFolder) + L" (Ctrl+Shift+N)" : text(I::NewTextDocument);
    case R::NewButtonArrow:
        if (const app::Tab* tab = ActiveTab(s); tab && IsConnectServerPage(tab->current_path))
            return text(I::ConnectServerMenu);
        return text(I::New);
    case R::Cut: return text(I::CutShortcut);
    case R::Copy: return text(I::CopyShortcut);
    case R::Paste: return text(I::PasteShortcut);
    case R::Rename: return text(I::RenameShortcut);
    case R::Delete: return text(I::DeleteShortcut);
    case R::SplitButton: return text(s.appPrefs.show_hints ? I::TipxSplit : I::SplitLayout);
    case R::DetailsToggle: return text(s.showDetailsPanel ? I::CollapseDetails : I::ExpandDetails);
    case R::PaneMediumIcons: return text(I::MediumIcons);
    case R::PaneWall: return text(I::PhotoWall);
    case R::PaneWallHint:  // Switch says its word; the close glyph needs one
        return s.hoverControlIndex == 2 ? text(I::PhotoWallSuggestNever) : L"";
    case R::PaneSizeSlider:
    case R::PaneSizeButton: {  // the icon size it is at, e.g. "Large icons · 136"
        const app::Tab* tab = ActiveTab(s);
        if (!tab || !ui::IsZoomIconMode(tab->view_mode)) return L"";
        const I mode = tab->view_mode == ui::ViewMode::ExtraLargeIcons ? I::ViewExtraLarge
            : tab->view_mode == ui::ViewMode::LargeIcons ? I::ViewLarge : I::MediumIcons;
        const float dip = ui::IconDip(tab->view_mode, tab->icon_dip, s.scale);
        if (tab->wall)  // the wall is sized by its row height (prototype: size x 0.78)
            return text(I::PhotoWall) + L" \u00B7 " + text(I::WallRowHeight) + L" " +
                   std::to_wstring(static_cast<int>(dip * 0.78f + 0.5f));
        return text(mode) + L" \u00B7 " + std::to_wstring(static_cast<int>(dip + 0.5f));
    }
    case R::PaneDetails: return text(I::ViewDetails);
    case R::ToolbarSort: return text(I::SortBy);
    case R::ToolbarGroup: return text(I::GroupBy);
    case R::ToolbarGroupClear: return text(I::GroupClear);
    case R::ToolbarMore:   // the customize button (T5); silent while its picker is open
        return ui::ToolbarPickerOpen() ? std::wstring() : text(I::CustomizeToolbar);
    case R::ToolbarPin: {
        const auto pins = ToolbarPinActions(s);
        if (s.hoverControlIndex >= static_cast<int>(pins.size()))
            return ToolbarAppTooltip(s, static_cast<size_t>(s.hoverControlIndex - static_cast<int>(pins.size())));
        return s.hoverControlIndex >= 0 ? pins[static_cast<size_t>(s.hoverControlIndex)].label : std::wstring{};
    }
    case R::FavoritesBarItem: {
        ui::HitTestResult hit;
        hit.region = R::FavoritesBarItem;
        hit.index = s.hoverControlIndex;
        return FavoritesBarTooltip(s, hit);
    }
    case R::FavoritesBarMore: return text(I::FavoritesBarMore);
    case R::SidebarBarSwitch:
        return text(I::FavoritesBarMoveToBar) + L" (" + app::ShownShortcut(L"Ctrl+Shift+B") + L")";
    case R::FavoritesBarDock:
        return text(I::FavoritesBarMoveToSidebar) + L" (" + app::ShownShortcut(L"Ctrl+Shift+B") + L")";
    case R::FavoritesBarChevron: return text(I::FavoritesBarSubfolders);
    case R::AddressFavorites: return text(I::StarredItems);
    case R::ToolbarSelAction: {
        if (s.hoverControlIndex == ui::kSelChipMore) return text(I::MoreActions);
        if (s.hoverControlIndex == ui::kSelChipClear) return text(I::ClearSelectionEsc);
        if (s.hoverControlIndex == ui::kSelChipPin) return text(I::SelChipPinTo);
        if (s.hoverControlIndex == ui::kSelChipLabel) {
            // "2 个文件夹、1 个文件 · 24.6 MB"
            const app::Tab* tab = ActiveTab(s);
            if (!tab || tab->content_results || !tab->snapshot) return L"";
            uint64_t bytes = 0;
            int files = 0, folders = 0;
            tab->SelectionSizeSummary(&bytes, &files, &folders);
            std::wstring tip = text(I::SelChipSummary);
            if (const size_t at = tip.find(L"%s"); at != std::wstring::npos) tip.replace(at, 2, std::to_wstring(folders));
            if (const size_t at = tip.find(L"%s"); at != std::wstring::npos) tip.replace(at, 2, std::to_wstring(files));
            if (files > 0) tip += L" \u00B7 " + pulse::format::ByteSize(bytes, true);
            return tip;
        }
        if (s.hoverControlIndex < 0) return L"";
        const auto actions = SelectionChipActions(s);
        return s.hoverControlIndex < static_cast<int>(actions.size())
            ? actions[static_cast<size_t>(s.hoverControlIndex)].label : std::wstring{};
    }
    case R::PaneColumnLayout: return text(I::ColumnLayout);
    case R::PaneExpand:
    case R::PaneLayoutZoom: return text(I::PaneZoom) + L"  Ctrl+Alt+Enter";
    case R::PaneLayoutRestore: return text(I::PaneZoomRestore);
    case R::PaneLayoutCompare: return text(I::CompareSideBySide);
    case R::PaneLayoutTray: return text(I::SplitLayout);
    case R::PaneLayoutCell: return text(I::LayoutPeekHint);
    case R::PaneLayoutPreset: {
        const I names[] = {I::LayoutSingle,I::LayoutVertical,I::LayoutHorizontal,I::LayoutThree,I::LayoutFour};
        if(s.hoverControlIndex<0||s.hoverControlIndex>4)return {};
        const wchar_t* keys[]={L"  Ctrl+Alt+1",L"  Ctrl+Alt+2",L"",L"  Ctrl+Alt+3",L"  Ctrl+Alt+4"};
        return text(names[s.hoverControlIndex])+keys[s.hoverControlIndex];
    }
    case R::PaneClose: return text(I::WorkspaceClosePane);
    case R::ColumnStripNav:  // names the arrow; the second line tells the wheel does the same
        return text(s.hoverControlIndex == ui::kColumnStripNavLeft ? I::ColumnStripScrollLeft
                                                                   : I::ColumnStripScrollRight) +
               (s.appPrefs.show_hints ? L"\n" + text(I::ColumnStripWheelHint) : std::wstring());
    case R::PaneViewButton: return text(I::More);
    case R::FilterBox: return text(s.appPrefs.show_hints ? I::TipxFilter : I::FilterCurrent);
    case R::FilterClear: return text(I::Clear);
    case R::Splitter:
        if (s.hoverControlIndex >= ui::kSplitterSwapIndex)
            return text(LayoutOf(s) == app::LayoutPreset::TwoHorizontal ? I::SplitSwapTopBottom
                                                                         : I::SplitSwapLeftRight);
        // Motion 4b: while dragging, the ratio badge speaks instead.
        if (s.splitterDragging) return {};
        return text(I::ResizeSplit);
    case R::DetailsOpen: return text(I::Open);
    case R::DetailsStar: return text(I::Favorite);
    case R::DetailsMore: return text(I::MoreActions);
    case R::DetailsRename: return text(I::Rename);
    case R::DetailsTagAdd: return text(I::AddTag);
    case R::DetailsResize: return text(I::ResizeDetails);
    case R::DetailsPreviewToggle: return text(s.detailsPreviewOnly ? I::PreviewExpandDetails : I::PreviewCollapseDetails);
    // Names the action, matching the star and rename hints beside it.
    case R::DetailsPreviewEnable:
        return text(s.detailsPreviewEnabled ? I::PreviewHide : I::PreviewShow);
    case R::DetailsPreview: return L"";
    case R::StatusBarTask:
        if (s.hoverSubIndex == 0) return s.ops.UndoLabel();
        if (s.hoverSubIndex == 1) return text(I::OpLocateResults) + L" · " + s.ops.Status().destination_dir;
        if (s.hoverSubIndex == 2) return text(I::OpResume);
        return text(s.ops.Status().active ? I::OpDetails : I::OpHistory);
    case R::StatusBarCancelSearch: return text(I::ContentCancelSearch);
    case R::DetailsNewTab: return text(I::OpenNewTab);
    case R::DetailsCopyPath: return text(I::CopyPath);
    case R::DetailsSection: return text(I::ExpandCollapse);
    case R::DetailsRating: return text(I::ColumnRating);
    case R::DetailsAttrToggle:
        switch (s.hoverControlIndex) {
        case 0: return text(I::ReadOnly);
        case 1: return text(I::Hidden);
        case 2: return text(I::SystemAttributes);
        default: return L"";
        }
    case R::DetailsSecurityChange: return text(I::SystemAttributes);
    case R::RowStar: return text(s.appPrefs.show_hints ? I::TipxStar : I::Star);
    case R::RowNewTab: return text(s.appPrefs.show_hints ? I::TipxRowNewTab : I::OpenNewTab);
    case R::RowMore: return text(I::MoreActions);
    case R::RowTagDot: {
        // The dot's tag, and what a click does.
        if (s.hoverSubIndex < 0 || s.hoverSubIndex >= static_cast<int>(s.places.tags.size())) return L"";
        wchar_t tip[256]{};
        swprintf_s(tip, l10n::Get(I::TagDotTip).c_str(),
                   s.places.tags[static_cast<size_t>(s.hoverSubIndex)].name.c_str());
        return tip;
    }
    case R::SidebarWorkspaceAction:
        switch (s.hoverSubIndex) {
        case 0: return text(I::OpenNewTab);
        case 1: return text(I::More);
        case 2: return text(I::WorkspaceUpdateLayout);
        case 3: return text(I::WorkspaceRestore);
        case 4: return text(I::WorkspaceSaveCurrent);
        default: return {};
        }
    case R::SidebarItemAction:
        return text(IsVerticalTabPath(s.hoverPath) ? I::TooltipCloseTab : I::Unpin);
    case R::WindowIdentity: return text(I::WindowIdentityTip);
    case R::SidebarToggle:
        // The extra line only describes collapsing; an expanded-from-rail state keeps the plain name.
        return text(s.appPrefs.show_hints && !SidebarRailActive(s) ? I::TipxSidebar : I::ToggleSidebar);
    // On the icon rail there is no text to read, so every row (and every folded
    // section's icon) names itself on hover.
    case R::SidebarHeader: return SidebarRailActive(s) ? s.hoverLabel : L"";
    case R::SidebarItem: {
        if (SidebarRailActive(s) && !s.hoverLabel.empty()) return s.hoverLabel;
        if (const app::StarredItem* starred = s.places.FindStarred(s.hoverPath);
            starred && !starred->badge.empty()) {
            return starred->badge;
        }
        if (const app::QuickAccessBadge* badge = s.places.FindQuickAccessBadge(s.hoverPath);
            badge && !badge->badge.empty()) {
            return badge->badge;
        }
        return L"";
    }
    case R::Row: {
        app::Pane* pane = PaneAtSlot(s, s.hoverPaneIndex);
        app::Tab* tab = pane ? pane->ActiveTab() : ActiveTab(s);
        if (tab && tab->snapshot && s.hoverControlIndex >= 0 &&
            s.hoverControlIndex < static_cast<int>(tab->EntryCount()) &&
            !IsDetailsTreePlaceholder(*tab, s.hoverControlIndex)) {
            const auto& entry = tab->EntryAt(s.hoverControlIndex);
            std::wstring tooltip = entry.name;
            if (!entry.change_type_text.empty()) {
                tooltip += L" · " + entry.change_type_text + L" · " + entry.full_path;
                if (!entry.change_old_path.empty() && entry.change_old_path != entry.full_path)
                    tooltip += L" (" + entry.change_old_path + L" → " + entry.full_path + L")";
                return tooltip;
            }
            std::wstring full = entry.full_path;
            if (full.empty() && !fs::IsVirtualPath(tab->current_path)) {
                full = tab->current_path;
                if (!full.empty() && !full.ends_with(L"\\")) full += L"\\";
                full += entry.name;
            }
            // Narrow panes drop columns; keep their facts reachable on hover.
            if (tab->view_mode == ui::ViewMode::Details) {
                using K = ui::MainRenderer::ColumnKind;
                const uint32_t shown = s.renderer.PaintedColumnMask(std::max(0, s.hoverPaneIndex));
                auto hidden = [&](K kind) { return shown && !(shown & (1u << static_cast<uint32_t>(kind))); };
                std::wstring view_kind;
                app::ParsePulsePath(tab->current_path, &view_kind, nullptr);
                const bool search = view_kind == L"search";
                if (search && hidden(K::Path) && !full.empty()) {
                    const size_t slash = full.find_last_of(L'\\');
                    if (slash != std::wstring::npos && slash > 0)
                        tooltip += L" · " + full.substr(0, slash);
                }
                if (hidden(K::Date) && (entry.mtime.dwLowDateTime || entry.mtime.dwHighDateTime))
                    tooltip += L" · " + pulse::l10n::Get(pulse::l10n::StringId::ColumnModified) + L" " +
                               pulse::format::LocalFileTime(entry.mtime);
            }
            if (const auto* indices = s.places.TagIndicesForPath(full); indices && !indices->empty()) {
                tooltip += pulse::l10n::Get(pulse::l10n::StringId::TooltipTags).c_str();
                bool first = true;
                for (int index : *indices) {
                    if (index < 0 || index >= static_cast<int>(s.places.tags.size())) continue;
                    if (!first) tooltip += L"、";
                    tooltip += s.places.tags[static_cast<size_t>(index)].name;
                    first = false;
                }
            }
            if (const app::StarredItem* starred = s.places.FindStarred(full);
                starred && !starred->badge.empty()) {
                tooltip += pulse::l10n::Get(pulse::l10n::StringId::TooltipBadge).c_str() + starred->badge;
            }
            if (tab->compare_marks &&
                static_cast<size_t>(s.hoverControlIndex) < tab->compare_marks->size()) {
                using CM = ui::CompareMark;
                using I = pulse::l10n::StringId;
                switch (static_cast<CM>((*tab->compare_marks)[static_cast<size_t>(s.hoverControlIndex)])) {
                case CM::OnlyHere: tooltip += L" \xB7 " + pulse::l10n::Get(I::CompareOnlyHere); break;
                case CM::Newer: tooltip += L" \xB7 " + pulse::l10n::Get(I::CompareNewer); break;
                case CM::Older: tooltip += L" \xB7 " + pulse::l10n::Get(I::CompareOlder); break;
                case CM::Differs: tooltip += L" \xB7 " + pulse::l10n::Get(I::CompareDiffers); break;
                default: break;
                }
            }
            // Only the name and it is fully visible: a tooltip would just repeat
            // it (B站 #15). Shortened names and extra facts still show.
            if (tooltip.size() == entry.name.size() &&
                !s.renderer.HoveredNameTruncated(std::max(0, s.hoverPaneIndex), s.hoverControlIndex))
                return L"";
            return tooltip;
        }
        return L"";
    }
    default: return L"";
    }
}

// Full path of a directory entry (normalized, empty when out of range).
std::wstring EntryFullPath(const app::Tab& tab, int index) {
    if (!tab.snapshot || index < 0 || index >= static_cast<int>(tab.EntryCount())) return L"";
    const fs::DirEntry& e = tab.EntryAt(static_cast<size_t>(index));
    if (e.change_record_only || !e.recycle_path.empty()) return L"";
    if (!e.full_path.empty()) return e.full_path;
    if (fs::IsVirtualPath(tab.current_path)) return L"";
    std::wstring full = tab.current_path;
    if (!full.ends_with(L"\\")) full += L"\\";
    full += e.name;
    return full;
}

std::vector<std::wstring> SelectedFullPaths(const app::Tab& tab) {
    std::vector<std::wstring> out;
    if (!tab.snapshot) return out;
    const auto indices = tab.SelectedIndices();
    for (int index : indices) {
        if (index >= 0 && static_cast<size_t>(index) < tab.EntryCount() &&
            !tab.EntryAt(static_cast<size_t>(index)).recycle_path.empty()) return out;
    }
    out.reserve(indices.size());
    for (int index : indices) {
        std::wstring full = EntryFullPath(tab, index);
        if (!full.empty()) out.push_back(std::move(full));
    }
    return out;
}

bool IsDeviceTab(const app::Tab* tab) {
    return tab && fs::IsDevicePath(tab->current_path);
}

std::vector<std::wstring> SelectedShellIds(const app::Tab& tab) {
    std::vector<std::wstring> out;
    if (!tab.snapshot) return out;
    const auto indices = tab.SelectedIndices();
    for (int index : indices) {
        if (index >= 0 && static_cast<size_t>(index) < tab.EntryCount() &&
            !tab.EntryAt(static_cast<size_t>(index)).recycle_path.empty()) return out;
    }
    out.reserve(indices.size());
    for (int index : indices) {
        if (index < 0 || index >= static_cast<int>(tab.EntryCount())) continue;
        const std::wstring& id = tab.EntryAt(static_cast<size_t>(index)).shell_id;
        if (id.empty()) return {};
        out.push_back(id);
    }
    return out;
}

bool RunDeviceVerb(AppState& s, std::vector<std::wstring> shell_ids, const wchar_t* verb, bool background) {
    if (shell_ids.empty()) return false;
    return s.ops.RunShellVerb(std::move(shell_ids), verb ? verb : L"", background, s.hwnd);
}

bool SubmitDeviceTransfer(AppState& s, std::vector<std::wstring> sources, const std::wstring& dest,
                          bool move, uint64_t* task) {
    if (task) *task = 0;
    if (sources.empty() || dest.empty()) return false;
    ops::OpRequest req;
    req.type = move ? ops::OpType::Move : ops::OpType::Copy;
    if (fs::IsDevicePath(dest)) {
        auto& devices = app::DeviceRegistry::Instance();
        if (!devices.FolderWritable(dest)) return false;
        req.dest_dir = devices.FolderShellId(dest);
    } else if (!fs::IsVirtualPath(dest)) {
        req.dest_dir = ClipboardPath(dest);
    }
    if (req.dest_dir.empty()) return false;
    req.sources.reserve(sources.size());
    for (auto& source : sources)
        req.sources.push_back(ops::IsShellItemId(source) ? std::move(source) : ClipboardPath(source));
    if (!ops::IsShellTransferRequest(req)) return false;
    // Both ends re-list: the target, and the phone folder a move empties.
    req.refresh_directories.push_back(dest);
    if (const app::Tab* tab = ActiveTab(s); IsDeviceTab(tab) && _wcsicmp(tab->current_path.c_str(), dest.c_str()) != 0)
        req.refresh_directories.push_back(tab->current_path);
    const uint64_t id = s.ops.Submit(std::move(req));
    if (task) *task = id;
    return id != 0;
}

std::wstring DeviceFriendlyPath(const std::wstring& path) {
    const auto where = fs::ParseDevicePath(path);
    if (!where) return path;
    std::wstring name;
    if (!app::DeviceRegistry::Instance().Describe(where->device, &name, nullptr) || name.empty())
        name = where->device;
    std::wstring out = std::wstring(l10n::Get(l10n::StringId::ThisPc).c_str()) + L"\\" + name;
    if (!where->storage.empty()) out += L"\\" + where->storage;
    for (const auto& part : where->parts) out += L"\\" + part;
    return out;
}

std::wstring TagDiscoveryKey(std::wstring path) {
    path = fs::NormalizePath(std::move(path));
    for (auto& c : path) c = static_cast<wchar_t>(std::towlower(c));
    return path;
}

void ApplyTagAdsDiscoveries(AppState& s, const std::vector<TagAdsDiscovery>& discoveries) {
    const uint64_t before = s.places.TagRevision();
    for (const auto& discovery : discoveries) {
        const std::wstring key = TagDiscoveryKey(discovery.path);
        s.tagAdsDiscoveryQueued.erase(key);
        s.tagAdsDiscoveryChecked.insert(key);
        s.places.MergeAdsRecords(discovery.path, discovery.records, discovery.legacy_names);
    }
    if (s.places.TagRevision() != before && s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void QueueVisibleTagDiscovery(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->loading || !tab->snapshot || tab->EntryCount() == 0) return;

    ui::PaneViewModel pane;
    app::FillPaneViewModel(pane, *s.pane, &s.places);
    const auto bounds = FocusedPaneRect(s);
    const auto list = s.renderer.PaneListRect(pane, bounds);
    if (list.right <= list.left || list.bottom <= list.top) return;
    const auto [first, last] = s.renderer.VisibleRangeInPane(pane, bounds);
    if (first < 0 || last < first) return;
    std::vector<std::wstring> paths;
    const int end = std::min(last, static_cast<int>(pane.EntryCount()) - 1);
    for (int view_row = first; view_row <= end; ++view_row) {
        const auto item = s.renderer.ItemRectInPane(pane, bounds, view_row);
        if (item.right <= list.left || item.left >= list.right || item.bottom <= list.top ||
            item.top >= list.bottom || item.right <= item.left || item.bottom <= item.top) continue;
        const int source = pane.SourceIndex(view_row);
        const std::wstring full = EntryFullPath(*tab, source);
        if (full.empty() || fs::IsVirtualPath(full) || s.places.TagIndicesForPath(full)) continue;
        const std::wstring key = TagDiscoveryKey(full);
        if (key.empty() || s.tagAdsDiscoveryChecked.contains(key) ||
            !s.tagAdsDiscoveryQueued.insert(key).second) {
            continue;
        }
        paths.push_back(full);
    }
    if (paths.empty()) return;

    const HWND notify = ServiceWindow(s);
    s.worker.EnqueueIo([paths = std::move(paths), notify] {
        auto discoveries = std::make_unique<std::vector<TagAdsDiscovery>>();
        discoveries->reserve(paths.size());
        for (const auto& path : paths) {
            TagAdsDiscovery discovery;
            discovery.path = path;
            discovery.records = app::ReadTagAdsV2(path);
            if (discovery.records.empty()) discovery.legacy_names = app::ReadTagAds(path);
            discoveries->push_back(std::move(discovery));
        }
        if (notify && PostMessageW(notify, WM_TAG_ADS_DISCOVERED, 0,
                                   reinterpret_cast<LPARAM>(discoveries.get()))) {
            discoveries.release();
        }
    });
}

// Full path of the focused selected entry (normalized, empty when nothing selected).
std::wstring SelectedFullPath(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return L"";
    return EntryFullPath(*tab, tab->selected_index);
}

void SyncSavedSearchSidebar(AppState& s) {
    s.sidebar.saved_searches.clear();
    const auto& searches = s.savedSearches.items();
    s.sidebar.saved_searches.reserve(searches.size());
    for (size_t i = 0; i < searches.size(); ++i) {
        app::SidebarEntry entry;
        entry.label = searches[i].name;
        entry.detail = searches[i].root;
        entry.glyph = searches[i].mode == app::SavedSearchMode::Duplicates
            ? L"\xE8EF" : L"\xE721";
        entry.fallback = searches[i].mode == app::SavedSearchMode::Duplicates
            ? L"Dup" : L"Find";
        entry.color = ui::HexColor(searches[i].mode == app::SavedSearchMode::Content
            ? 0x0EA5E9 : searches[i].mode == app::SavedSearchMode::Duplicates
                ? 0xF59E0B : 0x22C55E);
        if (searches[i].mode == app::SavedSearchMode::Tags) {
            // A smart view: a saved tag combination, marked with the tag glyph.
            entry.detail.clear();
            entry.glyph = L"\xE8EC";
            entry.fallback = L"Tag";
            entry.color = ui::HexColor(0xA855F7);
        }
        entry.path = L"pulse:saved-search:" + std::to_wstring(i);
        s.sidebar.saved_searches.push_back(std::move(entry));
    }
}

// ---------------------------------------------------------------------------
// Phone folder watch (UpdateDeviceFolderWatch). UI thread only, but for the
// worker that parses the folder's PIDL and posts it back.
// ---------------------------------------------------------------------------
namespace {
struct DeviceFolderWatch {
    std::wstring path;   // the pulse:device: folder watched (or being parsed)
    ULONG id = 0;        // SHChangeNotifyRegister registration, 0 = none yet
    uint64_t ticket = 0; // matches the worker's answer to this path
};
std::unordered_map<HWND, DeviceFolderWatch> g_device_folder_watches;
uint64_t g_device_folder_ticket = 0;
// A copy of many files raises one notification per file: one re-list after
// the burst instead.
constexpr UINT_PTR kTimerDeviceWatch = 0x4457;  // 'DW'
constexpr UINT kDeviceWatchSettleMs = 400;
}  // namespace

void UpdateDeviceFolderWatch(AppState& s) {
    if (!s.hwnd) return;
    const app::Tab* tab = ActiveTab(s);
    const std::wstring wanted = IsDeviceTab(tab) ? tab->current_path : std::wstring();
    const auto found = g_device_folder_watches.find(s.hwnd);
    if (found != g_device_folder_watches.end() && _wcsicmp(found->second.path.c_str(), wanted.c_str()) == 0) return;
    ReleaseDeviceFolderWatch(s.hwnd);
    if (wanted.empty()) return;
    const std::wstring folder_id = app::DeviceRegistry::Instance().FolderShellId(wanted);
    if (folder_id.empty()) return;
    DeviceFolderWatch& watch = g_device_folder_watches[s.hwnd];
    watch.path = wanted;
    watch.ticket = ++g_device_folder_ticket;
    const HWND hwnd = s.hwnd;
    const uint64_t ticket = watch.ticket;
    std::thread([hwnd, ticket, folder_id] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(shell_id::ParseId(folder_id, &pidl)) && pidl &&
            !PostMessageW(hwnd, WM_DEVICE_WATCH_READY, static_cast<WPARAM>(ticket), reinterpret_cast<LPARAM>(pidl)))
            CoTaskMemFree(pidl);
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

void OnDeviceWatchReady(AppState& s, WPARAM ticket, LPARAM pidl_param) {
    auto pidl = reinterpret_cast<PIDLIST_ABSOLUTE>(pidl_param);
    const auto found = g_device_folder_watches.find(s.hwnd);
    if (found != g_device_folder_watches.end() && found->second.ticket == static_cast<uint64_t>(ticket) &&
        !found->second.id && pidl) {
        SHChangeNotifyEntry entry{pidl, FALSE};
        found->second.id = SHChangeNotifyRegister(s.hwnd, SHCNRF_ShellLevel | SHCNRF_NewDelivery,
            SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER |
                SHCNE_UPDATEDIR | SHCNE_UPDATEITEM,
            WM_DEVICE_FOLDER_CHANGED, 1, &entry);
    }
    if (pidl) CoTaskMemFree(pidl);  // the registration keeps its own copy
}

void OnDeviceFolderChanged(AppState& s, WPARAM wparam, LPARAM lparam) {
    PIDLIST_ABSOLUTE* pidls = nullptr;
    LONG event = 0;
    if (HANDLE lock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wparam), static_cast<DWORD>(lparam),
                                                &pidls, &event))
        SHChangeNotification_Unlock(lock);
    if (g_device_folder_watches.contains(s.hwnd)) SetTimer(s.hwnd, kTimerDeviceWatch, kDeviceWatchSettleMs, nullptr);
}

void FireDeviceFolderWatch(AppState& s) {
    KillTimer(s.hwnd, kTimerDeviceWatch);
    const auto found = g_device_folder_watches.find(s.hwnd);
    if (found == g_device_folder_watches.end() || found->second.path.empty()) return;
    s.store.MarkDirty(found->second.path);
    const app::Tab* tab = ActiveTab(s);
    if (tab && _wcsicmp(tab->current_path.c_str(), found->second.path.c_str()) == 0)
        RefreshActiveTab(s, RefreshReason::ShellNotification);
}

void ReleaseDeviceFolderWatch(HWND hwnd) {
    const auto found = g_device_folder_watches.find(hwnd);
    if (found == g_device_folder_watches.end()) return;
    if (found->second.id) SHChangeNotifyDeregister(found->second.id);
    g_device_folder_watches.erase(found);
    KillTimer(hwnd, kTimerDeviceWatch);
}

} // namespace pulse
