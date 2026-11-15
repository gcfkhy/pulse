#pragma once
#include <windows.h>
namespace pulse {
struct AppState;
void TickUpdates(AppState& state, unsigned long long now);
void CheckForUpdates(AppState& state);
void InstallUpdate(AppState& state);
// This build only tells about a newer release: the toast and Settings > About
// open its release page in the browser instead of downloading and installing it.
void OpenUpdateRelease(AppState& state);
void CompleteUpdateCheck(AppState& state);
void CompleteUpdateDownload(AppState& state);
LRESULT CloseForUpdate(AppState& state);
// After prefs load: remember the running version; queue the "updated" toast on upgrades.
void NoteRunningVersion(AppState& state);
// Settings > About, scrolled to the newest release note (toast click target).
void ShowReleaseNotes(AppState& state);
}
