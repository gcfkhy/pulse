// column_strip_nav_probe.cpp — GUI probe for the column view's arrow buttons and
// wheel panning. Runs only in an explicitly isolated --shot test instance
// (PULSE_TEST_COLUMN_NAV_FLOW=<log file>) opened on a deep fixture folder with a
// long column (bench_data/column_nav_deep). It drives the real mouse handlers
// and writes offscreen snapshots next to the --shot output. The window must be
// tall enough (about 700 px) for the 4-row columns to need no scrolling.
// Fixture: bench_data/column_nav_deep/level01/.../level14, three note1-3.txt in
// every level and 160 file001-160.txt more in level03; open level14, e.g.
//   PULSE_TEST_DATA_DIR=<scratch> PULSE_TEST_COLUMN_NAV_FLOW=<log> pulse.exe
//   --test-instance --shot <png> --size 1500x900 [--dark] <...\level14>
// The first entry is selected before measuring. In level14 that is a file, so the
// current list is the last column; in level13 it is the level14 folder, so a child
// column opens to the right of the list. Run both.
#include "app_input.h"
#include "app_column_view.h"
#include "../common/localization.h"
#include "../ui/column_strip_layout.h"
#include "../ui/ui_renderer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

void Render(pulse::AppState& state);

namespace {
using namespace pulse;

constexpr ULONGLONG kChainPauseMs = 350;  // app_column_view.cpp kWheelChainPauseMs

struct Geometry {
    ui::WindowViewModel vm;
    D2D1_RECT_F pane_rect{};
    ui::ColumnStripLayout layout;
    ui::ColumnStripNavLayout nav;
    const ui::PaneViewModel& Pane() const { return vm.pane_slots.empty() ? vm.pane : vm.pane_slots.front().pane; }
};

class Probe {
public:
    Probe(AppState& state, FILE* out) : s(state), log(out) {}
    int Run();

private:
    AppState& s;
    FILE* log;
    int failures = 0;

    void Check(bool ok, const char* label) {
        std::fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label);
        std::fflush(log);
        failures += !ok;
    }
    void Note(const char* what, float a, float b = 0.0f) {
        std::fprintf(log, "[INFO] %s %.2f %.2f\n", what, a, b);
        std::fflush(log);
    }
    app::Tab* Tab() { return ActiveTab(s); }

    Geometry Geo() {
        Geometry g;
        g.vm = BuildVm(s, false);
        g.pane_rect = g.vm.pane_slots.empty()
            ? s.renderer.ContentRect(static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()))
            : g.vm.pane_slots.front().rect;
        g.layout = s.renderer.ColumnStripGeometry(g.Pane(), g.pane_rect);
        g.nav = ui::LayoutColumnStripNav(g.layout, s.scale);
        return g;
    }
    void Pump(int ms) {
        const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(ms);
        do {
            s.teachTip = -1;  // a first-run tip card would float over the arrows

            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Render(s);
            Sleep(16);
        } while (GetTickCount64() < end);
    }
    void Shot(const wchar_t* name) {
        Render(s);
        const std::wstring path = s.shot.output + L"." + name + L".png";
        Check(s.compositor.SaveSnapshot(path.c_str()), "snapshot saved");
    }
    float FromRight() { return Tab() ? Tab()->column_strip.scroll_from_right_dip : -1.0f; }
    void ToOldest() {
        const float scale = s.scale > 0.0f ? s.scale : 1.0f;
        Tab()->column_strip.scroll_from_right_dip = Geo().layout.max_scroll_px / scale;
        Pump(600);
    }
    static POINT Center(const D2D1_RECT_F& r) {
        return {static_cast<LONG>((r.left + r.right) * 0.5f), static_cast<LONG>((r.top + r.bottom) * 0.5f)};
    }
    void Hover(POINT p) {
        const Geometry g = Geo();
        const auto bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
                                        static_cast<float>(s.compositor.Height()));
        UpdatePointerHover(s, s.renderer.HitTest(g.vm, bounds, static_cast<float>(p.x), static_cast<float>(p.y)), p);
    }
    void Click(POINT p) {
        Hover(p);
        const LPARAM at = MAKELPARAM(p.x, p.y);
        HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at);
        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, at);
    }
    void Wheel(POINT client, int notches, UINT msg = WM_MOUSEWHEEL) {
        POINT screen = client;
        ClientToScreen(s.hwnd, &screen);
        Hover(client);
        HandleMouseWheel(&s, s.hwnd, msg, MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(notches * WHEEL_DELTA))),
                         MAKELPARAM(screen.x, screen.y));
    }
    // The ancestor column showing `leaf`, or -1.
    int AncestorIndex(const wchar_t* leaf) {
        const auto& ancestors = Tab()->column_strip.ancestors;
        for (size_t i = 0; i < ancestors.size(); ++i) {
            const std::wstring& path = ancestors[i].path;
            const std::wstring tail = std::wstring(L"\\") + leaf;
            if (path.size() >= tail.size() && path.compare(path.size() - tail.size(), tail.size(), tail) == 0)
                return static_cast<int>(i);
        }
        return -1;
    }
    // One arrow at each edge of the pane, whatever column is under it.
    bool NavAtEnds(const Geometry& g) {
        const float tol = 0.5f;
        const float inset = ui::kColumnStripNavInsetDip * (s.scale > 0.5f ? s.scale : 0.5f);
        bool ok = true;
        if (g.nav.left) ok = ok && std::fabs(g.nav.left_rect.left - (g.pane_rect.left + inset)) <= tol;
        if (g.nav.right) ok = ok && std::fabs(g.nav.right_rect.right - (g.pane_rect.right - inset)) <= tol;
        if (g.nav.left && g.nav.right) ok = ok && g.nav.left_rect.right <= g.nav.right_rect.left;
        return ok;
    }
    // x of the child column's left edge, or -1 when none is open.
    static float ChildLeft(const Geometry& g) {
        return !g.layout.columns.empty() && g.layout.columns.back().id == ui::kColumnStripChildId
            ? g.layout.columns.back().rect.left : -1.0f;
    }
    bool HasChildColumn(const Geometry& g) {
        return !g.layout.columns.empty() && g.layout.columns.back().id == ui::kColumnStripChildId;
    }
};

int Probe::Run() {
    app::Tab* tab = Tab();
    if (!s.isolatedTest || !tab || tab->current_path.find(L"column_nav_deep") == std::wstring::npos) {
        std::fprintf(log, "[ERROR] open the column_nav_deep fixture in a --test-instance\n");
        return 2;
    }
    ToggleColumnLayout(s, 0);
    bool loaded = false;
    for (int i = 0; i < 300 && !loaded; ++i) {
        Pump(50);
        tab = Tab();
        loaded = tab && tab->column_layout && !tab->column_strip.ancestors.empty();
        for (const auto& column : tab->column_strip.ancestors) loaded = loaded && column.snapshot;
    }
    Check(loaded, "column view opens with every ancestor listed");
    if (!loaded) return 2;
    const float scale = s.scale;
    const float unit = scale > 0.5f ? scale : 0.5f;
    const size_t count = tab->column_strip.ancestors.size();

    // The first entry is selected: a folder opens a child column beside the list
    // (start in level13), a file leaves the list as the last column (level14).
    tab->SelectOnly(0);
    Pump(500);

    Geometry g = Geo();
    Check(g.layout.active && g.layout.max_scroll_px > 0.0f, "deep folders overflow the strip");
    Note("window px", static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    Note("viewport px", g.layout.viewport.right - g.layout.viewport.left, g.layout.max_scroll_px);
    Note("child column (1 = yes)", HasChildColumn(g) ? 1.0f : 0.0f);
    // The arrows are at the pane's two edges, so the pane must hold both.
    const bool arrows_fit = g.layout.viewport.right - g.layout.viewport.left >=
        (2.0f * ui::kColumnStripNavDiameterDip + 3.0f * ui::kColumnStripNavInsetDip) * unit;
    Check(arrows_fit == (g.nav.left || g.nav.right), "arrows appear exactly when the strip overflows a pane that holds both");
    Check(FromRight() == 0.0f && g.layout.scroll_px >= g.layout.max_scroll_px - 0.5f,
          "strip rests at its right end");
    Check(NavAtEnds(g), "left arrow at the pane's left edge, right arrow at its right edge");
    const float last_right = HasChildColumn(g) ? g.layout.columns.back().rect.right : g.layout.body.right;
    Check(std::fabs(last_right - g.pane_rect.right) < 1.0f, "at rest the last column ends at the pane's right edge");
    const float rest_scroll = g.layout.scroll_px, rest_body_left = g.layout.body.left;
    const float rest_child_left = ChildLeft(g);
    Shot(L"01_rest");

    if (arrows_fit) {
        Check(g.nav.left && !g.nav.right, "at rest only the arrow toward older ancestors shows");
        // Hover plate and tooltip on the left arrow.
        const POINT left_at = Center(g.nav.left_rect);
        Hover(left_at);
        Pump(120);
        Check(s.hoverRegion == static_cast<int>(ui::HitTestResult::ColumnStripNav) &&
                  s.hoverControlIndex == ui::kColumnStripNavLeft, "pointer over the left arrow hits it");
        const std::wstring tip = TooltipForHover(s);
        Check(tip.find(l10n::Get(l10n::StringId::ColumnStripScrollLeft)) == 0 &&
                  tip.find(l10n::Get(l10n::StringId::ColumnStripWheelHint)) != std::wstring::npos,
              "tooltip names the arrow and mentions the wheel");
        Shot(L"02_hover_left");

        // One click = one column, gliding there.
        const float before = g.layout.scroll_px;
        Click(left_at);
        // Frames right after the click: the strip must still be on its way.
        std::wstring first_frames[4];
        for (int i = 0; i < 4; ++i) {
            Render(s);
            first_frames[i] = s.shot.output + L".03_glide_" + std::to_wstring(i) + L".png";
            s.compositor.SaveSnapshot(first_frames[i].c_str());
            Sleep(35);
        }
        Pump(500);
        g = Geo();
        const float moved = before - g.layout.scroll_px;
        Note("left click moved px", moved, 240.0f * unit);
        Check(moved >= 32.0f * unit && moved <= 480.0f * unit, "a click moves the strip by about one column");
        Check(g.nav.left && g.nav.right && NavAtEnds(g), "after one step both arrows show");
        // One strip: the list and the child column move by what the ancestors did.
        const float strip_moved = rest_scroll - g.layout.scroll_px;
        Check(std::fabs((g.layout.body.left - rest_body_left) - strip_moved) < 1.0f,
              "the current folder's list moves with the strip");
        if (rest_child_left >= 0.0f) {
            Check(std::fabs((ChildLeft(g) - rest_child_left) - strip_moved) < 1.0f,
                  "the child column moves with the strip");
        }
        Shot(L"04_after_left_click");

        // Keep clicking to the oldest ancestor: the left arrow goes away at the end.
        int clicks = 1;
        while (g.nav.left && clicks < static_cast<int>(count) + 3) {
            Click(Center(g.nav.left_rect));
            Pump(60);
            g = Geo();
            ++clicks;
        }
        Pump(500);
        Note("clicks to the oldest ancestor", static_cast<float>(clicks), static_cast<float>(count));
        Check(clicks <= static_cast<int>(count) + 1, "no more clicks than columns reach the oldest ancestor");
        Check(g.layout.scroll_px <= 0.5f && !g.nav.left && g.nav.right, "at the oldest ancestor the left arrow is gone");
        Check(ui::LayoutColumnStripNav(g.layout, s.scale).left == false, "left arrow stays hidden at the end");
        Check(!(ui::HitTestResult::ColumnStripNav == s.renderer.HitTest(g.vm,
                  D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height())),
                  g.nav.left_rect.left + 1.0f, (g.nav.left_rect.top + g.nav.left_rect.bottom) * 0.5f).region),
              "a hidden arrow cannot be clicked");
        Shot(L"05_oldest");

        // Back to the parent with the right arrow.
        clicks = 0;
        while (g.nav.right && clicks < static_cast<int>(count) + 3) {
            Click(Center(g.nav.right_rect));
            Pump(60);
            g = Geo();
            ++clicks;
        }
        Pump(500);
        Check(FromRight() == 0.0f && g.layout.scroll_px >= g.layout.max_scroll_px - 0.5f && !g.nav.right && g.nav.left,
              "clicking right returns beside the parent and the right arrow goes away");
        Shot(L"06_back_at_parent");
    }

    // Wheel over a column with nothing to scroll: the strip pans, 96 dip a notch.
    g = Geo();
    int parent_id = static_cast<int>(count) - 1;
    POINT over_parent{};
    for (const auto& slot : g.layout.columns)
        if (slot.id == parent_id) over_parent = POINT{
            static_cast<LONG>((std::max)(slot.rect.left, g.layout.viewport.left) + 60.0f * unit),
            static_cast<LONG>(slot.rect.top + (slot.rect.bottom - slot.rect.top) * 0.4f)};
    Check(over_parent.x != 0, "the parent column is on screen");
    Wheel(over_parent, 1);
    Check(std::fabs(FromRight() - 96.0f) < 0.5f, "wheel up over a short column pans toward older ancestors");
    Wheel(over_parent, -3);
    Check(FromRight() == 0.0f, "wheel down pans back and stops at the parent end");
    Wheel(over_parent, 2);
    Pump(500);
    Shot(L"07_wheel_pan");

    // A long column scrolls its rows first; the strip only moves after a pause at its end.
    const int long_id = AncestorIndex(L"level03");
    Check(long_id >= 0, "fixture has the long level03 column");
    if (long_id >= 0) {
        ToOldest();  // the long column is among the oldest ancestors
        g = Geo();
        {   // A narrow strip shows less than two columns: put the long one's left edge at the viewport's.
            const float scale_now = s.scale > 0.0f ? s.scale : 1.0f;
            const float edge_px = static_cast<float>(long_id) * ui::kColumnStripDefaultDip * unit;
            Tab()->column_strip.scroll_from_right_dip = (g.layout.max_scroll_px - edge_px) / scale_now;
            Pump(600);
            g = Geo();
        }
        POINT over_long{};
        for (const auto& slot : g.layout.columns)
            if (slot.id == long_id) over_long = POINT{
                static_cast<LONG>((std::max)(slot.rect.left, g.layout.viewport.left) + 60.0f * unit),
                static_cast<LONG>(slot.rect.top + (slot.rect.bottom - slot.rect.top) * 0.4f)};
        Check(over_long.x != 0, "the long column is on screen");
        const float pan_before = FromRight();
        auto& column = tab->column_strip.ancestors[static_cast<size_t>(long_id)];
        column.auto_scroll = false;
        column.scroll_dip = 0.0f;
        Wheel(over_long, -3);
        Check(column.scroll_dip > 0.0f && std::fabs(FromRight() - pan_before) < 0.01f,
              "wheel down over a long column scrolls its rows, not the strip");
        for (int i = 0; i < 3; ++i) Wheel(over_long, 1);
        Check(column.scroll_dip <= 0.5f && std::fabs(FromRight() - pan_before) < 0.01f,
              "wheel up scrolls the rows back to the top first");
        Wheel(over_long, 1);  // still within the pause after the last row scroll: a spin's tail
        Check(std::fabs(FromRight() - pan_before) < 0.01f, "the tail of a spin at the end of a column does not pan");
        Sleep(static_cast<DWORD>(kChainPauseMs) + 100);
        for (int i = 0; i < 90; ++i) { Wheel(over_long, -1); Sleep(5); }
        Check(column.scroll_dip > 0.0f && std::fabs(FromRight() - pan_before) < 0.01f,
              "a long spin down to the bottom never pans the strip");
        Sleep(static_cast<DWORD>(kChainPauseMs) + 100);
        Wheel(over_long, -1);
        Check(std::fabs(FromRight() - (pan_before - 96.0f)) < 0.5f,
              "after a pause the wheel at the bottom pans the strip toward the parent");
        Pump(500);
        Shot(L"08_long_column");
    }

    // The current folder's list (nothing to scroll there) hands the wheel to the strip too.
    // Scrolled a little from rest, the list is still partly in the pane.
    constexpr float kStartDip = 150.0f;
    Tab()->column_strip.scroll_from_right_dip = kStartDip;
    Pump(600);
    g = Geo();
    Check(std::fabs(FromRight() - kStartDip) < 0.5f && g.layout.body.left > rest_body_left + 1.0f,
          "scrolled from rest the list has moved right with the strip");
    const float body_x = (std::min)(g.layout.body.left + 30.0f * unit, g.pane_rect.right - 20.0f * unit);
    const LONG row_y = static_cast<LONG>(g.layout.viewport.top + (g.layout.viewport.bottom - g.layout.viewport.top) * 0.4f);
    const POINT over_body{static_cast<LONG>(body_x), row_y};
    // Sideways wheel (tilt / touchpad): right looks further right, toward the list at the right end.
    Wheel(over_body, 1, WM_MOUSEHWHEEL);
    Check(std::fabs(FromRight() - (kStartDip - 96.0f)) < 0.5f, "tilting right pans toward the right end");
    const POINT over_ancestors{static_cast<LONG>((g.pane_rect.left + g.pane_rect.right) * 0.5f), row_y};
    Wheel(over_ancestors, -1, WM_MOUSEHWHEEL);
    Check(std::fabs(FromRight() - kStartDip) < 0.5f, "tilting left over the columns pans back");
    Wheel(over_body, -1);
    Check(std::fabs(FromRight() - (kStartDip - 96.0f)) < 0.5f, "wheel over the current list pans the strip");
    // Wheel over the arrows themselves.
    g = Geo();
    if (g.nav.left) {
        const float before_arrow = FromRight();
        Wheel(Center(g.nav.left_rect), 1);
        Check(FromRight() > before_arrow, "wheel over an arrow pans too");
    }
    Pump(500);
    g = Geo();
    Check(NavAtEnds(g), "arrows still at the ends of the area at the end");
    // A selection that moves brings the list back into view.
    Tab()->column_strip.scroll_from_right_dip = kStartDip;
    Pump(400);
    Check(std::fabs(FromRight() - kStartDip) < 0.5f, "strip scrolled away from the list again");
    Tab()->SelectOnly(1);
    Pump(600);
    Check(FromRight() == 0.0f, "moving the selection scrolls the strip back to the list");
    Shot(L"09_final");
    return failures ? 1 : 0;
}
} // namespace

// Explicit isolated --shot runs only.
int RunColumnStripNavProbe(pulse::AppState& state, const wchar_t* output) {
    FILE* log = nullptr;
    if (_wfopen_s(&log, output, L"w") || !log) return 2;
    Probe probe(state, log);
    const int result = probe.Run();
    std::fclose(log);
    return result;
}
