// ui_sidebar.cpp — Sidebar.
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "tab_shape.h"
#include "command_icons.h"
#include "bloom_accent_picker.h"
#include "typography.h"
#include "../app/resource.h"
#include "../app/places.h"
#include "../common/text_format.h"
#include <windowsx.h>
#include <d2d1effects.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <string_view>

namespace pulse::ui {

namespace {

// Accent insertion indicator shared by the two sidebar reorders (sections and
// quick-access pins).
void DrawSidebarInsertionLine(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush,
                              const D2D1_RECT_F& sb, float scale, float y) {
    const SidebarMetrics m = MakeSidebarMetrics(scale);
    const float th = 2.5f * scale;
    const float lx0 = sb.left + m.pad + 2.0f * scale;
    const float lx1 = sb.right - m.pad - 2.0f * scale;
    FillRoundedRect(dc, brush, lx0, y - th * 0.5f, lx1 - lx0, th, th * 0.5f);
    dc->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(lx0 + 4.0f * scale, y), 3.0f * scale, 3.0f * scale),
        brush);
}

// Sidebar motion (animation phase 7, pulse_motion_v2): the selection pill
// stretches toward the new row - its leading edge on a stiff spring, the
// trailing edge on a softer one - a hover plate glides under the pointer, and
// an item's expand chevron turns. The state lives here, per renderer; frames
// come from the renderer's frame pump (RequestMotionFrame), so idle stays
// idle: nothing ticks once everything has settled.
constexpr motion::SpringSpec kPillLead{900.0f, 60.0f};
constexpr motion::SpringSpec kPillTrail{300.0f, 30.0f};
constexpr float kPillMaxStretch = 1.9f;  // times the row height
constexpr float kHoverFadeInMs = 90.0f, kHoverFadeOutMs = 120.0f;

struct ChevronTurn {
    bool expanded = false;
    uint64_t at = 0;
};

struct SidebarMotion {
    motion::SpringRectMotion lead, trail, hover;
    uint64_t hover_context = 0;
    float hover_alpha = 0.0f;
    D2D1_RECT_F hover_rect{}, hover_target{};
    int64_t hover_key = 0;
    float hover_radius = 0.0f;
    uint64_t hover_last = 0;
    std::unordered_map<std::wstring, ChevronTurn> chevrons;
    // Quick-access reorder: per-row make-room offsets (px) keyed by run, the
    // held row's lift start and the last frame time for the follow filter.
    std::unordered_map<int, float> pin_offsets;
    int pin_run = -1;
    // Count roll + gulp per row path (motion v2 "dropbin"). epoch counts the
    // expanded sidebar's frames so only a change seen frame-to-frame plays.
    struct CountRoll {
        std::wstring last, prev;
        uint64_t start = 0;
        uint64_t epoch = 0;
        bool down = false;
    };
    std::unordered_map<std::wstring, CountRoll> counts;
    uint64_t count_epoch = 0;
    // Star flight (motion v2 "star"): launch point, start, and the 已加星标
    // row as last painted (valid when star_dest_epoch == count_epoch).
    D2D1_POINT_2F star_from{};
    uint64_t star_start = 0;
    bool star_landed = false;
    D2D1_RECT_F star_dest{};
    uint64_t star_dest_epoch = 0;
    uint64_t pin_lift_start = 0;
    uint64_t pin_last = 0;
    bool animations = true;
    uint64_t animations_checked = 0;
};

SidebarMotion& SidebarMotionFor(const void* renderer) {
    static std::unordered_map<const void*, SidebarMotion> motions;
    return motions[renderer];
}

// The pill spans both springs: leading edge from the stiff one, trailing edge
// from the soft one, cut to kPillMaxStretch rows and a touch thinner while
// stretched.
D2D1_RECT_F StretchPill(const D2D1_RECT_F& lead, const D2D1_RECT_F& trail, float scale) {
    D2D1_RECT_F pill = D2D1::RectF((std::min)(lead.left, trail.left), (std::min)(lead.top, trail.top),
                                   (std::max)(lead.right, trail.right), (std::max)(lead.bottom, trail.bottom));
    const float row_h = (std::max)(1.0f, lead.bottom - lead.top);
    const float max_h = row_h * kPillMaxStretch;
    if (pill.bottom - pill.top > max_h) {
        if (lead.top >= trail.top) pill.top = pill.bottom - max_h;  // moving down: the bottom leads
        else pill.bottom = pill.top + max_h;
    }
    const float stretch = (std::clamp)((pill.bottom - pill.top - row_h) / row_h, 0.0f, 1.0f);
    pill.left += 2.0f * scale * stretch;
    pill.right -= 2.0f * scale * stretch;
    return pill;
}

float ChevronAngle(SidebarMotion& m, const std::wstring& key, bool expanded, uint64_t now, bool& moving) {
    auto [it, fresh] = m.chevrons.try_emplace(key);
    ChevronTurn& turn = it->second;
    if (fresh) {
        turn.expanded = expanded;
        turn.at = 0;
    } else if (turn.expanded != expanded) {
        turn.expanded = expanded;
        turn.at = m.animations ? now : 0;
    }
    float p = 1.0f;
    if (turn.at) {
        const float t = static_cast<float>(now - turn.at);
        p = motion::SpringProgress(motion::spring::Snappy, t);
        if (t < static_cast<float>(motion::SpringSettleMs(motion::spring::Snappy))) moving = true;
        else turn.at = 0;
    }
    return 90.0f * (expanded ? p : 1.0f - p);
}

} // namespace

D2D1_RECT_F MainRenderer::SidebarRect(float w, float h) const {
    float top = title_bar_height_ + margin_;
    float bottom = h - status_height_ - margin_;
    const float right = SidebarPeekVisible(w) ? SidebarFullWidth(w) : SidebarOwnWidth(w);
    return D2D1::RectF(0.0f, top, right, bottom);
}

void MainRenderer::SidebarGroupBands(const WindowViewModel& vm, float w, float h,
                                     std::vector<SidebarGroupBand>& out) const {
    std::vector<SidebarSlot> slots;
    LayoutSidebar(vm, SidebarRect(w, h), scale_, slots, &out);
}

// Hover peek: the expanded sidebar floats over the panes on a raised card.
void MainRenderer::DrawSidebarPeek(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                   const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const D2D1_RECT_F sb = SidebarRect(rect.right, rect.bottom);
    const float r = theme.radius_flyout * scale_;
    const D2D1_RECT_F card = D2D1::RectF(sb.left + 4.0f * scale_, sb.top - 2.0f * scale_,
                                         sb.right + 6.0f * scale_, sb.bottom + 2.0f * scale_);
    // Cheap layered shadow; no effect graph for a transient overlay.
    for (int i = 3; i >= 1; --i) {
        const float g = static_cast<float>(i) * 3.0f * scale_;
        MakeBrush(dc, D2D1_COLOR_F{0.0f, 0.0f, 0.0f, vm.dark ? 0.12f : 0.045f}, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), card.left - g * 0.5f, card.top + g * 0.4f,
                        card.right - card.left + g, card.bottom - card.top + g * 0.6f, r + g);
    }
    MakeBrush(dc, BlendOver(theme.surface_flyout, theme.bg), brFillSelected_);
    FillRoundedRect(dc, brFillSelected_.get(), card.left, card.top,
                    card.right - card.left, card.bottom - card.top, r);
    MakeBrush(dc, theme.stroke_card, brStrokeCard_);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(card.left + 0.5f, card.top + 0.5f,
        card.right - 0.5f, card.bottom - 0.5f), r, r), brStrokeCard_.get(), 1.0f);
    DrawSidebar(vm, rect, theme);
}

void MainRenderer::DrawSidebar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    D2D1_RECT_F sb = SidebarRect(rect.right, rect.bottom);
    const D2D1_RECT_F sb_clip = sb;
    // Collapse/expand: lay out at the expanded width and reveal it through
    // the easing width, so labels slide rather than re-wrap every frame.
    if (collapse_anim_ && rect.right / scale_ >= kSidebarRailWindowDip)
        sb.right = (std::max)(sb.right, SidebarFullWidth(rect.right));
    const float w = sb.right - sb.left;

    // Partially visible scrolled rows must not paint over the toolbar or status bar.
    dc->PushAxisAlignedClip(sb_clip, D2D1_ANTIALIAS_MODE_ALIASED);
    // Motion v2 "rail": while the sidebar folds, the icon column stays solid
    // and everything right of it (labels, counts) fades - late on the way
    // out to full width, early on the way into the rail.
    bool label_layer = false;
    if (collapse_anim_ && rect.right / scale_ >= kSidebarRailWindowDip) {
        const float open = std::clamp((1.0f - collapse_value_ - 0.35f) / 0.65f, 0.0f, 1.0f);
        const D2D1_GRADIENT_STOP stops[] = {
            {0.0f, D2D1::ColorF(0, 0, 0, 1.0f)},
            {0.5f, D2D1::ColorF(0, 0, 0, 1.0f)},
            {1.0f, D2D1::ColorF(0, 0, 0, open * open)},
        };
        ComPtr<ID2D1GradientStopCollection> stop_set;
        ComPtr<ID2D1LinearGradientBrush> fade;
        const float x0 = sb.left + 38.0f * scale_;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 3, &stop_set)) && stop_set.get() &&
            SUCCEEDED(dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(x0 - 8.0f * scale_, 0.0f),
                                                    D2D1::Point2F(x0 + 8.0f * scale_, 0.0f)),
                stop_set.get(), &fade)) && fade.get()) {
            D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(sb_clip, nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.0f, fade.get());
            dc->PushLayer(lp, nullptr);
            label_layer = true;
        }
    }

    // The sidebar sits on the shared sheet painted by Render(); the pane cards
    // beside it provide the edge, so it has no fill or divider of its own.

    std::vector<SidebarSlot> slots;
    LayoutSidebar(vm, sb, scale_, slots);
    const bool compact = SidebarRailLayout(w, scale_);
    painter_.BeginFrame(theme, IsHighContrast());

    // Only the most specific matching row is selected (Desktop, not also the
    // C: drive that contains it). That row owns one pill that glides to the
    // newly selected row (ui_motion.h); reorder gestures and high contrast
    // keep the static fill.
    int selected_slot = -1;
    {
        size_t best = 0;
        for (size_t k = 0; k < slots.size(); ++k) {
            const auto& slot = slots[k];
            if (slot.kind != SidebarSlot::Item && slot.kind != SidebarSlot::Drive &&
                slot.kind != SidebarSlot::Tag) continue;
            if (slot.group < 0 || slot.item < 0) continue;
            const auto& item = vm.sidebar[slot.group].items[slot.item];
            if (item.tab_row) continue;  // tabs mark themselves (active card)
            if (!PathIsSelfOrChild(item.path, vm.pane.path)) continue;
            if (selected_slot < 0 || item.path.size() > best) {
                selected_slot = static_cast<int>(k);
                best = item.path.size();
            }
        }
    }
    const bool reordering = vm.sidebar_pin_drag_index >= 0 || vm.sidebar_group_drag_id >= 0;
    const int pill_slot = !reordering && !IsHighContrast() ? selected_slot : -1;
    SidebarMotion& sm = SidebarMotionFor(this);
    if (motion_now_ - sm.animations_checked > 2000 || !sm.animations_checked) {
        // The Windows animation switch, re-read every couple of seconds.
        sm.animations_checked = motion_now_ ? motion_now_ : 1;
        sm.animations = motion::SystemAnimationsEnabled();
        sm.lead.SetAnimationsEnabled(sm.animations);
        sm.trail.SetAnimationsEnabled(sm.animations);
        sm.hover.SetAnimationsEnabled(sm.animations);
    }
    bool sidebar_moving = false;
    // One hover plate glides between rows (the menus' plate); reorders and
    // high contrast keep each row's own static hover.
    const bool plate_live = !reordering && !IsHighContrast();
    int hover_slot = -1;
    if (plate_live) {
        for (size_t k = 0; k < slots.size(); ++k) {
            const auto& slot = slots[k];
            if (slot.kind != SidebarSlot::Item && slot.kind != SidebarSlot::Drive &&
                slot.kind != SidebarSlot::Tag) continue;
            if (slot.group < 0 || slot.item < 0) continue;
            if (static_cast<int>(k) == pill_slot) continue;
            const auto& item = vm.sidebar[slot.group].items[slot.item];
            if (item.tab_row && item.tab_active) continue;  // the raised card is its own hover
            if (IsHovered(vm, HitTestResult::SidebarItem, slot.run) ||
                IsHovered(vm, HitTestResult::SidebarItemAction, slot.run) ||
                IsHovered(vm, HitTestResult::SidebarItemExpand, slot.run)) {
                hover_slot = static_cast<int>(k);
                break;
            }
        }
    }
    {
        const uint64_t context = compact ? 1u : 2u;
        const float dt = static_cast<float>((std::min<uint64_t>)(motion_now_ - sm.hover_last, 64));
        sm.hover_last = motion_now_;
        if (hover_slot >= 0) {
            const auto& slot = slots[static_cast<size_t>(hover_slot)];
            // Coming from nothing, the plate appears at the row (no glide
            // from wherever it was last).
            if (sm.hover_alpha <= 0.0f) ++sm.hover_context;
            sm.hover_key = slot.run;
            sm.hover_target = slot.rc;
            sm.hover_rect = sm.hover.Update(context * 0x10000u + sm.hover_context, slot.run, slot.rc,
                                            motion_frame_, motion_now_, motion::spring::Plate);
            sm.hover_radius = (compact ? theme.radius_control : theme.radius_flyout) * scale_;
            sm.hover_alpha = sm.animations ? (std::min)(1.0f, sm.hover_alpha + dt / kHoverFadeInMs) : 1.0f;
        } else {
            // Fading out where it was; kept in step so a pointer coming back
            // mid-fade glides on from there.
            sm.hover_alpha = sm.animations && plate_live ? (std::max)(0.0f, sm.hover_alpha - dt / kHoverFadeOutMs) : 0.0f;
            if (sm.hover_alpha > 0.0f)
                sm.hover_rect = sm.hover.Update(context * 0x10000u + sm.hover_context, sm.hover_key, sm.hover_target,
                                                motion_frame_, motion_now_, motion::spring::Plate);
            else
                sm.hover.Hide();
        }
        if (sm.hover.Active(motion_now_) || (sm.hover_alpha > 0.0f && sm.hover_alpha < 1.0f)) sidebar_moving = true;
    }
    const auto isSelectedSlot = [&](const SidebarSlot& slot) {
        return selected_slot >= 0 && &slot == &slots[static_cast<size_t>(selected_slot)];
    };
    // Vertical tabs: the block sits in a recessed well so open tabs read as
    // their own thing, apart from the places below; the active tab's card
    // rises out of it.
    for (int g = 0; g < static_cast<int>(vm.sidebar.size()); ++g) {
        if (!vm.sidebar[static_cast<size_t>(g)].tabs_section) continue;
        float top = 1e9f, bottom = -1.0f, left = 1e9f, right = -1.0f;
        for (const auto& slot : slots) {
            if (slot.group != g) continue;
            top = (std::min)(top, slot.rc.top);
            bottom = (std::max)(bottom, slot.rc.bottom);
            left = (std::min)(left, slot.rc.left);
            right = (std::max)(right, slot.rc.right);
        }
        if (bottom < 0.0f) break;
        const float pad = (compact ? 2.0f : 4.0f) * scale_;
        const D2D1_RECT_F well = D2D1::RectF((std::max)(sb.left + 2.0f * scale_, left - pad),
            top - pad, (std::min)(sb.right - 2.0f * scale_, right + pad), bottom + pad);
        const float r = theme.radius_flyout * scale_;
        MakeBrush(dc, vm.dark ? D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 0.045f}
                              : D2D1_COLOR_F{0.0f, 0.0f, 0.0f, 0.035f}, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), well.left, well.top,
                        well.right - well.left, well.bottom - well.top, r);
        MakeBrush(dc, vm.dark ? D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 0.06f}
                              : D2D1_COLOR_F{0.0f, 0.0f, 0.0f, 0.05f}, brStrokeDivider_);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(well.left + 0.5f, well.top + 0.5f,
            well.right - 0.5f, well.bottom - 0.5f), r, r), brStrokeDivider_.get(), 1.0f);
        break;
    }
    if (sm.hover_alpha > 0.0f) {
        D2D1_COLOR_F fill = theme.fill_hover;
        fill.a *= sm.hover_alpha;
        painter_.FillRoundedRect(sm.hover_rect, sm.hover_radius, fill);
    }
    if (pill_slot >= 0) {
        const auto& slot = slots[static_cast<size_t>(pill_slot)];
        const auto& item = vm.sidebar[slot.group].items[slot.item];
        const uint64_t context = compact ? 1u : 2u;
        const int64_t key = static_cast<int64_t>(std::hash<std::wstring>{}(item.path)) ^
                            (static_cast<int64_t>(slot.group) << 48);
        const D2D1_RECT_F lead = sm.lead.Update(context, key, slot.rc, motion_frame_, motion_now_, kPillLead);
        const D2D1_RECT_F trail = sm.trail.Update(context, key, slot.rc, motion_frame_, motion_now_, kPillTrail);
        const D2D1_RECT_F pill = StretchPill(lead, trail, scale_);
        if (sm.lead.Active(motion_now_) || sm.trail.Active(motion_now_)) sidebar_moving = true;
        const float radius = (compact ? theme.radius_control : theme.radius_flyout) * scale_;
        // Same fills the static rows use (Painter::DrawSidebarItem / rail).
        D2D1_COLOR_F fill = theme.fill_selected;
        if (!compact) {
            fill = vm.dark ? D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 26.0f / 255.0f}
                           : D2D1_COLOR_F{0.0f, 0.0f, 0.0f, 20.0f / 255.0f};
        }
        MakeBrush(dc, fill, brFillSelected_);
        FillRoundedRect(dc, brFillSelected_.get(), pill.left, pill.top,
                        pill.right - pill.left, pill.bottom - pill.top, radius);
    }
    if (pill_slot < 0) {
        sm.lead.Hide();
        sm.trail.Hide();
    }
    const auto isPillSlot = [&](const SidebarSlot& slot) {
        return pill_slot >= 0 && &slot == &slots[static_cast<size_t>(pill_slot)];
    };
    // Every return below asks for the next frame while something moves.
    struct FrameRequest {
        const MainRenderer* self;
        const bool& moving;
        ~FrameRequest() {
            if (moving) self->RequestMotionFrame();
        }
    } frame_request{this, sidebar_moving};

    if (compact) {
        for (const auto& slot : slots) {
            if (slot.kind == SidebarSlot::Rail) {
                // The section's own rail row: one icon stands in for the whole
                // section, and clicking it folds or unfolds the section.
                const auto& group = vm.sidebar[slot.group];
                if (IsHovered(vm, HitTestResult::SidebarHeader, slot.group)) {
                    MakeBrush(dc, theme.fill_hover, brFillSelected_);
                    FillRoundedRect(dc, brFillSelected_.get(), slot.rc.left, slot.rc.top,
                        slot.rc.right - slot.rc.left, slot.rc.bottom - slot.rc.top,
                        theme.radius_control * scale_);
                }
                std::wstring glyph = group.icon_glyph;
                if (glyph.empty() && !group.items.empty())
                    glyph = group.items.front().icon_glyph;
                if (glyph.empty()) glyph = kIconFolder;
                const int rail_svg = FluentSvgIdForGlyph(glyph);
                const float rail_icon = 20.0f * scale_;
                const float rail_pad_x = (slot.rc.right - slot.rc.left - rail_icon) * 0.5f;
                const float rail_pad_y = (slot.rc.bottom - slot.rc.top - rail_icon) * 0.5f;
                const D2D1_RECT_F rail_rc = D2D1::RectF(
                    slot.rc.left + rail_pad_x, slot.rc.top + rail_pad_y,
                    slot.rc.right - rail_pad_x, slot.rc.bottom - rail_pad_y);
                if (IsHighContrast() || rail_svg == 0 || !DrawFluentSvg(rail_svg, rail_rc, 1.0f, nullptr, true)) {
                    const std::wstring fallback = group.header.empty()
                        ? std::wstring(L"?") : group.header.substr(0, 1);
                    DrawIconText(slot.rc.left, slot.rc.top, slot.rc.right - slot.rc.left,
                        slot.rc.bottom - slot.rc.top, glyph, fallback,
                        theme.text_secondary, 0.92f);
                }
                if (group.badge_count > 0) {
                    // 网络位置: devices found on this network that the user has
                    // not looked at yet (pulse_network_design.html §06).
                    IDWriteTextFormat* fmt = compositor_->SmallFormat();
                    wchar_t num[4]{};
                    if (group.badge_count > 9) wcscpy_s(num, L"9+");
                    else num[0] = static_cast<wchar_t>(L'0' + group.badge_count);
                    const float d = 14.0f * scale_;
                    const float badge_w = group.badge_count > 9 ? d * 1.45f : d;
                    const D2D1_RECT_F b = D2D1::RectF(rail_rc.right - badge_w * 0.4f, rail_rc.top - d * 0.45f,
                                                      rail_rc.right + badge_w * 0.6f, rail_rc.top + d * 0.55f);
                    MakeBrush(dc, theme.accent, brAccent_);
                    dc->FillRoundedRectangle(D2D1::RoundedRect(b, d * 0.5f, d * 0.5f), brAccent_.get());
                    const DWRITE_TEXT_ALIGNMENT oldAlign = fmt->GetTextAlignment();
                    const DWRITE_PARAGRAPH_ALIGNMENT oldPara = fmt->GetParagraphAlignment();
                    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    MakeBrush(dc, theme.accent_text, brText_);
                    dc->DrawText(num, static_cast<UINT32>(wcslen(num)), fmt, b, brText_.get(),
                                 D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
                    fmt->SetTextAlignment(oldAlign);
                    fmt->SetParagraphAlignment(oldPara);
                }
                // Tags read as their own group on the rail: a short divider
                // above the section, and when folded the first tag colors peek
                // out of the icon's corner so the section never looks empty.
                bool tag_section = false;
                for (const auto& it : group.items) {
                    if (it.is_tag) { tag_section = true; break; }
                }
                if (tag_section && slot.run > 0) {
                    MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
                    FillRect(dc, brStrokeDivider_.get(), slot.rc.left + 10.0f * scale_,
                             slot.rc.top - 1.0f, slot.rc.right - slot.rc.left - 20.0f * scale_, 1.0f);
                }
                if (tag_section && group.collapsed && !IsHighContrast()) {
                    const float mini = 4.0f * scale_;
                    float mx = rail_rc.right + 1.0f * scale_;
                    const float my = rail_rc.bottom - 1.0f * scale_;
                    D2D1_COLOR_F halo = vm.dark ? D2D1_COLOR_F{0.125f, 0.125f, 0.125f, 1.0f}
                                                : D2D1_COLOR_F{0.97f, 0.973f, 0.957f, 1.0f};
                    int shown = 0;
                    for (const auto& it : group.items) {
                        if (!it.is_tag || it.tag_dot.a <= 0.0f) continue;
                        if (++shown > 3) break;
                        MakeBrush(dc, halo, brFillHover_);
                        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(mx, my), mini + 1.5f * scale_,
                                                      mini + 1.5f * scale_), brFillHover_.get());
                        MakeBrush(dc, it.tag_dot, brFillHover_);
                        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(mx, my), mini, mini),
                                        brFillHover_.get());
                        mx -= mini * 2.0f;
                    }
                }
                // Separator between the section row and its expanded rows.
                if (!group.collapsed) {
                    MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
                    FillRect(dc, brStrokeDivider_.get(), slot.rc.left + 8.0f * scale_,
                             slot.rc.bottom - 1.0f,
                             slot.rc.right - slot.rc.left - 16.0f * scale_, 1.0f);
                }
                continue;
            }
            if (slot.kind == SidebarSlot::RailMore) {
                // "+N": rows of this section that did not fit on the rail.
                wchar_t label[16]{};
                swprintf_s(label, L"+%d", slot.batch);
                const float cw = 30.0f * scale_;
                const float ch = 20.0f * scale_;
                const float cx = (slot.rc.left + slot.rc.right) * 0.5f;
                const float cy = (slot.rc.top + slot.rc.bottom) * 0.5f;
                const D2D1_RECT_F chip = D2D1::RectF(cx - cw * 0.5f, cy - ch * 0.5f,
                                                     cx + cw * 0.5f, cy + ch * 0.5f);
                MakeBrush(dc, theme.fill_hover, brFillSelected_);
                FillRoundedRect(dc, brFillSelected_.get(), chip.left, chip.top, cw, ch, ch * 0.5f);
                IDWriteTextFormat* fmt = compositor_->SmallFormat();
                const DWRITE_TEXT_ALIGNMENT oldAlign = fmt->GetTextAlignment();
                const DWRITE_PARAGRAPH_ALIGNMENT oldPara = fmt->GetParagraphAlignment();
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                MakeBrush(dc, theme.text_secondary, brText_);
                if (IsHighContrast() || !compositor_->DrawLumaText(label, fmt, chip, theme.text_secondary,
                                                                   theme.bg, DWRITE_TEXT_ALIGNMENT_CENTER))
                    dc->DrawText(label, static_cast<UINT32>(wcslen(label)), fmt, chip, brText_.get(),
                                 D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
                fmt->SetTextAlignment(oldAlign);
                fmt->SetParagraphAlignment(oldPara);
                continue;
            }
            if (slot.group < 0 || slot.item < 0) continue;
            const auto& item = vm.sidebar[slot.group].items[slot.item];
            const bool pill_row = isPillSlot(slot);
            const bool selected = (!pill_row && isSelectedSlot(slot)) || item.tab_active;
            const bool hovered = !pill_row && !plate_live && IsHovered(vm, HitTestResult::SidebarItem, slot.run);
            if (selected || hovered || slot.run == vm.sidebar_drop_index) {
                MakeBrush(dc, selected ? theme.fill_selected : theme.fill_hover, brFillSelected_);
                FillRoundedRect(dc, brFillSelected_.get(), slot.rc.left, slot.rc.top,
                    slot.rc.right - slot.rc.left, slot.rc.bottom - slot.rc.top,
                    theme.radius_control * scale_);
            }
            if (item.is_tag) {
                // One colored dot per tag, the same color as the wide sidebar's.
                const float r = 6.0f * scale_;
                const D2D1_POINT_2F c = D2D1::Point2F((slot.rc.left + slot.rc.right) * 0.5f,
                                                      (slot.rc.top + slot.rc.bottom) * 0.5f);
                const D2D1_COLOR_F dot_color = item.tag_dot.a > 0 ? item.tag_dot : theme.accent;
                MakeBrush(dc, dot_color, brFillHover_);
                dc->FillEllipse(D2D1::Ellipse(c, r, r), brFillHover_.get());
                continue;
            }
            const D2D1_COLOR_F iconColor = item.icon_color.a > 0 ? item.icon_color
                                         : item.tag_dot.a > 0 ? item.tag_dot : theme.text;
            const int svg_id = item.is_tag ? 0 : FluentSvgIdForGlyph(item.icon_glyph);
            const float icon = 20.0f * scale_;
            const float pad_x = (slot.rc.right - slot.rc.left - icon) * 0.5f;
            const float pad_y = (slot.rc.bottom - slot.rc.top - icon) * 0.5f;
            const D2D1_RECT_F icon_rc = D2D1::RectF(slot.rc.left + pad_x, slot.rc.top + pad_y,
                                                    slot.rc.right - pad_x, slot.rc.bottom - pad_y);
            if (IsHighContrast() || svg_id == 0 ||
                !DrawFluentSvg(svg_id, icon_rc, 1.0f, nullptr, true)) {
                DrawIconText(slot.rc.left, slot.rc.top, slot.rc.right - slot.rc.left,
                    slot.rc.bottom - slot.rc.top, item.icon_glyph, item.fallback_text,
                    iconColor, 0.92f);
            }
            if (item.tab_row && item.tab_number > 0 && item.tab_number <= 9) {
                // Rail tab icons carry their Ctrl+N number.
                IDWriteTextFormat* fmt = compositor_->SmallFormat();
                const wchar_t num[2] = {static_cast<wchar_t>(L'0' + item.tab_number), 0};
                const float d = 14.0f * scale_;
                const D2D1_RECT_F b = D2D1::RectF(icon_rc.right - d * 0.45f, icon_rc.bottom - d * 0.55f,
                                                  icon_rc.right + d * 0.55f, icon_rc.bottom + d * 0.45f);
                MakeBrush(dc, item.tab_active ? theme.accent : theme.text_secondary, brAccent_);
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((b.left + b.right) * 0.5f,
                    (b.top + b.bottom) * 0.5f), d * 0.5f, d * 0.5f), brAccent_.get());
                // Center with DirectWrite alignment on the shared format, then
                // put its alignment back for the other users.
                const DWRITE_TEXT_ALIGNMENT oldAlign = fmt->GetTextAlignment();
                const DWRITE_PARAGRAPH_ALIGNMENT oldPara = fmt->GetParagraphAlignment();
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                MakeBrush(dc, HexColor(0xFFFFFF), brText_);
                if (IsHighContrast() || !compositor_->DrawLumaText(std::wstring_view(num, 1), fmt, b,
                                                                   HexColor(0xFFFFFF), theme.bg,
                                                                   DWRITE_TEXT_ALIGNMENT_CENTER))
                    dc->DrawText(num, 1, fmt, b, brText_.get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                                 DWRITE_MEASURING_MODE_NATURAL);
                fmt->SetTextAlignment(oldAlign);
                fmt->SetParagraphAlignment(oldPara);
            }
        }
        if (label_layer) dc->PopLayer();
        dc->PopAxisAlignedClip();
        return;
    }


    // Sidebar row reorder (pulse_motion_v2 qareorder; quick-access rows, tabs
    // and tags): the held row lifts and rides the cursor while the rows it
    // passes slide aside to open its slot.
    // It paints last so the rows sliding under it never cover the card.
    std::vector<size_t> paint_order(slots.size());
    for (size_t k = 0; k < slots.size(); ++k) paint_order[k] = k;
    int pin_slot = -1;
    float pin_pitch = 0.0f;
    const bool pin_live = vm.sidebar_pin_drag_index >= 0 && sm.animations && !IsHighContrast();
    if (pin_live) {
        for (size_t k = 0; k < slots.size(); ++k) {
            const auto& slot = slots[k];
            if (slot.group < 0 || slot.item < 0 || slot.kind == SidebarSlot::Header) continue;
            if (slot.run != vm.sidebar_pin_drag_index) continue;
            pin_slot = static_cast<int>(k);
            break;
        }
    }
    if (pin_slot >= 0) {
        const auto& held = slots[static_cast<size_t>(pin_slot)];
        pin_pitch = held.rc.bottom - held.rc.top;
        if (static_cast<size_t>(pin_slot) + 1 < slots.size() &&
            slots[static_cast<size_t>(pin_slot) + 1].group == held.group &&
            slots[static_cast<size_t>(pin_slot) + 1].rc.top > held.rc.top)
            pin_pitch = slots[static_cast<size_t>(pin_slot) + 1].rc.top - held.rc.top;
        else if (pin_slot > 0 && slots[static_cast<size_t>(pin_slot) - 1].group == held.group &&
                 slots[static_cast<size_t>(pin_slot) - 1].rc.top < held.rc.top)
            pin_pitch = held.rc.top - slots[static_cast<size_t>(pin_slot) - 1].rc.top;
        paint_order.erase(paint_order.begin() + pin_slot);
        paint_order.push_back(static_cast<size_t>(pin_slot));
        if (sm.pin_run != held.run) {
            sm.pin_run = held.run;
            sm.pin_lift_start = motion_now_;
            sm.pin_offsets.clear();
            sm.pin_last = motion_now_;
        }
        const float held_c = (held.rc.top + held.rc.bottom) * 0.5f;
        const float gap = vm.sidebar_pin_gap_line_y;
        const float dt = static_cast<float>(motion_now_ > sm.pin_last ? motion_now_ - sm.pin_last : 0);
        sm.pin_last = motion_now_;
        const float follow = 1.0f - std::exp(-(std::min)(dt, 64.0f) / 55.0f);
        bool sliding = false;
        for (const auto& slot : slots) {
            if (slot.group != held.group || slot.item < 0 || slot.kind == SidebarSlot::Header ||
                slot.run == held.run) continue;
            const float c = (slot.rc.top + slot.rc.bottom) * 0.5f;
            float target = 0.0f;
            if (gap > 0.0f) {
                if (c > held_c && c < gap) target = -pin_pitch;
                else if (c < held_c && c > gap) target = pin_pitch;
            }
            float& cur = sm.pin_offsets[slot.run];
            cur += (target - cur) * follow;
            if (std::fabs(target - cur) < 0.4f) cur = target;
            else sliding = true;
        }
        const uint64_t lift_ms = motion_now_ - sm.pin_lift_start;
        if (sliding || lift_ms < static_cast<uint64_t>(motion::SpringSettleMs(motion::spring::Pop)))
            RequestMotionFrame();
    } else if (sm.pin_run >= 0) {
        sm.pin_run = -1;
        sm.pin_offsets.clear();
    }
    // Per-row transform for the reorder; restores on every exit of the body.
    struct RowXform {
        ID2D1DeviceContext* dc = nullptr;
        D2D1_MATRIX_3X2_F saved{};
        bool active = false;
        void Apply(ID2D1DeviceContext* d, const D2D1_MATRIX_3X2_F& m) {
            dc = d;
            dc->GetTransform(&saved);
            dc->SetTransform(m * saved);
            active = true;
        }
        ~RowXform() { if (active) dc->SetTransform(saved); }
    };

    ++sm.count_epoch;
    for (const size_t paint_k : paint_order) {
        const auto& slot = slots[paint_k];
        RowXform row_xform;
        if (pin_slot >= 0 && slot.group == slots[static_cast<size_t>(pin_slot)].group &&
            slot.item >= 0 && slot.kind != SidebarSlot::Header) {
            if (static_cast<int>(paint_k) == pin_slot) {
                const float t = static_cast<float>(motion_now_ - sm.pin_lift_start);
                const float lift = motion::SpringProgress(motion::spring::Pop, t);
                const float s_ = 1.0f + 0.03f * lift;
                const D2D1_POINT_2F c = D2D1::Point2F((slot.rc.left + slot.rc.right) * 0.5f,
                                                      (slot.rc.top + slot.rc.bottom) * 0.5f);
                row_xform.Apply(dc, D2D1::Matrix3x2F::Scale(s_, s_, c) *
                                    D2D1::Matrix3x2F::Translation(0.0f, vm.sidebar_pin_drag_dy));
            } else if (auto it = sm.pin_offsets.find(slot.run);
                       it != sm.pin_offsets.end() && it->second != 0.0f) {
                row_xform.Apply(dc, D2D1::Matrix3x2F::Translation(0.0f, it->second));
            }
        }
        if (slot.kind == SidebarSlot::Header) {
            fluent::SidebarSectionHeaderSpec header;
            header.bounds = slot.rc;
            header.text = vm.sidebar[slot.group].header;
            header.expanded = !vm.sidebar[slot.group].collapsed;
            header.glyph = vm.sidebar[slot.group].icon_glyph;
            const int header_svg = FluentSvgIdForGlyph(header.glyph);
            header.skip_glyph =
                !IsHighContrast() && header_svg != 0 && EnsureFluentSvg(header_svg, true);
            // The section being dragged stays lit for the whole gesture.
            const bool header_dragging = vm.sidebar_group_drag_id >= 0 &&
                vm.sidebar[slot.group].id == vm.sidebar_group_drag_id;
            // #80: on a navigable section the title is a link and lights on
            // its own; the rest of the row keeps the fold hover.
            const SidebarGroup& group = vm.sidebar[slot.group];
            const bool title_hot = group.navigable && !header_dragging &&
                IsHovered(vm, HitTestResult::SidebarHeader, slot.group, 1);
            header.state.hovered = header_dragging ||
                (!title_hot && IsHovered(vm, HitTestResult::SidebarHeader, slot.group));
            if (title_hot) {
                painter_.FillRoundedRect(
                    painter_.SidebarSectionHeaderTitleRect(slot.rc, group.header,
                                                           !group.icon_glyph.empty()),
                    6.0f * scale_, theme.fill_hover);
            }
            painter_.DrawSidebarSectionHeader(header);
            if (header.skip_glyph) {
                DrawFluentSvg(header_svg, painter_.SidebarSectionHeaderIconRect(slot.rc),
                              1.0f, nullptr, true);
            }
            if (vm.sidebar[slot.group].add_action != SidebarAddAction::None) {
                DrawIconText(slot.rc.right - 52.0f * scale_, slot.rc.top,
                    24.0f * scale_, slot.rc.bottom - slot.rc.top,
                    kIconAdd, L"+", theme.text_secondary, 0.72f);
            }
            continue;
        }
        if (slot.group < 0 || slot.item < 0) continue;

        const auto& item = vm.sidebar[slot.group].items[slot.item];
        // Motion v2 "dropbin": a row whose count changes (files dropped on the
        // 回收站, items tagged) gulps - a quick squeeze that springs back past
        // rest - under an accent wash, while its count rolls to the new value.
        float count_roll = 1.0f;
        bool count_down = false;
        const std::wstring* count_prev = nullptr;
        if (item.path == L"pulse:starred") {
            sm.star_dest = slot.rc;
            sm.star_dest_epoch = sm.count_epoch;
        }
        if (!item.path.empty() && slot.kind != SidebarSlot::Drive && !item.tab_row) {
            std::wstring now_count;
            if (item.path == L"pulse:recycle") now_count = item.detail;
            else if (item.show_count || item.count > 0) now_count = std::to_wstring((std::min)(item.count, 99));
            auto [found, fresh] = sm.counts.try_emplace(item.path);
            SidebarMotion::CountRoll& cr = found->second;
            const bool seen_last_frame = !fresh && cr.epoch + 1 == sm.count_epoch;
            if (seen_last_frame && sm.animations && cr.last != now_count &&
                !now_count.empty() && !cr.last.empty()) {
                cr.prev = cr.last;
                cr.start = motion_now_ ? motion_now_ : 1;
                cr.down = wcstol(now_count.c_str(), nullptr, 10) < wcstol(cr.last.c_str(), nullptr, 10);
            }
            cr.last = std::move(now_count);
            cr.epoch = sm.count_epoch;
            if (cr.start) {
                constexpr motion::SpringSpec kGulp{420.0f, 20.0f};  // HTML "bouncy"
                constexpr float kSqueezeMs = 90.0f;
                const float t = static_cast<float>(motion_now_ - cr.start);
                if (t >= 760.0f) {
                    cr.start = 0;
                } else {
                    count_roll = t >= static_cast<float>(motion::SpringSettleMs(motion::spring::Smooth))
                        ? 1.0f : motion::SpringProgress(motion::spring::Smooth, t);
                    count_down = cr.down;
                    count_prev = &cr.prev;
                    const float squeeze = t < kSqueezeMs
                        ? motion::EaseOutCubic(t / kSqueezeMs)
                        : 1.0f - motion::SpringProgress(kGulp, t - kSqueezeMs);
                    const float g = 1.0f - 0.07f * squeeze;
                    if (!row_xform.active) {
                        const D2D1_POINT_2F c = D2D1::Point2F((slot.rc.left + slot.rc.right) * 0.5f,
                                                              (slot.rc.top + slot.rc.bottom) * 0.5f);
                        row_xform.Apply(dc, D2D1::Matrix3x2F::Scale(g, g, c));
                    }
                    const float wash = 1.0f - t / 760.0f;
                    MakeBrush(dc, WithAlpha(theme.accent, (vm.dark ? 0.30f : 0.22f) * wash * wash), brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), slot.rc.left, slot.rc.top,
                        slot.rc.right - slot.rc.left, slot.rc.bottom - slot.rc.top,
                        theme.radius_control * scale_);
                    RequestMotionFrame();
                }
            }
        }
        fluent::ControlState state;
        state.selected = isSelectedSlot(slot);
        state.hovered = IsHovered(vm, HitTestResult::SidebarItem, slot.run) ||
            IsHovered(vm, HitTestResult::SidebarItemAction, slot.run) ||
            IsHovered(vm, HitTestResult::SidebarItemExpand, slot.run) ||
            IsHovered(vm, HitTestResult::SidebarBarSwitch, slot.run);
        // Rows of the section being reordered stay lit like its header, and the
        // dragged pin row reads as held.
        if (vm.sidebar_group_drag_id >= 0 &&
            vm.sidebar[slot.group].id == vm.sidebar_group_drag_id) state.hovered = true;
        if (vm.sidebar_pin_drag_index >= 0 && slot.run == vm.sidebar_pin_drag_index)
            state.hovered = true;
        const bool row_hot = state.hovered;
        // The gliding plate paints the hover under ordinary rows.
        if (plate_live && !(item.tab_row && item.tab_active)) state.hovered = false;
        if (item.tab_row && item.tab_active) {
            // Active tab: raised card with an accent bar, like the top strip's
            // active tab plate.
            state.selected = false;
            const float r = theme.radius_control * scale_;
            MakeBrush(dc, vm.dark ? D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 0.10f} : HexColor(0xFFFFFF), brFillSelected_);
            FillRoundedRect(dc, brFillSelected_.get(), slot.rc.left, slot.rc.top,
                slot.rc.right - slot.rc.left, slot.rc.bottom - slot.rc.top, r);
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(slot.rc.left + 0.5f, slot.rc.top + 0.5f,
                slot.rc.right - 0.5f, slot.rc.bottom - 0.5f), r, r), brStrokeCard_.get(), 1.0f);
            const float barH = (slot.rc.bottom - slot.rc.top) - 16.0f * scale_;
            MakeBrush(dc, item.tag_dot.a > 0.0f ? item.tag_dot : theme.accent, brAccent_);
            FillRoundedRect(dc, brAccent_.get(), slot.rc.left + 1.0f * scale_,
                slot.rc.top + 8.0f * scale_, 3.0f * scale_, barH, 1.5f * scale_);
        } else if (item.tab_row) {
            state.selected = false;
        }
        if (item.tab_row && item.flash > 0.0f) {
            MakeBrush(dc, WithAlpha(theme.accent, (vm.dark ? 0.34f : 0.26f) * item.flash), brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), slot.rc.left, slot.rc.top,
                slot.rc.right - slot.rc.left, slot.rc.bottom - slot.rc.top, theme.radius_control * scale_);
        }
        if (isPillSlot(slot)) {
            // The gliding pill already painted this row's background.
            state.selected = false;
            state.hovered = false;
            state.pressed = false;
        }
        if (slot.kind == SidebarSlot::Drive) {
            fluent::DriveSidebarItemSpec drive;
            drive.bounds = slot.rc;
            drive.name = item.label;
            drive.detail = item.detail;
            drive.glyph = item.icon_glyph;
            drive.capacity = item.used_ratio;
            drive.state = state;
            drive.drop_target = slot.run == vm.sidebar_drop_index;
            drive.icon_color = item.icon_color;
            if (item.danger) drive.bar_color = theme.danger;
            const int drive_svg = FluentSvgIdForGlyph(item.icon_glyph);
            drive.skip_glyph = !IsHighContrast() && drive_svg != 0 && EnsureFluentSvg(drive_svg, true);
            painter_.DrawDriveSidebarItem(drive);
            if (drive.skip_glyph) {
                DrawFluentSvg(drive_svg, painter_.DriveSidebarItemIconRect(slot.rc),
                              1.0f, nullptr, true);
            }
        } else {
            const bool heldPin = static_cast<int>(paint_k) == pin_slot;
            if (heldPin) {
                // Raised while dragging: a clearly deeper shadow + brighter card
                // than a plain selected row, so the lift reads at a glance.
                const float r = theme.radius_control * scale_;
                ComPtr<ID2D1CommandList> card;
                dc->CreateCommandList(&card);
                if (card.get()) {
                    ComPtr<ID2D1Image> prev;
                    dc->GetTarget(&prev);
                    // Record untransformed: DrawImage below applies the current
                    // transform (the held pin's lift/follow), so recording
                    // through it as well would move the card twice as far.
                    D2D1_MATRIX_3X2_F record_saved{};
                    dc->GetTransform(&record_saved);
                    dc->SetTransform(D2D1::Matrix3x2F::Identity());
                    dc->SetTarget(card.get());
                    MakeBrush(dc, vm.dark ? HexColor(0x303030) : HexColor(0xFFFFFF), brFillSelected_);
                    FillRoundedRect(dc, brFillSelected_.get(),
                        slot.rc.left + 1.0f, slot.rc.top + 1.0f,
                        slot.rc.right - slot.rc.left - 2.0f,
                        slot.rc.bottom - slot.rc.top - 2.0f, r);
                    dc->SetTarget(prev.get());
                    dc->SetTransform(record_saved);
                    card->Close();
                    ComPtr<ID2D1Effect> shadow;
                    if (SUCCEEDED(dc->CreateEffect(kShadowEffectClsid, &shadow)) && shadow.get()) {
                        shadow->SetInput(0, card.get());
                        shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 8.0f * scale_);
                        shadow->SetValue(D2D1_SHADOW_PROP_COLOR,
                            D2D1::Vector4F(0.0f, 0.0f, 0.0f, vm.dark ? 0.44f : 0.28f));
                        dc->DrawImage(shadow.get(), D2D1::Point2F(0.0f, 3.0f * scale_),
                            D2D1_INTERPOLATION_MODE_LINEAR);
                    }
                    dc->DrawImage(card.get());
                    MakeBrush(dc, theme.stroke_card, brStrokeCard_);
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(slot.rc, r, r),
                        brStrokeCard_.get(), 1.0f);
                }
            }
            if (item.divider_above) {
                // 收藏夹's separator: in the gap above the row, inset like the row.
                MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
                FillRect(dc, brStrokeDivider_.get(), slot.rc.left + 10.0f * scale_,
                         std::round(slot.rc.top - 1.0f * scale_),
                         (std::max)(0.0f, slot.rc.right - slot.rc.left - 20.0f * scale_), 1.0f);
            }
            fluent::SidebarItemSpec row;
            row.bounds = slot.rc;
            row.text = item.label;
            if (item.path == L"pulse:recycle") row.detail = item.detail;
            row.glyph = item.icon_glyph;
            row.badge_text = item.badge;
            row.badge_color = item.badge_color;
            row.custom_badge_color = !item.badge.empty() && item.badge_color.a > 0.0f;
            row.state = state;
            row.badge_count = item.count;
            row.show_count = item.show_count;
            row.drop_target = slot.run == vm.sidebar_drop_index;
            row.tag_dot = item.is_tag;
            row.tag_color = item.tag_dot;
            row.status_dot = item.status_dot;
            row.status_color = item.status_color;
            row.icon_color = item.icon_color;
            row.suppress_text = item.editing;
            if (count_prev) {
                row.count_roll = count_roll;
                row.count_prev = *count_prev;
                row.count_roll_down = count_down;
            }
            // The label is the folder's 别名 (places show it): the mark follows it.
            row.alias_mark = !item.path.empty() && !item.is_tag && !item.editing &&
                             !app::AliasStore::Instance().Empty() && app::AliasStore::Instance().ShowMark() &&
                             app::AliasStore::Instance().Shown(item.path) == item.label;
            const int row_svg = item.is_tag ? 0 : FluentSvgIdForGlyph(item.icon_glyph);
            row.skip_glyph = !IsHighContrast() && row_svg != 0 && EnsureFluentSvg(row_svg, true);
            // 网盘 roots: the program's registered icon, as Explorer's navigation pane.
            const D2D1_RECT_F row_icon_rc = painter_.SidebarItemIconRect(slot.rc, item.status_dot);
            ID2D1Bitmap* row_shell_icon = item.icon_path.empty() || IsHighContrast() ? nullptr
                : icon_cache_.ExactBitmapFor(item.icon_path,
                      (std::max)(row_icon_rc.right - row_icon_rc.left, row_icon_rc.bottom - row_icon_rc.top));
            if (row_shell_icon) row.skip_glyph = true;
            const bool unpin = SidebarItemHasUnpin(item);
            const auto unpin_rc = WorkspaceUnpinRect(slot.rc, scale_);
            const auto expand_rc = SidebarExpandRect(slot.rc, scale_);
            if (unpin)
                row.trailing_reserve = (std::max)(0.0f,
                    (slot.rc.right - unpin_rc.left) - 6.0f * scale_);
            else if (item.expandable)
                row.trailing_reserve = (std::max)(0.0f,
                    (slot.rc.right - (item.bar_switch ? SidebarBarSwitchRect(slot.rc, scale_) : expand_rc).left) -
                    4.0f * scale_);
            painter_.DrawSidebarItem(row);
            if (row_shell_icon) {
                dc->DrawBitmap(row_shell_icon, &row_icon_rc, 1.0f,
                               D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            } else if (row.skip_glyph) {
                DrawFluentSvg(row_svg, row_icon_rc, 1.0f, nullptr, true);
            }
            if (item.editing) {
                // In-place tag rename: give the hosted edit a real TextField
                // frame. Geometry must match TagRenameCell in app_main.cpp.
                D2D1_RECT_F cell = slot.rc;
                cell.left += 34.0f * scale_;
                cell.right -= 38.0f * scale_;
                cell.top += 2.0f * scale_;
                cell.bottom -= 2.0f * scale_;
                if (cell.right > cell.left && cell.bottom > cell.top) {
                    fluent::ControlState field;
                    field.focused = true;
                    painter_.DrawTextFieldFrame(cell, field);
                }
            }
            if (unpin && item.tab_row) {
                // Tab rows: close button, shown on hover and on the active tab.
                const bool close_hot = IsHovered(vm, HitTestResult::SidebarItemAction, slot.run);
                if (row_hot || item.tab_active) {
                    if (close_hot) {
                        MakeBrush(dc, theme.fill_hover, brFillHover_);
                        FillRoundedRect(dc, brFillHover_.get(), unpin_rc.left, unpin_rc.top,
                            unpin_rc.right - unpin_rc.left, unpin_rc.bottom - unpin_rc.top,
                            4.0f * scale_);
                    }
                    DrawIconText(unpin_rc.left, unpin_rc.top,
                        unpin_rc.right - unpin_rc.left, unpin_rc.bottom - unpin_rc.top,
                        kIconCloseSmall, L"x", close_hot ? theme.text : theme.text_secondary, 0.62f);
                }
            } else if (unpin) {
                const bool unpin_hot = IsHovered(vm, HitTestResult::SidebarItemAction, slot.run);
                if (unpin_hot) {
                    MakeBrush(dc, WithAlpha(theme.accent, vm.dark ? 0.22f : 0.16f), brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), unpin_rc.left, unpin_rc.top,
                        unpin_rc.right - unpin_rc.left, unpin_rc.bottom - unpin_rc.top,
                        4.0f * scale_);
                }
                DrawIconText(unpin_rc.left, unpin_rc.top,
                    unpin_rc.right - unpin_rc.left, unpin_rc.bottom - unpin_rc.top,
                             kIconPinFilled, L"P", unpin_hot ? theme.accent_hover : theme.accent, 0.72f);
            } else if (item.expandable) {
                if (item.bar_switch && row_hot) {
                    // 收藏夹 root: 改成横向收藏栏 (the same switch as Ctrl+Shift+B).
                    const D2D1_RECT_F sw = SidebarBarSwitchRect(slot.rc, scale_);
                    const bool switch_hot = IsHovered(vm, HitTestResult::SidebarBarSwitch, slot.run);
                    if (switch_hot) {
                        MakeBrush(dc, theme.fill_hover, brFillHover_);
                        FillRoundedRect(dc, brFillHover_.get(), sw.left, sw.top, sw.right - sw.left,
                                        sw.bottom - sw.top, 4.0f * scale_);
                    }
                    DrawIconText(sw.left, sw.top, sw.right - sw.left, sw.bottom - sw.top,
                                 command_icons::kGlyphTopBar, L"", switch_hot ? theme.accent : theme.text_secondary,
                                 0.68f);
                }
                const bool expand_hot = IsHovered(vm, HitTestResult::SidebarItemExpand, slot.run);
                if (expand_hot) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), expand_rc.left, expand_rc.top,
                        expand_rc.right - expand_rc.left, expand_rc.bottom - expand_rc.top,
                        4.0f * scale_);
                }
                // The chevron turns (Snappy) instead of swapping glyphs.
                const float angle = IsHighContrast() ? (item.expanded ? 90.0f : 0.0f)
                    : ChevronAngle(sm, std::to_wstring(slot.group) + L"|" + item.path, item.expanded,
                                   motion_now_, sidebar_moving);
                if (angle <= 0.01f || angle >= 89.99f) {
                    DrawIconText(expand_rc.left, expand_rc.top,
                        expand_rc.right - expand_rc.left, expand_rc.bottom - expand_rc.top,
                        item.expanded ? kIconChevronDown : kIconChevronRight,
                        item.expanded ? L"v" : L">", theme.text_secondary, 0.68f);
                } else {
                    D2D1_MATRIX_3X2_F saved{};
                    dc->GetTransform(&saved);
                    const D2D1_POINT_2F center = D2D1::Point2F((expand_rc.left + expand_rc.right) * 0.5f,
                                                               (expand_rc.top + expand_rc.bottom) * 0.5f);
                    dc->SetTransform(D2D1::Matrix3x2F::Rotation(angle, center) * saved);
                    DrawIconText(expand_rc.left, expand_rc.top,
                        expand_rc.right - expand_rc.left, expand_rc.bottom - expand_rc.top,
                        kIconChevronRight, L">", theme.text_secondary, 0.68f);
                    dc->SetTransform(saved);
                }
            }
        }
    }

    // Insertion lines for the two reorders: the dragged section and the dragged
    // quick-access pin share one indicator.
    if (vm.sidebar_group_gap_line_y > 0.0f || vm.sidebar_pin_gap_line_y > 0.0f) {
        MakeBrush(dc, theme.accent, brAccent_);
        if (vm.sidebar_group_gap_line_y > 0.0f)
            DrawSidebarInsertionLine(dc, brAccent_.get(), sb, scale_,
                                     vm.sidebar_group_gap_line_y);
        if (vm.sidebar_pin_gap_line_y > 0.0f && pin_slot < 0)
            DrawSidebarInsertionLine(dc, brAccent_.get(), sb, scale_,
                                     vm.sidebar_pin_gap_line_y);
    }

    painter_.DrawScrollbar(SidebarScrollbarSpec(vm, sb, scale_));
    if (label_layer) dc->PopLayer();
    dc->PopAxisAlignedClip();
}

void MainRenderer::LaunchStarFlight(float x, float y) {
    if (!motion::SystemAnimationsEnabled()) return;
    SidebarMotion& sm = SidebarMotionFor(this);
    sm.star_from = D2D1::Point2F(x, y);
    sm.star_start = motion::NowMs();
    sm.star_landed = false;
    RequestMotionFrame();
}

// Motion v2 "star": the star arcs from where it was clicked into the sidebar's
// 已加星标 row with a short spark trail, bursts there, and the row gulps.
void MainRenderer::DrawStarFlight(const Theme& theme) {
    SidebarMotion& sm = SidebarMotionFor(this);
    if (!sm.star_start || !compositor_ || !compositor_->Dc()) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    constexpr float kFlightMs = 560.0f;
    constexpr float kBurstMs = 360.0f;
    const uint64_t now = motion::NowMs();
    const float t = static_cast<float>(now - sm.star_start);
    if (t >= kFlightMs + kBurstMs) {
        sm.star_start = 0;
        return;
    }
    D2D1_POINT_2F to = D2D1::Point2F(28.0f * scale_, 96.0f * scale_);
    if (sm.star_dest_epoch == sm.count_epoch && sm.star_dest.right > sm.star_dest.left)
        to = D2D1::Point2F(sm.star_dest.left + 22.0f * scale_,
                           (sm.star_dest.top + sm.star_dest.bottom) * 0.5f);
    const D2D1_POINT_2F from = sm.star_from;
    const float dist = std::hypot(to.x - from.x, to.y - from.y);
    // Arc over the content, its apex kept inside the window (16 dip down):
    // the curve's peak is (from + 2*ctrl + to) / 4.
    const float apex_floor = (4.0f * 16.0f * scale_ - from.y - to.y) * 0.5f;
    const D2D1_POINT_2F ctrl = D2D1::Point2F((from.x + to.x) * 0.5f,
        (std::max)(apex_floor, (std::min)(from.y, to.y) - (std::max)(60.0f * scale_, dist * 0.22f)));
    auto at = [&](float e) {
        const float v = 1.0f - e;
        return D2D1::Point2F(v * v * from.x + 2.0f * v * e * ctrl.x + e * e * to.x,
                             v * v * from.y + 2.0f * v * e * ctrl.y + e * e * to.y);
    };
    auto ease = [](float u) {
        u = std::clamp(u, 0.0f, 1.0f);
        return u < 0.5f ? 4.0f * u * u * u : 1.0f - std::pow(-2.0f * u + 2.0f, 3.0f) * 0.5f;
    };
    D2D1_MATRIX_3X2_F saved{};
    dc->GetTransform(&saved);
    if (t < kFlightMs) {
        const float e = ease(t / kFlightMs);
        // Spark trail: a few dots sampled behind the star, smaller and fainter.
        for (int k = 1; k <= 5; ++k) {
            const float ek = e - 0.045f * static_cast<float>(k);
            if (ek <= 0.0f) break;
            const D2D1_POINT_2F q = at(ek);
            const float fade = 1.0f - static_cast<float>(k) / 6.0f;
            MakeBrush(dc, WithAlpha(theme.accent, 0.55f * fade), brAccent_);
            const float r = (2.4f * fade + 0.6f) * scale_;
            dc->FillEllipse(D2D1::Ellipse(q, r, r), brAccent_.get());
        }
        const D2D1_POINT_2F p = at(e);
        const float size = 18.0f * scale_ * (1.0f + 0.45f * std::sin(3.14159265f * e) - 0.25f * e);
        dc->SetTransform(D2D1::Matrix3x2F::Rotation(300.0f * e, p) * saved);
        DrawIconText(p.x - size * 0.5f, p.y - size * 0.5f, size, size, L"\xE735", L"*",
                     theme.accent, 1.0f);
        dc->SetTransform(saved);
    } else {
        if (!sm.star_landed) {
            sm.star_landed = true;
            // The row takes the star in: replay its gulp now that it lands.
            auto found = sm.counts.find(L"pulse:starred");
            if (found != sm.counts.end()) {
                if (found->second.prev.empty()) found->second.prev = found->second.last;
                found->second.start = now;
            }
        }
        const float b = std::clamp((t - kFlightMs) / kBurstMs, 0.0f, 1.0f);
        const float out = 1.0f - (1.0f - b) * (1.0f - b) * (1.0f - b);
        for (int k = 0; k < 8; ++k) {
            const float a = 6.2831853f * static_cast<float>(k) / 8.0f + 0.3f;
            const float rr = (5.0f + 15.0f * out) * scale_;
            const D2D1_POINT_2F q = D2D1::Point2F(to.x + std::cos(a) * rr, to.y + std::sin(a) * rr);
            MakeBrush(dc, WithAlpha(theme.accent, 0.8f * (1.0f - b)), brAccent_);
            const float r = 2.2f * (1.0f - b * 0.7f) * scale_;
            dc->FillEllipse(D2D1::Ellipse(q, r, r), brAccent_.get());
        }
        MakeBrush(dc, WithAlpha(theme.accent, 0.45f * (1.0f - b)), brAccent_);
        const float ring = (6.0f + 14.0f * out) * scale_;
        dc->DrawEllipse(D2D1::Ellipse(to, ring, ring), brAccent_.get(), 1.4f * scale_);
    }
    RequestMotionFrame();
}

float MainRenderer::SidebarMaxScroll(const WindowViewModel& vm, float window_w,
                                     float window_h) const {
    const D2D1_RECT_F sb = SidebarRect(window_w, window_h);
    const auto bar = SidebarScrollbarSpec(vm, sb, scale_);
    return bar.enabled ? std::max(0.0f, bar.content_extent - bar.viewport_extent) : 0.0f;
}

bool MainRenderer::SidebarScrollbarGeometry(const WindowViewModel& vm, float window_w,
                                            float window_h, D2D1_RECT_F& track,
                                            D2D1_RECT_F& thumb, float& max_scroll) const {
    const auto bar = SidebarScrollbarSpec(vm, SidebarRect(window_w, window_h), scale_);
    track = bar.viewport;
    thumb = fluent::ScrollbarThumbRect(bar, scale_);
    max_scroll = bar.enabled ? std::max(0.0f, bar.content_extent - bar.viewport_extent) : 0.0f;
    return max_scroll > 0.0f && thumb.bottom > thumb.top;
}

} // namespace pulse::ui