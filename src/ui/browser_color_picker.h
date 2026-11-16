// browser_color_picker.h — Chrome DevTools-style colour picker for 界面底色.
//
// Layered Direct2D popup following ColorPickerPopup (State, Layout, Render,
// Proc, EditProc, PlaceEdits, Present, Px). Other ColorPickerPopup callers
// keep the QFluent replica unchanged.
#pragma once

#include "color_picker.h"

#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>

namespace pulse::ui {

// HSL used by the format row: h 0..360 (360 wraps to 0), s/l 0..100.
struct HslColor {
    int h = 0;
    int s = 0;
    int l = 0;
};

HslColor HslFromRgb(uint32_t rgb) noexcept;
uint32_t RgbFromHsl(int h, int s, int l) noexcept;

// Unit square of the saturation/value pad: x 0..1 = S, y 0..1 = 1-V.
struct SvNorm {
    float s = 0.0f;
    float v = 1.0f;
};

SvNorm SvFromSquare(float nx, float ny) noexcept;
void SquareFromSv(float s, float v, float& nx, float& ny) noexcept;
uint32_t RgbFromHueSv(int h, float s, float v) noexcept;

bool ParseRgbChannels(const wchar_t* r, const wchar_t* g, const wchar_t* b,
                      uint32_t& rgb_out) noexcept;
bool ParseHslChannels(const wchar_t* h, const wchar_t* s, const wchar_t* l,
                      uint32_t& rgb_out) noexcept;

inline constexpr uint32_t kBrowserPickerSwatches[] = {
    kSurfaceDefaultRgb,
    kSurfaceNeutralRgb,
    0xFAFAFAu, 0xF5F5F5u, 0xEEEEEEu, 0xE0E0E0u,
    0xF7F4F0u, 0xF3EDE4u, 0xE8DCC8u,
    0xF4F7FBu, 0xE8F0F8u, 0xD6E4F0u, 0xB8D0E8u,
    0xF2F7F2u, 0xE4F0E6u, 0xC8E0D0u,
    0xFBF4F6u, 0xF5E4EAu, 0xE8C8D4u,
    0xF6F3FBu, 0xEAE4F4u, 0xD0C4E4u,
    0xFBF8EEu, 0xF5EDD4u, 0xF0E6C8u, 0xE8F4F4u,
};
inline constexpr int kBrowserPickerSwatchCount =
    static_cast<int>(sizeof(kBrowserPickerSwatches) / sizeof(kBrowserPickerSwatches[0]));
inline constexpr int kBrowserPickerSwatchCols = 8;
inline constexpr int kBrowserPickerRowsCollapsed = 2;
inline constexpr int kBrowserPickerRowsExpanded = 4;

class BrowserColorPicker {
public:
    // Modal. Returns true when the user accepts (Enter or a click outside).
    // on_preview fires for every live colour (square, hue, fields, swatches,
    // eyedropper) so the owner can apply without writing prefs. Esc restores
    // the caller's previous colour by returning false.
    static bool Pick(HWND owner, Compositor* compositor, float scale, POINT screen_pt,
                     uint32_t initial_rgb, bool dark,
                     const std::function<void(uint32_t)>& on_preview, uint32_t& result_rgb);

    static bool SaveDebugSnapshot(Compositor* compositor, const wchar_t* png_path,
                                  bool dark, D2D1_COLOR_F accent, float scale);
};

} // namespace pulse::ui
