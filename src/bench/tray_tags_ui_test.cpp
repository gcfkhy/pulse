#ifdef PULSE_WITH_SELFTEST
#include "../app/app_internal.h"
#include "../ui/ui_renderer.h"
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>
#include <cstdio>
namespace {
LRESULT CALLBACK TrayTagsWindow(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<pulse::AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == pulse::WM_TAG_ADS_DISCOVERED) {
        std::unique_ptr<std::vector<pulse::TagAdsDiscovery>> rows(reinterpret_cast<std::vector<pulse::TagAdsDiscovery>*>(lp));
        if (state && rows) pulse::ApplyTagAdsDiscoveries(*state, *rows);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
void Pump(HWND hwnd) { MSG msg{}; while (PeekMessageW(&msg, hwnd, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
}
bool RunTrayTagsUiTest() {
    using namespace pulse;
    std::filesystem::create_directories(L"bench_data");
    FILE* log = nullptr; _wfopen_s(&log, L"bench_data/m18034_tray_tags_test.log", L"w");
    if (!log) return false;
    int failures = 0;
    auto check = [&](bool ok, const char* label) { fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label); fflush(log); failures += !ok; };
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"tray-tags-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    if (!CreateDirectoryW(root.c_str(), nullptr)) { check(false, "exclusive fixture created"); fclose(log); return false; }
    std::vector<std::wstring> files;
    auto file = [&](const std::wstring& name) {
        auto path = (root / name).wstring();
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { DWORD written = 0; WriteFile(h, "private", 7, &written, nullptr); CloseHandle(h); files.push_back(path); }
        return h != INVALID_HANDLE_VALUE ? path : std::wstring();
    };
    auto owner = std::make_unique<AppState>(); auto& s = *owner;
    s.isolatedTest = true; s.shot.active = true;
    s.appPrefs.persist = s.searchHistory.persist = s.ctxMenuPrefs.persist = s.places.persist = false;
    WNDCLASSW wc{}; wc.lpfnWndProc = TrayTagsWindow; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"PulseTrayTagsPrivateTest";
    RegisterClassW(&wc);
    s.hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1000, 640, nullptr, nullptr, wc.hInstance, nullptr);
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    const bool ready = s.hwnd && s.compositor.Init(s.hwnd);
    check(ready, "private compositor initialized without application services");
    s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.NewTab(root.wstring()); s.pane = s.window_tabs.Active()->panes.front().get();
    auto setup = [&](int panes, const fs::SnapshotPtr& entries) {
        s.pane = nullptr; Root(s).reset(); Panes(s).clear(); std::vector<app::Pane*> used;
        for (int i = 0; i < panes; ++i) {
            auto pane = std::make_unique<app::Pane>(); pane->view.current_path = root.wstring();
            pane->view.SetSnapshot(entries); pane->view.loading = false; pane->focused = i == 0;
            used.push_back(pane.get()); Panes(s).push_back(std::move(pane));
        }
        LayoutOf(s) = panes == 1 ? app::LayoutPreset::Single : app::LayoutPreset::TwoVertical;
        Root(s) = app::MakePresetTree(LayoutOf(s), used); LiveLayout(s).focused_index = 0; s.pane = used.front();
    };
    auto wait = [&](const auto& predicate) { const auto deadline = GetTickCount64() + 3000; while (!predicate() && GetTickCount64() < deadline) { Pump(s.hwnd); Sleep(1); } return predicate(); };
    if (ready) {
        s.worker.Start([](app::WorkResult) {});
        int case_number = 0;
        for (float scale : {1.0f, 1.5f}) for (int panes : {1, 2})
        for (ui::ViewMode mode : {ui::ViewMode::List, ui::ViewMode::SmallIcons, ui::ViewMode::Details}) {
            ++case_number;
            auto entries = std::make_shared<std::vector<fs::DirEntry>>();
            bool written = true;
            const std::wstring tag = L"private-discovery-" + std::to_wstring(case_number);
            for (int i = 0; i < 96; ++i) {
                const auto name = std::wstring(i % 2 ? L"hidden-" : L"visible-") + std::to_wstring(case_number) + L"-" + std::to_wstring(i) + (i < 48 ? L".txt" : L".dat");
                const auto path = file(name); fs::DirEntry e; e.name = name; e.full_path = path; e.attrs = FILE_ATTRIBUTE_NORMAL; e.size = 7;
                written &= !path.empty() && app::WriteTagAdsV2(path, {{tag, L"Imported fixture", 0x3399AA}});
                entries->push_back(std::move(e));
            }
            check(written, "private NTFS ADS fixtures written");
            setup(panes, entries); auto* tab = ActiveTab(s); tab->view_mode = mode;
            s.scale = scale; s.renderer.SetScale(scale); s.compositor.RecreateTextFormats(scale); s.compositor.Resize(static_cast<UINT>(1000 * scale), static_cast<UINT>(640 * scale));
            if (mode == ui::ViewMode::Details) {
                tab->group_by = 3; tab->filter_text = L"visible";
                ui::PaneViewModel pane; app::FillPaneViewModel(pane, *s.pane, &s.places);
                check(pane.groups && pane.groups->size() >= 2, "grouped filtered snapshot has two real groups");
                if (pane.groups && !pane.groups->empty()) { tab->collapsed_groups[tab->current_path].insert(pane.groups->front().key); ++tab->group_collapse_rev; }
            }
            for (int stage = 0; stage < (mode == ui::ViewMode::Details ? 1 : 2); ++stage) {
                if (stage) {
                    ui::PaneViewModel current; app::FillPaneViewModel(current, *s.pane, &s.places);
                    const auto bounds = FocusedPaneRect(s);
                    if (mode == ui::ViewMode::List) tab->scroll_x = (std::min)(440.0f * scale, s.renderer.MaxScrollXForPane(current, bounds));
                    else tab->scroll_y = (std::min)(240.0f * scale, s.renderer.MaxScrollForPane(current, bounds));
                }
                ui::PaneViewModel pane; app::FillPaneViewModel(pane, *s.pane, &s.places);
                const auto bounds = FocusedPaneRect(s); const auto list = s.renderer.PaneListRect(pane, bounds);
                std::set<std::wstring> expected;
                // Scan every model row, independently of production VisibleRange.
                for (int row = 0; row < static_cast<int>(pane.EntryCount()); ++row) {
                    const auto box = s.renderer.ItemRectInPane(pane, bounds, row);
                    if (box.right <= box.left || box.bottom <= box.top || box.right <= list.left || box.left >= list.right || box.bottom <= list.top || box.top >= list.bottom) continue;
                    const auto path = EntryFullPath(*tab, pane.SourceIndex(row)); const auto key = TagDiscoveryKey(path);
                    if (!s.tagAdsDiscoveryChecked.contains(key) && !s.places.TagIndicesForPath(path)) expected.insert(key);
                }
                QueueVisibleTagDiscovery(s);
                const std::set<std::wstring> queued(s.tagAdsDiscoveryQueued.begin(), s.tagAdsDiscoveryQueued.end());
                fprintf(log, "[CASE] dpi=%g panes=%d mode=%d stage=%d expected=%zu queued=%zu\n", scale, panes, static_cast<int>(mode), stage, expected.size(), queued.size());
                check(!expected.empty() && queued == expected, "real discovery queue matches all and only visible source identities");
                QueueVisibleTagDiscovery(s);
                check(s.tagAdsDiscoveryQueued.size() == queued.size(), "repeated visibility pass does not duplicate queued ADS work");
                check(wait([&] { return s.tagAdsDiscoveryQueued.empty(); }), "actual background ADS reads reach production merge handler");
                bool imported = true;
                for (const auto& path : files) if (expected.contains(TagDiscoveryKey(path))) imported &= s.places.TagIndicesForPath(path) != nullptr;
                check(imported, "visible files imported their real NTFS ADS tags");
                QueueVisibleTagDiscovery(s); check(s.tagAdsDiscoveryQueued.empty(), "completed discovery remains deduplicated");
            }
        }
        s.worker.Stop(); Pump(s.hwnd);
    }
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, 0); const HWND hwnd = s.hwnd; s.hwnd = nullptr;
    owner.reset(); if (hwnd) DestroyWindow(hwnd);
    bool cleaned = true; for (const auto& path : files) cleaned &= DeleteFileW(path.c_str()) != FALSE;
    cleaned &= RemoveDirectoryW(root.c_str()) != FALSE;
    check(cleaned, "exclusive private fixture and all ADS cleaned up");
    fprintf(log, "failures=%d\n", failures); fclose(log); return failures == 0;
}
#endif
