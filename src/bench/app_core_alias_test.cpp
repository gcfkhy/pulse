// app_core_alias_test.cpp - Multi-window M1: AppState's process-wide members
// are references into one AppCore, per-window members stay owned by AppState.
//
// Checked at compile time (a broken alias fails the build); the executable
// reports the checked list as [PASS] lines.
#include "../app/app_state.h"
#include "../ui/ui_renderer.h"

#include <cstdio>
#include <type_traits>

using pulse::AppCore;
using pulse::AppCoreOwner;
using pulse::AppState;

namespace {

// Every AppCore member AppState still exposes under its old name.
#define PULSE_APP_CORE_ALIASES(X) \
    X(store) X(worker) X(watches) X(notify_mu) X(notify_queue) X(size_patches) X(last_unc_poll) \
    X(sidebar) X(sidebarRefresh) X(networkLocations) X(recycle_info) X(recycle_refresh_at) \
    X(recycle_refresh_left) X(recycle_ignore_items) X(recycle_info_guard) X(places) \
    X(ctxMenuPrefs) X(context_menu) X(appPrefs) X(searchHistory) X(searchHistoryWriter) \
    X(settings) X(update_checker) X(update_result) X(update_result_ready) X(update_installer) \
    X(update_install_error) X(next_update_progress_paint) X(next_update_check) \
    X(notified_update_version) X(whatsNewVersion) X(whatsNewAt) X(tray_controller) \
    X(globalSearchHotkey) X(globalSearchWindow) X(single_instance) X(tagAdsResultRevision) \
    X(tagAdsDiscoveryQueued) X(tagAdsDiscoveryChecked) X(index) X(folderSizes) X(changes) \
    X(networkIndex) X(contentSearch) X(duplicateSearch) X(duplicateScan) X(dup_volume_cache) \
    X(dup_volume_cache_tick) X(dup_volume_cache_pending) X(savedSearches) \
    X(pendingIndexSearches) X(liveNetworkSearches) X(sessionSavedJson) X(updateSessionPrepared) \
    X(restoreUpdateSession) X(prefsSavedJson) X(sessionAutosaveCheck) X(nextIndexReq) \
    X(probe_scheduler) X(probeUnc) X(probeQueue) X(darkMode) X(themeOverride) X(safeMode) \
    X(startupComplete) X(isolatedTest) X(isolatedTestPersist) X(contentIndexObserver) \
    X(accentColor) X(session_path) X(gitRoots) X(resultMutex) X(results) X(ops) X(shell_windows) \
    X(shell_windows_published) X(explorer_takeover) X(cutPaths) X(pendingCutClipboardSequence) \
    X(pendingCutClipboardPaths) \
    X(completedCutClipboardPaths)

template <typename Alias, typename Field>
constexpr bool kIsCoreAlias = std::is_lvalue_reference_v<Alias> &&
                              !std::is_reference_v<Field> &&
                              std::is_same_v<Alias, Field&>;

#define PULSE_CHECK_CORE_ALIAS(name)                                                   \
    static_assert(kIsCoreAlias<decltype(AppState::name), decltype(AppCore::name)>,  \
                  "AppState::" #name " must be a reference to AppCore::" #name);
PULSE_APP_CORE_ALIASES(PULSE_CHECK_CORE_ALIAS)

// Per-window state: owned by each AppState, never shared.
#define PULSE_WINDOW_MEMBERS(X) \
    X(hwnd) X(compositor) X(renderer_owner) X(framePump) X(quickPreview) X(window_tabs) X(pane) \
    X(targetPane) X(tabs) X(scale) X(maximized) X(menu) X(dropTarget) X(hwndAddressEdit) \
    X(hwndFilterEdit) X(hwndRenameEdit) X(sidebarScroll) X(showDetailsPanel)
#define PULSE_CHECK_WINDOW_MEMBER(name)                                    \
    static_assert(!std::is_reference_v<decltype(AppState::name)>,         \
                  "AppState::" #name " is per-window state, not an alias");
PULSE_WINDOW_MEMBERS(PULSE_CHECK_WINDOW_MEMBER)
// The renderer is owned per window through renderer_owner (so app_state.h does
// not need ui_renderer.h); `renderer` only names that same object.
static_assert(std::is_same_v<decltype(AppState::renderer), pulse::ui::MainRenderer&>,
              "AppState::renderer names the window's own renderer_owner");
static_assert(std::is_same_v<decltype(AppState::renderer_owner), pulse::ui::MainRendererPtr>,
              "each window owns its renderer");

static_assert(std::is_base_of_v<AppCoreOwner, AppState>,
              "a stand-alone AppState owns its core through AppCoreOwner");
static_assert(std::is_same_v<decltype(AppState::core), AppCore&>, "AppState::core is a reference");
static_assert(std::is_default_constructible_v<AppState>,
              "tests and selftest probes construct AppState without a shared core");
static_assert(std::is_constructible_v<AppState, AppCore&>, "windows bind to a shared AppCore");
static_assert(!std::is_convertible_v<AppCore&, AppState>, "binding to a core is explicit");
static_assert(!std::is_copy_constructible_v<AppState> && !std::is_copy_assignable_v<AppState>,
              "copying a window state would alias its core silently");
static_assert(!std::is_copy_constructible_v<AppCore> && !std::is_copy_assignable_v<AppCore>,
              "the process core is unique");

// Multi-window M2: icon and thumbnail caches are process-wide (render_caches.h);
// each window's renderer binds to AppCore's instance instead of owning one.
static_assert(std::is_same_v<decltype(AppCore::render_caches), pulse::ui::RenderCaches>,
              "AppCore owns the shared render caches");
static_assert(std::is_constructible_v<pulse::ui::MainRenderer, pulse::ui::RenderCaches&>,
              "a window renderer binds to shared caches");
static_assert(!std::is_convertible_v<pulse::ui::RenderCaches&, pulse::ui::MainRenderer>,
              "binding a renderer to caches is explicit");
static_assert(std::is_default_constructible_v<pulse::ui::MainRenderer>,
              "stand-alone renderers (folder picker, tests) own their caches");
static_assert(std::is_same_v<decltype(std::declval<pulse::ui::MainRenderer&>().Caches()), pulse::ui::RenderCaches&>,
              "a renderer exposes the caches it draws from");
static_assert(!std::is_copy_constructible_v<pulse::ui::RenderCaches> && !std::is_copy_constructible_v<pulse::ui::MainRenderer>,
              "copying would split or alias a cache silently");

#define PULSE_NAME(name) #name,
constexpr const char* kAliases[] = {PULSE_APP_CORE_ALIASES(PULSE_NAME)};
constexpr const char* kWindowMembers[] = {PULSE_WINDOW_MEMBERS(PULSE_NAME)};

}  // namespace

int main() {
    for (const char* name : kAliases)
        std::printf("[PASS] AppState::%s aliases AppCore::%s\n", name, name);
    for (const char* name : kWindowMembers)
        std::printf("[PASS] AppState::%s stays per-window\n", name);
    std::printf("[PASS] AppState ownership: default-constructs its own core, binds explicitly to a shared core, non-copyable\n");
    std::printf("app_core_alias_test: %zu aliases, %zu per-window members, 0 failures\n",
                std::size(kAliases), std::size(kWindowMembers));
    return 0;
}
