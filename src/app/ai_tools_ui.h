// ai_tools_ui.h - AI tools that read or show something in Pulse's windows.
//
// The broker's call thread posts a task to Pulse's host window
// (WM_AI_UI_TASK) and waits a few seconds for the UI thread to run it; a task
// that is cancelled or late is dropped unrun. The UI thread only reads state
// and starts navigation; anything touching the disk happens on the call
// thread first.
#pragma once
#include "ai_broker.h"
#include <atomic>
#include <functional>
#include <windows.h>

namespace pulse { struct AppState; }

namespace pulse::app::ai {

// The host window's WM_AI_UI_TASK; `target` is the most recently active
// window (null when none is open). Frees the task.
void HandleAiUiTask(AppState* target, LPARAM lparam);

// Queues `work` for the UI thread without waiting (false: no host window).
// For callers that track completion themselves (ai_confirm.h); `work` gets
// the most recently active window or null.
bool PostAiUiTask(HWND host, std::function<void(AppState*)> work);
// Runs `work` on the UI thread and waits for it (short work only). False
// without a host window, or when cancelled or not started within 5 s (then
// it never runs, so `work` may capture the caller's locals by reference).
bool RunOnAiUiThread(HWND host, std::function<void(AppState*)> work, const std::atomic<bool>& cancelled);

// pulse.get_context: the get_state snapshot (actions/action_state.h): tabs,
// panes in screen order, up to 200 selected paths, preview, dialog.
ToolOutcome GetContextTool(HWND host, const ToolCall& call);
// pulse.reveal {paths[]}: opens the first item's folder (the client's reveal
// tab when it still exists, else a tab already showing it, else a new one)
// and selects the items in it; items in other folders come back as not_shown.
ToolOutcome RevealTool(HWND host, const ToolCall& call);

} // namespace pulse::app::ai
