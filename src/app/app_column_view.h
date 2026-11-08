// app_column_view.h — Column (Miller) view: listing sync and input handling.
#pragma once
#include "app_runtime.h"

namespace pulse {

// Keeps every visible pane's column listings in step with its folder and
// selection. Cheap when nothing changed; starts async loads otherwise.
void SyncColumnStrips(AppState& s);
// Called for every worker result before pane handling.
void NoteColumnStripResult(AppState& s, const app::WorkResult& res);

// Mouse-down on column-view regions (toggle button, rows, dividers).
// Returns true when consumed.
bool HandleColumnStripMouseDown(AppState& s, const ui::WindowViewModel& vm,
                                const ui::HitTestResult& hit, int x);
bool UpdateColumnStripResize(AppState& s, int x);
// Double-click on a divider restores that column's default width.
void ResetColumnStripWidth(AppState& s, const ui::HitTestResult& hit);
void EndColumnStripResize(AppState& s);
// Double-click right after a column click changed the folder: swallow it so
// the second click does not open whatever moved under the cursor.
bool ColumnStripSwallowDoubleClick(const AppState& s);
// Right-click on a column row: selects that item the way a click does (the
// list moves to its folder). Returns true when the item is now selected.
bool SelectColumnStripRow(AppState& s, const ui::HitTestResult& hit);
// Drag over a column: a drop lands in the folder row under the pointer, or
// else in that column's folder. *folder_row is the row (or -1). False when
// the hit is not on a column, so the caller keeps its own target.
bool ColumnStripDropFolder(AppState& s, const ui::HitTestResult& hit, std::wstring& folder,
                           int* folder_row);
// Wheel over a column: scrolls its rows; where it has none to scroll that way
// (or with Shift, over the arrows or the scrollbar) it pans the strip of ancestor columns.
bool HandleColumnStripWheel(AppState& s, const ui::WindowViewModel& vm,
                            const ui::HitTestResult& hit, int wheel_delta);
// Wheel over the current folder's list that has nothing to scroll: pans the
// strip instead. (x, y) is the client point under the pointer.
bool HandleColumnStripBodyWheel(AppState& s, const ui::WindowViewModel& vm,
                                const ui::HitTestResult& hit, int x, int y, int wheel_delta);
// Sideways wheel (tilt wheel, touchpad swipe) over the column view pans the strip.
bool HandleColumnStripHWheel(AppState& s, const ui::WindowViewModel& vm,
                             const ui::HitTestResult& hit, int x, int y, int wheel_delta);
// Left/Right in the column view: go to the parent / enter the selected folder.
bool HandleColumnArrowKey(AppState& s, bool right);
void ToggleColumnLayout(AppState& s, int pane_index);
// OLE drag: the list or column under `pt` that can still scroll, or false.
bool LocateDropScrollBand(AppState& s, const ui::WindowViewModel& vm,
                          const ui::HitTestResult& hit, POINT pt,
                          app::Tab*& tab, int& column_id, D2D1_RECT_F& list, float& max_px);
bool ApplyDropScroll(AppState& s, app::Tab& tab, int column_id, int pane_index,
                     const ui::WindowViewModel& vm, float delta_px);

} // namespace pulse
