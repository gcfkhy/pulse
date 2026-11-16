#include "edit_host.h"
#include "FluentTokens.h"
// browser_color_picker.cpp — Chrome DevTools-style colour picker.
//
// Layered WS_EX_LAYERED popup on the main compositor (ColorPickerPopup
// surface pattern): SV square, eyedropper + preview + hue slider, hex/RGB/HSL
// native EDIT overlays, swatch grid. No alpha slider: 界面底色 is opaque.
#include "browser_color_picker.h"

#include "command_icons.h"
#include "fluent_components.h"
#include "fluent_menu.h"
#include "ui_compositor.h"
#include "lumatext_renderer.h"
#include "typography.h"
#include "../common/localization.h"

#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <d2d1effects.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "uxtheme.lib")

namespace pulse::ui {

namespace {

int ClampI(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float ClampF(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

HslColor HslFromRgb(uint32_t rgb) noexcept {
    const float r = static_cast<float>((rgb >> 16) & 0xff) / 255.0f;
    const float g = static_cast<float>((rgb >> 8) & 0xff) / 255.0f;
    const float b = static_cast<float>(rgb & 0xff) / 255.0f;
    const float mx = (std::max)({r, g, b});
    const float mn = (std::min)({r, g, b});
    const float d = mx - mn;
    const float l = (mx + mn) * 0.5f;
    HslColor out;
    out.l = ClampI(static_cast<int>(std::lround(l * 100.0f)), 0, 100);
    if (d < 1.0e-6f) return out;
    const float denom = 1.0f - std::fabs(2.0f * l - 1.0f);
    const float s = denom < 1.0e-6f ? 0.0f : d / denom;
    out.s = ClampI(static_cast<int>(std::lround(s * 100.0f)), 0, 100);
    float h = 0.0f;
    if (mx == r) h = 60.0f * std::fmod((g - b) / d + 6.0f, 6.0f);
    else if (mx == g) h = 60.0f * ((b - r) / d + 2.0f);
    else h = 60.0f * ((r - g) / d + 4.0f);
    if (h < 0.0f) h += 360.0f;
    out.h = ClampI(static_cast<int>(std::lround(h)), 0, 360);
    if (out.h == 360) out.h = 0;
    return out;
}

uint32_t RgbFromHsl(int h, int s, int l) noexcept {
    h = ((h % 360) + 360) % 360;
    const float hf = static_cast<float>(h) / 60.0f;
    const float sf = static_cast<float>(ClampI(s, 0, 100)) / 100.0f;
    const float lf = static_cast<float>(ClampI(l, 0, 100)) / 100.0f;
    const float c = (1.0f - std::fabs(2.0f * lf - 1.0f)) * sf;
    const float x = c * (1.0f - std::fabs(std::fmod(hf, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    if (hf < 1) { r = c; g = x; }
    else if (hf < 2) { r = x; g = c; }
    else if (hf < 3) { g = c; b = x; }
    else if (hf < 4) { g = x; b = c; }
    else if (hf < 5) { r = x; b = c; }
    else { r = c; b = x; }
    const float m = lf - c * 0.5f;
    auto channel = [&](float v) {
        return static_cast<uint32_t>(std::lround(ClampF(v + m, 0.0f, 1.0f) * 255.0f));
    };
    return (channel(r) << 16) | (channel(g) << 8) | channel(b);
}

SvNorm SvFromSquare(float nx, float ny) noexcept {
    nx = ClampF(nx, 0.0f, 1.0f);
    ny = ClampF(ny, 0.0f, 1.0f);
    return {nx, 1.0f - ny};
}

void SquareFromSv(float s, float v, float& nx, float& ny) noexcept {
    nx = ClampF(s, 0.0f, 1.0f);
    ny = ClampF(1.0f - v, 0.0f, 1.0f);
}

uint32_t RgbFromHueSv(int h, float s, float v) noexcept {
    const int si = ClampI(static_cast<int>(std::lround(ClampF(s, 0.0f, 1.0f) * 255.0f)), 0, 255);
    const int vi = ClampI(static_cast<int>(std::lround(ClampF(v, 0.0f, 1.0f) * 255.0f)), 0, 255);
    return RgbFromHsv(h, si, vi);
}

namespace {

bool ParseIntStrict(const wchar_t* text, int lo, int hi, int& out) noexcept {
    if (!text || !*text) return false;
    if (*text == L'+' || *text == L'-') return false;
    wchar_t* end = nullptr;
    const long v = wcstol(text, &end, 10);
    if (end == text || *end != 0) return false;
    if (v < lo || v > hi) return false;
    out = static_cast<int>(v);
    return true;
}

} // namespace

bool ParseRgbChannels(const wchar_t* r, const wchar_t* g, const wchar_t* b,
                      uint32_t& rgb_out) noexcept {
    int rv = 0, gv = 0, bv = 0;
    if (!ParseIntStrict(r, 0, 255, rv) || !ParseIntStrict(g, 0, 255, gv) ||
        !ParseIntStrict(b, 0, 255, bv)) {
        return false;
    }
    rgb_out = (static_cast<uint32_t>(rv) << 16) | (static_cast<uint32_t>(gv) << 8) |
              static_cast<uint32_t>(bv);
    return true;
}

bool ParseHslChannels(const wchar_t* h, const wchar_t* s, const wchar_t* l,
                      uint32_t& rgb_out) noexcept {
    int hv = 0, sv = 0, lv = 0;
    if (!ParseIntStrict(h, 0, 360, hv) || !ParseIntStrict(s, 0, 100, sv) ||
        !ParseIntStrict(l, 0, 100, lv)) {
        return false;
    }
    rgb_out = RgbFromHsl(hv, sv, lv);
    return true;
}

namespace {

constexpr GUID kShadowEffectClsid = {0xC67EA361, 0x1863, 0x4e69,
    {0x89, 0xDB, 0x69, 0x5D, 0x3E, 0x9D, 0x5B, 0x53}};

constexpr wchar_t kClassName[] = L"PulseBrowserColorPicker";

constexpr float kCardW = 300.0f;
constexpr float kMargin = 16.0f;
constexpr float kInnerW = kCardW - kMargin * 2.0f;
constexpr float kTop = 16.0f;
constexpr float kSvH = 148.0f;
constexpr float kGap = 12.0f;
constexpr float kToolH = 28.0f;
constexpr float kEyedrop = 28.0f;
constexpr float kPreview = 24.0f;
constexpr float kHueGrooveH = 12.0f;
constexpr float kInputH = 28.0f;
constexpr float kLabelH = 16.0f;
constexpr float kSepGap = 10.0f;
constexpr float kSepH = 1.0f;
constexpr float kSwatchD = 20.0f;
constexpr float kSwatchRowGap = 10.0f;
constexpr float kExpandW = 24.0f;
constexpr float kBottom = 14.0f;
constexpr int kShadow = FluentMenu::kShadowMargin;

constexpr int kPartNone = 0;
constexpr int kPartSv = 1;
constexpr int kPartHue = 2;
constexpr int kPartEyedrop = 3;
constexpr int kPartCycle = 4;
constexpr int kPartExpand = 5;
constexpr int kPartSwatch0 = 100;

enum class TextFormatKind { Hex = 0, Rgb = 1, Hsl = 2 };

struct EditHook {
    struct State* s = nullptr;
    int index = 0; // 0 hex, 1..3 channels
};

struct State {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    Compositor* compositor = nullptr;
    float scale = 1.0f;
    bool dark = true;
    D2D1_COLOR_F accent{};
    Theme theme{};
    fluent::Painter painter;
    std::function<void(uint32_t)> on_preview;

    uint32_t rgb = 0xFFFFFFu;
    HsvColor hsv{};
    TextFormatKind format = TextFormatKind::Hex;
    bool palette_open = false;
    bool syncing = false;
    bool done = false;
    bool accepted = false;
    bool eyedrop = false;
    uint32_t eyedrop_restore = 0;
    uint32_t last_preview = 0xFFFFFFFFu;

    int base_x = 0;
    int base_y = 0;
    int present_dx = 0;
    int win_w = 0;
    int win_h = 0;

    D2D1_RECT_F r_sv{};
    D2D1_RECT_F r_eyedrop{};
    D2D1_RECT_F r_preview{};
    D2D1_RECT_F r_hue{};
    D2D1_RECT_F r_hex{};
    D2D1_RECT_F r_ch[3]{};
    D2D1_RECT_F r_hex_label{};
    D2D1_RECT_F r_ch_label[3]{};
    D2D1_RECT_F r_cycle{};
    D2D1_RECT_F r_sep{};
    D2D1_RECT_F r_swatch[kBrowserPickerSwatchCount]{};
    D2D1_RECT_F r_expand{};
    int swatch_vis = kBrowserPickerSwatchCols * kBrowserPickerRowsCollapsed;

    HWND edits[4]{};
    std::array<EditHook, 4> hooks{};
    HFONT font = nullptr;
    HBRUSH edit_brush = nullptr;
    int line_h = 16;
    int focus_edit = -1;
    bool edits_shown = false;

    int hot = kPartNone;
    int drag = kPartNone;
    bool tracking_leave = false;

    HDC mem_dc = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    int dib_w = 0;
    int dib_h = 0;
};

int Red(uint32_t rgb) { return static_cast<int>((rgb >> 16) & 0xff); }
int Green(uint32_t rgb) { return static_cast<int>((rgb >> 8) & 0xff); }
int Blue(uint32_t rgb) { return static_cast<int>(rgb & 0xff); }

float Px(const State& s, float dip) { return dip * s.scale; }

bool Contains(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

COLORREF ColorRefFrom(D2D1_COLOR_F c) {
    return RGB(static_cast<BYTE>(std::lround(ClampF(c.r, 0.0f, 1.0f) * 255.0f)),
               static_cast<BYTE>(std::lround(ClampF(c.g, 0.0f, 1.0f) * 255.0f)),
               static_cast<BYTE>(std::lround(ClampF(c.b, 0.0f, 1.0f) * 255.0f)));
}

D2D1_COLOR_F FieldFill(const State& s) {
    const D2D1_COLOR_F plate = s.dark ? HexColor(0x2B2B2B) : HexColor(0xF9F9F9);
    return BlendOver(s.theme.fill_input, plate);
}

float CardHeightDip(bool palette_open) {
    const int rows = palette_open ? kBrowserPickerRowsExpanded : kBrowserPickerRowsCollapsed;
    const float swatches = static_cast<float>(rows) * kSwatchD +
                           static_cast<float>(rows - 1) * kSwatchRowGap;
    return kTop + kSvH + kGap + kToolH + kGap + kInputH + kLabelH + kSepGap + kSepH +
           kSepGap + swatches + kBottom;
}

void Layout(State& s) {
    const float m = static_cast<float>(kShadow);
    auto rect = [&](float l, float t, float r, float b) {
        return D2D1::RectF(m + Px(s, l), m + Px(s, t), m + Px(s, r), m + Px(s, b));
    };
    const float x0 = kMargin;
    const float x1 = kMargin + kInnerW;
    float y = kTop;
    s.r_sv = rect(x0, y, x1, y + kSvH);
    y += kSvH + kGap;

    const float tool_mid = y + kToolH * 0.5f;
    s.r_eyedrop = rect(x0, y, x0 + kEyedrop, y + kToolH);
    const float prev_l = x0 + kEyedrop + 8.0f;
    s.r_preview = rect(prev_l, tool_mid - kPreview * 0.5f, prev_l + kPreview,
                       tool_mid + kPreview * 0.5f);
    const float hue_l = prev_l + kPreview + 10.0f;
    s.r_hue = rect(hue_l, tool_mid - kHueGrooveH * 0.5f, x1, tool_mid + kHueGrooveH * 0.5f);
    y += kToolH + kGap;

    const float cycle_l = x1 - kEyedrop;
    s.r_cycle = rect(cycle_l, y, x1, y + kInputH);
    const float fields_r = cycle_l - 8.0f;
    s.r_hex = rect(x0, y, fields_r, y + kInputH);
    const float ch_w = (fields_r - x0 - 8.0f * 2.0f) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const float cl = x0 + static_cast<float>(i) * (ch_w + 8.0f);
        s.r_ch[i] = rect(cl, y, cl + ch_w, y + kInputH);
        s.r_ch_label[i] = rect(cl, y + kInputH, cl + ch_w, y + kInputH + kLabelH);
    }
    s.r_hex_label = rect(x0, y + kInputH, fields_r, y + kInputH + kLabelH);
    y += kInputH + kLabelH + kSepGap;
    s.r_sep = rect(x0, y, x1, y + kSepH);
    y += kSepH + kSepGap;

    const int rows = s.palette_open ? kBrowserPickerRowsExpanded : kBrowserPickerRowsCollapsed;
    s.swatch_vis = (std::min)(kBrowserPickerSwatchCount, rows * kBrowserPickerSwatchCols);
    const float grid_r = x1 - kExpandW - 8.0f;
    const float cell = (grid_r - x0) / static_cast<float>(kBrowserPickerSwatchCols);
    for (int i = 0; i < kBrowserPickerSwatchCount; ++i) {
        const int col = i % kBrowserPickerSwatchCols;
        const int row = i / kBrowserPickerSwatchCols;
        const float cx = x0 + (static_cast<float>(col) + 0.5f) * cell;
        const float cy = y + static_cast<float>(row) * (kSwatchD + kSwatchRowGap) + kSwatchD * 0.5f;
        s.r_swatch[i] = rect(cx - kSwatchD * 0.5f, cy - kSwatchD * 0.5f,
                             cx + kSwatchD * 0.5f, cy + kSwatchD * 0.5f);
    }
    const float expand_top = y;
    const float expand_h = static_cast<float>(rows) * kSwatchD +
                           static_cast<float>(rows - 1) * kSwatchRowGap;
    s.r_expand = rect(x1 - kExpandW, expand_top, x1, expand_top + expand_h);

    const float card_h = CardHeightDip(s.palette_open);
    s.win_w = static_cast<int>(std::lround(Px(s, kCardW))) + kShadow * 2;
    s.win_h = static_cast<int>(std::lround(Px(s, card_h))) + kShadow * 2;
}

void SetRgb(State& s, uint32_t rgb, bool keep_hue_if_gray) {
    rgb &= 0x00FFFFFFu;
    s.rgb = rgb;
    const HsvColor hsv = HsvFromRgb(rgb);
    if (keep_hue_if_gray && hsv.s == 0) {
        s.hsv.s = 0;
        s.hsv.v = hsv.v;
    } else {
        s.hsv = hsv;
    }
}

void NotifyPreview(State& s) {
    if (s.rgb == s.last_preview) return;
    s.last_preview = s.rgb;
    if (s.on_preview) s.on_preview(s.rgb);
}

int HitTest(const State& s, float x, float y) {
    if (Contains(s.r_sv, x, y)) return kPartSv;
    if (Contains(s.r_hue, x, y)) return kPartHue;
    if (Contains(s.r_eyedrop, x, y)) return kPartEyedrop;
    if (Contains(s.r_cycle, x, y)) return kPartCycle;
    if (Contains(s.r_expand, x, y)) return kPartExpand;
    for (int i = 0; i < s.swatch_vis; ++i) {
        const float cx = (s.r_swatch[i].left + s.r_swatch[i].right) * 0.5f;
        const float cy = (s.r_swatch[i].top + s.r_swatch[i].bottom) * 0.5f;
        const float r = (s.r_swatch[i].right - s.r_swatch[i].left) * 0.5f;
        const float dx = x - cx, dy = y - cy;
        if (dx * dx + dy * dy <= r * r) return kPartSwatch0 + i;
    }
    return kPartNone;
}

void DrawEyedropperIcon(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush,
                        const D2D1_RECT_F& rc) {
    const float side = (std::min)(rc.right - rc.left, rc.bottom - rc.top);
    const float cx = (rc.left + rc.right) * 0.5f;
    const float cy = (rc.top + rc.bottom) * 0.5f;
    D2D1_MATRIX_3X2_F prev{};
    dc->GetTransform(&prev);
    dc->SetTransform(D2D1::Matrix3x2F::Rotation(-45.0f, D2D1::Point2F(cx, cy)) * prev);
    const float w = side * 0.10f;
    const D2D1_RECT_F body = D2D1::RectF(cx - w, cy - side * 0.28f, cx + w, cy + side * 0.16f);
    dc->FillRoundedRectangle(D2D1::RoundedRect(body, w, w), brush);
    dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + side * 0.26f), side * 0.13f, side * 0.13f),
                    brush, (std::max)(1.0f, side * 0.055f));
    const D2D1_POINT_2F tip[3] = {
        D2D1::Point2F(cx, cy - side * 0.40f),
        D2D1::Point2F(cx - w * 1.15f, cy - side * 0.22f),
        D2D1::Point2F(cx + w * 1.15f, cy - side * 0.22f),
    };
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    if (factory.get()) {
        ComPtr<ID2D1PathGeometry> path;
        if (SUCCEEDED(factory->CreatePathGeometry(&path))) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(path->Open(&sink))) {
                sink->BeginFigure(tip[0], D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(tip[1]);
                sink->AddLine(tip[2]);
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                dc->FillGeometry(path.get(), brush);
            }
        }
    }
    dc->SetTransform(prev);
}

void FillLinearRound(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float radius,
                     D2D1_POINT_2F a, D2D1_POINT_2F b, const D2D1_GRADIENT_STOP* stops, int n) {
    ComPtr<ID2D1GradientStopCollection> coll;
    if (FAILED(dc->CreateGradientStopCollection(stops, static_cast<UINT32>(n),
                                                D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &coll))) {
        return;
    }
    ComPtr<ID2D1LinearGradientBrush> brush;
    if (FAILED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b),
                                             coll.get(), &brush))) {
        return;
    }
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.get());
}

void DrawInputField(State& s, ID2D1DeviceContext* dc, const D2D1_RECT_F& rc, bool focused) {
    const float radius = Px(s, 4.0f);
    s.painter.FillRoundedRect(rc, radius, FieldFill(s));
    const D2D1_COLOR_F border = focused ? s.accent : s.theme.stroke_card;
    s.painter.StrokeRoundedRect(rc, radius, border, focused ? Px(s, 2.0f) : Px(s, 1.0f));
    (void)dc;
}

void DrawCommand(State& s, ID2D1DeviceContext* dc, command_icons::Icon icon,
                 const D2D1_RECT_F& rc, const D2D1_COLOR_F& color) {
    ComPtr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(color, &brush);
    ID2D1StrokeStyle* stroke = nullptr;
    command_icons::CreateStrokeStyle(dc, &stroke);
    command_icons::Draw(dc, brush.get(), stroke, icon,
                        command_icons::CenteredBounds(rc, Px(s, 16.0f)));
    if (stroke) stroke->Release();
}

bool Render(State& s) {
    ID2D1DeviceContext* dc = s.compositor ? s.compositor->Dc() : nullptr;
    if (!dc || s.win_w <= 0 || s.win_h <= 0) return false;
    const D2D1_SIZE_U size{static_cast<UINT32>(s.win_w), static_cast<UINT32>(s.win_h)};
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> bmp;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &bmp))) return false;

    ComPtr<ID2D1Image> old_target;
    dc->GetTarget(&old_target);
    dc->SetTarget(bmp.get());
    const D2D1_TEXT_ANTIALIAS_MODE old_text_aa = dc->GetTextAntialiasMode();
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    const float card_h = CardHeightDip(s.palette_open);
    const auto card = D2D1::RectF(static_cast<float>(kShadow), static_cast<float>(kShadow),
                                  static_cast<float>(kShadow) + Px(s, kCardW),
                                  static_cast<float>(kShadow) + Px(s, card_h));
    const float radius = Px(s, s.theme.radius_flyout);

    ComPtr<ID2D1CommandList> card_list;
    if (SUCCEEDED(dc->CreateCommandList(&card_list))) {
        dc->SetTarget(card_list.get());
        ComPtr<ID2D1SolidColorBrush> black;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1.0f), &black);
        dc->FillRoundedRectangle(D2D1::RoundedRect(card, radius, radius), black.get());
        card_list->Close();
        dc->SetTarget(bmp.get());
        ComPtr<ID2D1Effect> shadow;
        if (SUCCEEDED(dc->CreateEffect(kShadowEffectClsid, &shadow))) {
            shadow->SetInput(0, card_list.get());
            shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 7.0f);
            shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0.0f, 0.0f, 0.0f, 0.36f));
            dc->DrawImage(shadow.get(), D2D1::Point2F(0.0f, 4.0f * s.scale));
        }
    }

    if (!s.painter.BeginFrame(s.theme, IsHighContrast())) {
        dc->EndDraw();
        dc->SetTextAntialiasMode(old_text_aa);
        dc->SetTarget(old_target.get());
        return false;
    }
    s.painter.DrawMenuSurface(card);

    const float sv_r = Px(s, 8.0f);
    const uint32_t hue_rgb = RgbFromHsv(s.hsv.h, 255, 255);
    const D2D1_COLOR_F hue_c = HexColor(hue_rgb);
    {
        ComPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        ComPtr<ID2D1RoundedRectangleGeometry> geo;
        bool clipped = false;
        if (factory.get() &&
            SUCCEEDED(factory->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(s.r_sv, sv_r, sv_r), &geo))) {
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geo.get(),
                                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                          nullptr);
            clipped = true;
        }
        D2D1_GRADIENT_STOP hor[2] = {
            D2D1::GradientStop(0.0f, D2D1::ColorF(1, 1, 1, 1)),
            D2D1::GradientStop(1.0f, hue_c),
        };
        FillLinearRound(dc, s.r_sv, sv_r,
                        D2D1::Point2F(s.r_sv.left, s.r_sv.top),
                        D2D1::Point2F(s.r_sv.right, s.r_sv.top), hor, 2);
        D2D1_GRADIENT_STOP ver[2] = {
            D2D1::GradientStop(0.0f, D2D1::ColorF(0, 0, 0, 0)),
            D2D1::GradientStop(1.0f, D2D1::ColorF(0, 0, 0, 1)),
        };
        FillLinearRound(dc, s.r_sv, sv_r,
                        D2D1::Point2F(s.r_sv.left, s.r_sv.top),
                        D2D1::Point2F(s.r_sv.left, s.r_sv.bottom), ver, 2);
        if (clipped) dc->PopLayer();
        s.painter.StrokeRoundedRect(s.r_sv, sv_r, D2D1::ColorF(0, 0, 0, 0.12f), Px(s, 1.0f));
    }
    {
        float nx = 0.0f, ny = 0.0f;
        SquareFromSv(s.hsv.s / 255.0f, s.hsv.v / 255.0f, nx, ny);
        const float cx = s.r_sv.left + nx * (s.r_sv.right - s.r_sv.left);
        const float cy = s.r_sv.top + ny * (s.r_sv.bottom - s.r_sv.top);
        const float ring = Px(s, 6.0f);
        ComPtr<ID2D1SolidColorBrush> shadow, white;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.35f), &shadow);
        dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + Px(s, 0.8f)), ring, ring), shadow.get());
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), ring, ring), white.get(), Px(s, 2.0f));
    }

    if (s.hot == kPartEyedrop || s.eyedrop) {
        s.painter.FillRoundedRect(s.r_eyedrop, Px(s, 4.0f),
                                  s.eyedrop ? s.theme.fill_pressed : s.theme.fill_hover);
    }
    {
        ComPtr<ID2D1SolidColorBrush> ink;
        dc->CreateSolidColorBrush(s.theme.text, &ink);
        DrawEyedropperIcon(dc, ink.get(), s.r_eyedrop);
    }

    {
        const float pr = (s.r_preview.right - s.r_preview.left) * 0.5f;
        const auto c = D2D1::Point2F((s.r_preview.left + s.r_preview.right) * 0.5f,
                                     (s.r_preview.top + s.r_preview.bottom) * 0.5f);
        ComPtr<ID2D1SolidColorBrush> fill, ring;
        dc->CreateSolidColorBrush(HexColor(s.rgb), &fill);
        const float luma = 0.2126f * (Red(s.rgb) / 255.0f) + 0.7152f * (Green(s.rgb) / 255.0f) +
                           0.0722f * (Blue(s.rgb) / 255.0f);
        const D2D1_COLOR_F ring_c = luma > 0.82f
            ? (s.dark ? D2D1::ColorF(1, 1, 1, 0.45f) : D2D1::ColorF(0, 0, 0, 0.28f))
            : D2D1::ColorF(0, 0, 0, 0.18f);
        dc->CreateSolidColorBrush(ring_c, &ring);
        dc->FillEllipse(D2D1::Ellipse(c, pr, pr), fill.get());
        dc->DrawEllipse(D2D1::Ellipse(c, pr, pr), ring.get(), Px(s, 1.0f));
    }

    {
        D2D1_GRADIENT_STOP stops[7];
        for (int i = 0; i < 7; ++i) {
            stops[i] = D2D1::GradientStop(static_cast<float>(i) / 6.0f,
                                          HexColor(RgbFromHsv(i * 60, 255, 255)));
        }
        const float hr = (s.r_hue.bottom - s.r_hue.top) * 0.5f;
        FillLinearRound(dc, s.r_hue, hr,
                        D2D1::Point2F(s.r_hue.left, s.r_hue.top),
                        D2D1::Point2F(s.r_hue.right, s.r_hue.top), stops, 7);
        const float t = static_cast<float>(((s.hsv.h % 360) + 360) % 360) / 359.0f;
        const float hx = s.r_hue.left + hr + t * (s.r_hue.right - s.r_hue.left - 2.0f * hr);
        const float hy = (s.r_hue.top + s.r_hue.bottom) * 0.5f;
        const float tr = Px(s, 7.0f);
        ComPtr<ID2D1SolidColorBrush> sh, wh, bd;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.28f), &sh);
        dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &wh);
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.16f), &bd);
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(hx, hy + Px(s, 0.8f)), tr, tr), sh.get());
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(hx, hy), tr, tr), wh.get());
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(hx, hy), tr, tr), bd.get(), Px(s, 0.75f));
    }

    IDWriteTextFormat* body = s.compositor->TextFormat();
    IDWriteTextFormat* caption = s.compositor->SmallFormat();
    if (!caption) caption = body;

    auto draw_value = [&](const D2D1_RECT_F& rc, const std::wstring& text, int edit_index) {
        DrawInputField(s, dc, rc, s.focus_edit == edit_index);
        const float pad = Px(s, 6.0f);
        s.painter.DrawText(text, D2D1::RectF(rc.left + pad, rc.top, rc.right - pad, rc.bottom),
                           body, s.theme.text, fluent::HorizontalAlignment::Center);
    };
    if (s.format == TextFormatKind::Hex) {
        draw_value(s.r_hex, FormatHexColor(s.rgb, false), 0);
        s.painter.DrawText(l10n::Get(l10n::StringId::ColorFormatHex), s.r_hex_label, caption,
                           s.theme.text_secondary, fluent::HorizontalAlignment::Center);
    } else {
        const HslColor hsl = HslFromRgb(s.rgb);
        for (int i = 0; i < 3; ++i) {
            std::wstring text;
            if (s.format == TextFormatKind::Rgb) {
                const int v = i == 0 ? Red(s.rgb) : i == 1 ? Green(s.rgb) : Blue(s.rgb);
                text = std::to_wstring(v);
            } else {
                const int v = i == 0 ? hsl.h : i == 1 ? hsl.s : hsl.l;
                text = std::to_wstring(v);
            }
            draw_value(s.r_ch[i], text, i + 1);
            const wchar_t* label = s.format == TextFormatKind::Rgb
                ? (i == 0 ? L"R" : i == 1 ? L"G" : L"B")
                : (i == 0 ? L"H" : i == 1 ? L"S" : L"L");
            s.painter.DrawText(label, s.r_ch_label[i], caption, s.theme.text_secondary,
                               fluent::HorizontalAlignment::Center);
        }
    }
    if (s.hot == kPartCycle) {
        s.painter.FillRoundedRect(s.r_cycle, Px(s, 4.0f), s.theme.fill_hover);
    }
    DrawCommand(s, dc, command_icons::Icon::SwapVertical, s.r_cycle, s.theme.text);

    s.painter.FillRoundedRect(s.r_sep, 0.0f, s.theme.stroke_divider);

    for (int i = 0; i < s.swatch_vis; ++i) {
        const uint32_t sw = kBrowserPickerSwatches[i];
        const float cx = (s.r_swatch[i].left + s.r_swatch[i].right) * 0.5f;
        const float cy = (s.r_swatch[i].top + s.r_swatch[i].bottom) * 0.5f;
        const float r = (s.r_swatch[i].right - s.r_swatch[i].left) * 0.5f;
        ComPtr<ID2D1SolidColorBrush> fill, ring;
        dc->CreateSolidColorBrush(HexColor(sw), &fill);
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), fill.get());
        const float luma = 0.2126f * (((sw >> 16) & 0xff) / 255.0f) +
                           0.7152f * (((sw >> 8) & 0xff) / 255.0f) +
                           0.0722f * ((sw & 0xff) / 255.0f);
        if (luma > 0.75f) {
            dc->CreateSolidColorBrush(s.dark ? D2D1::ColorF(1, 1, 1, 0.35f)
                                             : D2D1::ColorF(0, 0, 0, 0.22f),
                                      &ring);
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), ring.get(), Px(s, 1.0f));
        }
        if ((sw & 0x00FFFFFFu) == s.rgb) {
            ComPtr<ID2D1SolidColorBrush> sel;
            dc->CreateSolidColorBrush(s.dark ? D2D1::ColorF(1, 1, 1, 0.92f)
                                             : D2D1::ColorF(0.12f, 0.12f, 0.12f, 0.88f),
                                      &sel);
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r + Px(s, 3.0f), r + Px(s, 3.0f)),
                            sel.get(), Px(s, 2.0f));
        }
    }
    if (s.hot == kPartExpand) {
        s.painter.FillRoundedRect(s.r_expand, Px(s, 4.0f), s.theme.fill_hover);
    }
    DrawCommand(s, dc,
                s.palette_open ? command_icons::Icon::ChevronUp : command_icons::Icon::ChevronDown,
                s.r_expand, s.theme.text_secondary);

    HRESULT hr = dc->EndDraw();
    dc->SetTextAntialiasMode(old_text_aa);
    dc->SetTarget(old_target.get());
    if (FAILED(hr)) return false;

    const auto cpu_props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> cpu;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, cpu_props, &cpu))) return false;
    if (FAILED(cpu->CopyFromBitmap(nullptr, bmp.get(), nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;

    if (!s.mem_dc) s.mem_dc = CreateCompatibleDC(nullptr);
    if (s.dib && (s.dib_w != s.win_w || s.dib_h != s.win_h)) {
        DeleteObject(s.dib);
        s.dib = nullptr;
        s.bits = nullptr;
    }
    if (!s.dib) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = s.win_w;
        bmi.bmiHeader.biHeight = -s.win_h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        s.dib = CreateDIBSection(s.mem_dc, &bmi, DIB_RGB_COLORS, &s.bits, nullptr, 0);
        if (!s.dib) {
            cpu->Unmap();
            return false;
        }
        s.dib_w = s.win_w;
        s.dib_h = s.win_h;
    }
    for (int row = 0; row < s.win_h; ++row) {
        std::memcpy(static_cast<uint8_t*>(s.bits) + static_cast<size_t>(row) * s.win_w * 4,
                    static_cast<const uint8_t*>(mapped.bits) + static_cast<size_t>(row) * mapped.pitch,
                    static_cast<size_t>(s.win_w) * 4);
    }
    cpu->Unmap();
    return true;
}

void PlaceEdits(State& s);

void Present(State& s, BYTE alpha, int dx) {
    if (!s.dib || !s.bits) return;
    HGDIOBJ old = SelectObject(s.mem_dc, s.dib);
    POINT dst{s.base_x + dx, s.base_y};
    POINT src{0, 0};
    SIZE sz{s.win_w, s.win_h};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(s.hwnd, nullptr, &dst, &sz, s.mem_dc, &src, 0, &blend, ULW_ALPHA);
    SelectObject(s.mem_dc, old);
    s.present_dx = dx;
    if (s.edits_shown) PlaceEdits(s);
}

void Repaint(State& s) {
    if (Render(s)) Present(s, 255, s.present_dx);
}

void SetEditText(State& s, HWND edit, const std::wstring& text) {
    s.syncing = true;
    SetWindowTextW(edit, text.c_str());
    s.syncing = false;
}

void SyncEdits(State& s, int skip_index) {
    if (skip_index != 0) SetEditText(s, s.edits[0], FormatHexColor(s.rgb, false));
    const HslColor hsl = HslFromRgb(s.rgb);
    const int ch[3] = {
        s.format == TextFormatKind::Hsl ? hsl.h : Red(s.rgb),
        s.format == TextFormatKind::Hsl ? hsl.s : Green(s.rgb),
        s.format == TextFormatKind::Hsl ? hsl.l : Blue(s.rgb),
    };
    for (int i = 0; i < 3; ++i) {
        if (skip_index == i + 1) continue;
        SetEditText(s, s.edits[i + 1], std::to_wstring(ch[i]));
    }
}

void ShowEdits(State& s) {
    if (s.edits_shown) return;
    s.edits_shown = true;
    PlaceEdits(s);
}

void PlaceEdits(State& s) {
    const float pad = Px(s, 6.0f);
    const bool hex = s.format == TextFormatKind::Hex;
    for (int i = 0; i < 4; ++i) {
        if (!s.edits[i]) continue;
        const bool show = s.edits_shown && !s.eyedrop && (hex ? i == 0 : i > 0);
        if (!show) {
            SetWindowPos(s.edits[i], HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_HIDEWINDOW);
            continue;
        }
        const D2D1_RECT_F& rc = i == 0 ? s.r_hex : s.r_ch[i - 1];
        const int x = static_cast<int>(std::lround(rc.left + pad));
        const int w = (std::max)(16, static_cast<int>(std::lround(rc.right - rc.left - pad * 2.0f)));
        const int line = (std::min)(s.line_h, static_cast<int>(rc.bottom - rc.top));
        const int y = static_cast<int>(std::lround(rc.top)) +
                      (std::max)(0, static_cast<int>(rc.bottom - rc.top - line) / 2);
        SetWindowPos(s.edits[i], HWND_TOP, x, y, w, line,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

int EditIndexFromHwnd(const State& s, HWND hwnd) {
    for (int i = 0; i < 4; ++i) {
        if (s.edits[i] == hwnd) return i;
    }
    return -1;
}

void ApplySvHue(State& s, float x, float y) {
    if (s.drag == kPartSv) {
        const float w = s.r_sv.right - s.r_sv.left;
        const float h = s.r_sv.bottom - s.r_sv.top;
        if (w <= 0.0f || h <= 0.0f) return;
        const SvNorm sv = SvFromSquare((x - s.r_sv.left) / w, (y - s.r_sv.top) / h);
        s.hsv.s = ClampI(static_cast<int>(std::lround(sv.s * 255.0f)), 0, 255);
        s.hsv.v = ClampI(static_cast<int>(std::lround(sv.v * 255.0f)), 0, 255);
        s.rgb = RgbFromHsv(s.hsv.h, s.hsv.s, s.hsv.v);
    } else if (s.drag == kPartHue) {
        const float inset = (s.r_hue.bottom - s.r_hue.top) * 0.5f;
        const float left = s.r_hue.left + inset;
        const float right = s.r_hue.right - inset;
        const float t = right > left ? ClampF((x - left) / (right - left), 0.0f, 1.0f) : 0.0f;
        s.hsv.h = ClampI(static_cast<int>(std::lround(t * 359.0f)), 0, 359);
        s.rgb = RgbFromHsv(s.hsv.h, s.hsv.s, s.hsv.v);
    } else {
        return;
    }
    SyncEdits(s, -1);
    NotifyPreview(s);
    Repaint(s);
}

void FocusEdit(State& s, int index) {
    if (index >= 0 && index < 4 && s.edits[index] && IsWindowVisible(s.edits[index]))
        SetFocus(s.edits[index]);
}

bool IsOurs(const State& s, HWND hwnd) {
    if (!hwnd) return false;
    if (hwnd == s.hwnd) return true;
    return EditIndexFromHwnd(s, hwnd) >= 0;
}

uint32_t SampleScreenRgb(POINT screen_pt) {
    HDC hdc = GetDC(nullptr);
    if (!hdc) return 0xFFFFFFFFu;
    const COLORREF c = GetPixel(hdc, screen_pt.x, screen_pt.y);
    ReleaseDC(nullptr, hdc);
    if (c == CLR_INVALID) return 0xFFFFFFFFu;
    return (static_cast<uint32_t>(GetRValue(c)) << 16) |
           (static_cast<uint32_t>(GetGValue(c)) << 8) |
           static_cast<uint32_t>(GetBValue(c));
}

void ApplyEyedropSample(State& s) {
    POINT pt{};
    GetCursorPos(&pt);
    const uint32_t rgb = SampleScreenRgb(pt);
    if (rgb == 0xFFFFFFFFu) return;
    SetRgb(s, rgb, true);
    SyncEdits(s, -1);
    NotifyPreview(s);
    Repaint(s);
}

void EndEyedrop(State& s, bool keep) {
    if (!s.eyedrop) return;
    s.eyedrop = false;
    if (!keep) {
        SetRgb(s, s.eyedrop_restore, true);
        SyncEdits(s, -1);
        NotifyPreview(s);
    }
    if (GetCapture() == s.hwnd) ReleaseCapture();
    PlaceEdits(s);
    Repaint(s);
}

void StartEyedrop(State& s) {
    s.eyedrop = true;
    s.eyedrop_restore = s.rgb;
    PlaceEdits(s);
    SetCapture(s.hwnd);
    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
    Repaint(s);
}

void CycleFormat(State& s) {
    s.format = static_cast<TextFormatKind>((static_cast<int>(s.format) + 1) % 3);
    SyncEdits(s, -1);
    PlaceEdits(s);
    Repaint(s);
}

void TogglePalette(State& s) {
    s.palette_open = !s.palette_open;
    Layout(s);
    HMONITOR monitor = MonitorFromWindow(s.hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    s.base_x = ClampI(s.base_x, mi.rcWork.left, mi.rcWork.right - s.win_w);
    s.base_y = ClampI(s.base_y, mi.rcWork.top, mi.rcWork.bottom - s.win_h);
    SetWindowPos(s.hwnd, nullptr, s.base_x + s.present_dx, s.base_y, s.win_w, s.win_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    Repaint(s);
}

void Nudge(State& s, int dh, int ds, int dv) {
    s.hsv.h = (s.hsv.h + dh + 3600) % 360;
    s.hsv.s = ClampI(s.hsv.s + ds, 0, 255);
    s.hsv.v = ClampI(s.hsv.v + dv, 0, 255);
    s.rgb = RgbFromHsv(s.hsv.h, s.hsv.s, s.hsv.v);
    SyncEdits(s, -1);
    NotifyPreview(s);
    Repaint(s);
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR,
                          DWORD_PTR ref) {
    auto* hook = reinterpret_cast<EditHook*>(ref);
    State* s = hook ? hook->s : nullptr;
    if (!s) return DefSubclassProc(hwnd, msg, wp, lp);
    const D2D1_COLOR_F fg = ColorFromRef(HcEditText(ColorRefFrom(s->theme.text)));
    const D2D1_COLOR_F bg = ColorFromRef(HcEditBack(ColorRefFrom(FieldFill(*s))));
    if (s->compositor) {
        LRESULT result = 0;
        if (HandleChildEditMessage(*s->compositor, s->compositor->TextFormat(), fg, bg,
                                   EditBackBrush(s->edit_brush), hwnd, msg, wp, lp, result))
            return result;
    }
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            s->done = true;
            return 0;
        }
        if (wp == VK_RETURN) {
            s->accepted = true;
            s->done = true;
            return 0;
        }
        if (wp == VK_TAB) {
            const int dir = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
            if (s->format == TextFormatKind::Hex) FocusEdit(*s, 0);
            else FocusEdit(*s, ((hook->index - 1 + dir + 3) % 3) + 1);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB) return 0;
        if (hook->index == 0 && wp >= 0x20) {
            const wchar_t ch = static_cast<wchar_t>(wp);
            const bool ok = (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') ||
                            (ch >= L'A' && ch <= L'F') || ch == L'#';
            if (!ok) return 0;
        }
        break;
    }
    if (!s->compositor) return DefSubclassProc(hwnd, msg, wp, lp);
    return DefPresentedChildEditProc(*s->compositor, s->compositor->TextFormat(), fg, bg,
                                     hwnd, msg, wp, lp);
}

void OnEditChange(State& s, int index) {
    if (s.syncing) return;
    wchar_t text[32]{};
    GetWindowTextW(s.edits[index], text, ARRAYSIZE(text));
    if (index == 0) {
        uint32_t argb = 0;
        if (ParseHexColor(text, argb)) {
            SetRgb(s, argb & 0x00FFFFFFu, true);
            SyncEdits(s, 0);
            NotifyPreview(s);
            Repaint(s);
        }
        return;
    }
    if (s.format == TextFormatKind::Hex) return;
    wchar_t a[32]{}, b[32]{}, c[32]{};
    GetWindowTextW(s.edits[1], a, ARRAYSIZE(a));
    GetWindowTextW(s.edits[2], b, ARRAYSIZE(b));
    GetWindowTextW(s.edits[3], c, ARRAYSIZE(c));
    uint32_t rgb = 0;
    const bool ok = s.format == TextFormatKind::Rgb ? ParseRgbChannels(a, b, c, rgb)
                                                    : ParseHslChannels(a, b, c, rgb);
    if (!ok) return;
    SetRgb(s, rgb, true);
    SyncEdits(s, index);
    NotifyPreview(s);
    Repaint(s);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    State* s = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        s = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        s->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }
    if (!s) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        if (s->eyedrop) {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        }
        break;
    case WM_CTLCOLOREDIT: {
        const int index = EditIndexFromHwnd(*s, reinterpret_cast<HWND>(lp));
        if (index < 0) break;
        HDC hdc = reinterpret_cast<HDC>(wp);
        SetTextColor(hdc, HcEditText(ColorRefFrom(s->theme.text)));
        SetBkColor(hdc, HcEditBack(ColorRefFrom(FieldFill(*s))));
        return reinterpret_cast<LRESULT>(EditBackBrush(s->edit_brush));
    }
    case WM_COMMAND: {
        const HWND from = reinterpret_cast<HWND>(lp);
        const int index = from ? EditIndexFromHwnd(*s, from) : -1;
        if (index < 0) break;
        if (HIWORD(wp) == EN_CHANGE) OnEditChange(*s, index);
        else if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) {
            s->focus_edit = HIWORD(wp) == EN_SETFOCUS ? index : -1;
            for (HWND e : s->edits) {
                if (e) InvalidateRect(e, nullptr, FALSE);
            }
            Repaint(*s);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const float x = static_cast<float>(GET_X_LPARAM(lp));
        const float y = static_cast<float>(GET_Y_LPARAM(lp));
        if (s->eyedrop) {
            ApplyEyedropSample(*s);
            EndEyedrop(*s, true);
            return 0;
        }
        const int part = HitTest(*s, x, y);
        SetFocus(hwnd);
        if (part == kPartSv || part == kPartHue) {
            s->drag = part;
            SetCapture(hwnd);
            ApplySvHue(*s, x, y);
        } else if (part == kPartEyedrop) {
            StartEyedrop(*s);
        } else if (part == kPartCycle) {
            CycleFormat(*s);
        } else if (part == kPartExpand) {
            TogglePalette(*s);
        } else if (part >= kPartSwatch0 && part < kPartSwatch0 + s->swatch_vis) {
            SetRgb(*s, kBrowserPickerSwatches[part - kPartSwatch0], true);
            SyncEdits(*s, -1);
            NotifyPreview(*s);
            Repaint(*s);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (s->eyedrop) {
            ApplyEyedropSample(*s);
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return 0;
        }
        const float x = static_cast<float>(GET_X_LPARAM(lp));
        const float y = static_cast<float>(GET_Y_LPARAM(lp));
        if (s->drag != kPartNone) {
            ApplySvHue(*s, x, y);
            return 0;
        }
        const int part = HitTest(*s, x, y);
        if (part != s->hot) {
            s->hot = part;
            Repaint(*s);
        }
        if (!s->tracking_leave) {
            s->tracking_leave = true;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        s->tracking_leave = false;
        if (s->hot != kPartNone && !s->eyedrop) {
            s->hot = kPartNone;
            Repaint(*s);
        }
        return 0;
    case WM_LBUTTONUP:
        if (s->eyedrop) return 0;
        s->drag = kPartNone;
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (s->eyedrop) {
                EndEyedrop(*s, false);
                return 0;
            }
            s->done = true;
            return 0;
        }
        if (wp == VK_RETURN) {
            s->accepted = true;
            s->done = true;
            return 0;
        }
        if (!s->eyedrop && GetFocus() == hwnd) {
            const int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
            if (wp == VK_LEFT) Nudge(*s, -step, 0, 0);
            else if (wp == VK_RIGHT) Nudge(*s, step, 0, 0);
            else if (wp == VK_UP) Nudge(*s, 0, 0, step);
            else if (wp == VK_DOWN) Nudge(*s, 0, 0, -step);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (s->drag != kPartNone) s->drag = kPartNone;
        if (s->eyedrop && reinterpret_cast<HWND>(lp) != hwnd) EndEyedrop(*s, true);
        return 0;
    case WM_CLOSE:
        s->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void DestroySurface(State& s) {
    if (s.dib) DeleteObject(s.dib);
    if (s.mem_dc) DeleteDC(s.mem_dc);
    s.dib = nullptr;
    s.mem_dc = nullptr;
    s.bits = nullptr;
}

void CreateBrushes(State& s) {
    const D2D1_COLOR_F fill = FieldFill(s);
    s.edit_brush = CreateSolidBrush(ColorRefFrom(fill));
}

void CreateEdits(State& s) {
    s.font = typography::CreateEditFont(s.scale, 13.0f);
    if (s.font) s.line_h = EditLineHeight(s.hwnd, s.font, 1 << 16);
    for (int i = 0; i < 4; ++i) {
        DWORD style = ES_CENTER;
        if (i > 0) style |= ES_NUMBER;
        HWND e = CreateChildEdit(s.hwnd, L"", style);
        if (!e) continue;
        SetWindowTheme(e, L"", L"");
        if (s.font) SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(s.font), TRUE);
        SendMessageW(e, EM_SETLIMITTEXT, i == 0 ? 7 : 3, 0);
        s.hooks[static_cast<size_t>(i)].s = &s;
        s.hooks[static_cast<size_t>(i)].index = i;
        SetWindowSubclass(e, EditProc, static_cast<UINT_PTR>(i + 1),
                          reinterpret_cast<DWORD_PTR>(&s.hooks[static_cast<size_t>(i)]));
        s.edits[i] = e;
    }
}

bool SavePng(const wchar_t* path, int w, int h, void* bits) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
        return false;
    }
    ComPtr<IWICStream> stream;
    wic->CreateStream(&stream);
    if (!stream.get() || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) {
        return false;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) return false;
    if (FAILED(frame->Initialize(nullptr))) return false;
    if (FAILED(frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h)))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    if (FAILED(frame->WritePixels(static_cast<UINT>(h), static_cast<UINT>(w * 4),
                                  static_cast<UINT>(w * 4 * h),
                                  static_cast<BYTE*>(bits)))) {
        return false;
    }
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(encoder->Commit());
}

void InitState(State& s, Compositor* compositor, float scale, bool dark, D2D1_COLOR_F accent,
               uint32_t initial_rgb) {
    s.compositor = compositor;
    s.scale = (std::max)(0.5f, scale);
    s.dark = dark;
    s.accent = accent;
    s.theme = IsHighContrast() ? MakeHighContrastTheme() : MakeTheme(dark, accent);
    SetRgb(s, initial_rgb, false);
    s.painter.SetCompositor(compositor);
    s.painter.SetScale(s.scale);
    Layout(s);
}

} // namespace

bool BrowserColorPicker::Pick(HWND owner, Compositor* compositor, float scale, POINT screen_pt,
                              uint32_t initial_rgb, bool dark,
                              const std::function<void(uint32_t)>& on_preview,
                              uint32_t& result_rgb) {
    if (!compositor || !compositor->Dc()) return false;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        registered = true;
    }

    State s;
    s.owner = owner;
    s.on_preview = on_preview;
    InitState(s, compositor, scale, dark, GetAccentColor(), initial_rgb);

    s.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, kClassName, L"", WS_POPUP,
                             0, 0, s.win_w, s.win_h, owner, nullptr,
                             GetModuleHandleW(nullptr), &s);
    if (!s.hwnd) return false;

    HMONITOR monitor = MonitorFromPoint(screen_pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    s.base_x = ClampI(screen_pt.x, mi.rcWork.left, mi.rcWork.right - s.win_w);
    s.base_y = ClampI(screen_pt.y, mi.rcWork.top, mi.rcWork.bottom - s.win_h);

    CreateBrushes(s);
    CreateEdits(s);
    SyncEdits(s, -1);
    NotifyPreview(s);

    if (Render(s)) {
        ShowWindow(s.hwnd, SW_SHOW);
        SetForegroundWindow(s.hwnd);
        const auto t0 = std::chrono::steady_clock::now();
        constexpr fluent::MotionSpec slide{170.0f, fluent::EasingCurve::OutCubic};
        for (;;) {
            const float elapsed = static_cast<float>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count());
            if (elapsed >= slide.duration_ms) break;
            const float p = fluent::EvaluateMotion(slide, elapsed);
            const int dx = static_cast<int>(std::lround((1.0f - p) * 10.0f * s.scale));
            const BYTE alpha = static_cast<BYTE>(
                std::lround(ClampF(elapsed / 120.0f, 0.0f, 1.0f) * 255.0f));
            Present(s, alpha, dx);
            Sleep(10);
        }
        Present(s, 255, 0);
    } else {
        ShowWindow(s.hwnd, SW_SHOW);
        SetForegroundWindow(s.hwnd);
    }
    ShowEdits(s);
    Repaint(s);

    MSG msg{};
    while (!s.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        const bool click = msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN ||
                           msg.message == WM_MBUTTONDOWN || msg.message == WM_NCLBUTTONDOWN ||
                           msg.message == WM_NCRBUTTONDOWN || msg.message == WM_NCMBUTTONDOWN;
        if (click && !IsOurs(s, msg.hwnd) && !s.eyedrop) {
            s.accepted = true;
            s.done = true;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    for (HWND e : s.edits) {
        if (e) DestroyWindow(e);
    }
    result_rgb = s.accepted ? s.rgb : initial_rgb;
    const bool accepted = s.accepted;
    DestroyWindow(s.hwnd);
    DestroySurface(s);
    if (s.font) DeleteObject(s.font);
    if (s.edit_brush) DeleteObject(s.edit_brush);
    return accepted;
}

bool BrowserColorPicker::SaveDebugSnapshot(Compositor* compositor, const wchar_t* png_path,
                                           bool dark, D2D1_COLOR_F accent, float scale) {
    if (!compositor || !compositor->Dc() || !png_path) return false;
    State s;
    InitState(s, compositor, scale, dark, accent, 0xD6E4F0u);
    s.hot = kPartEyedrop;
    if (!Render(s)) return false;
    const bool ok = SavePng(png_path, s.win_w, s.win_h, s.bits);
    DestroySurface(s);
    return ok;
}

} // namespace pulse::ui
