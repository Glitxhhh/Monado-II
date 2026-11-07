// [XCDE] LINUX-015: presentation only; no configuration or process ownership.
// Dear ImGui's MIT-licensed checkbox layout is adapted below solely to wrap
// long labels while retaining its ID, hitbox, navigation and edit semantics.
#pragma once
#include "imgui_internal.h"
#include <cmath>
#include <string>
#ifdef MONADO_LAUNCHER_RESOURCES
#include "launcher_fonts.inc"
#endif
#ifdef MONADO_LAUNCHER_ARTWORK
#include "launcher_art.inc"
#endif

namespace launcher_theme {
static ImFont *heading_font, *brand_font;
static ImVec4 color(unsigned rgb, float alpha = 1) {
    return ImVec4(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, alpha);
}
static ImU32 ink(unsigned rgb, float alpha = 1) { return ImGui::ColorConvertFloat4ToU32(color(rgb, alpha)); }
static bool fonts(ImGuiIO &io, float scale) {
#ifdef MONADO_LAUNCHER_RESOURCES
    ImFontConfig cfg; cfg.FontDataOwnedByAtlas = false;
    ImFont *body = io.Fonts->AddFontFromMemoryTTF((void *)monado_body_ttf, sizeof monado_body_ttf, 16 * scale, &cfg);
    heading_font = io.Fonts->AddFontFromMemoryTTF((void *)monado_heading_ttf, sizeof monado_heading_ttf, 20 * scale, &cfg);
    brand_font = io.Fonts->AddFontFromMemoryTTF((void *)monado_brand_ttf, sizeof monado_brand_ttf, 44 * scale, &cfg);
    return body != nullptr;
#else
    (void)io; (void)scale; return false;
#endif
}
static SDL_Texture *artwork(SDL_Renderer *renderer) {
#ifdef MONADO_LAUNCHER_ARTWORK
    SDL_Surface *surface = SDL_LoadPNG_IO(SDL_IOFromConstMem(monado_art_png, sizeof monado_art_png), true);
    if (!surface) return nullptr;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    return texture;
#else
    (void)renderer; return nullptr;
#endif
}
static void style(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(18, 16); s.WindowRounding = 0;
    s.FramePadding = ImVec2(10, 6); s.ItemSpacing = ImVec2(10, 9);
    s.ItemInnerSpacing = ImVec2(9, 5); s.CellPadding = ImVec2(8, 6);
    s.FrameRounding = 3; s.FrameBorderSize = 1;
    s.ChildRounding = 3; s.ChildBorderSize = 0;
    s.PopupRounding = 4; s.PopupBorderSize = 1;
    s.TabRounding = 3; s.TabBorderSize = 1; s.SeparatorTextBorderSize = 2;
    s.ScrollbarSize = 12; s.ScrollbarRounding = 4; s.GrabRounding = 2;
    s.DisabledAlpha = .65f;
    s.Colors[ImGuiCol_Text] = color(0xF4F5F3);
    s.Colors[ImGuiCol_TextDisabled] = color(0xB5C0CC);
    s.Colors[ImGuiCol_WindowBg] = color(0x0A1420);
    s.Colors[ImGuiCol_ChildBg] = color(0x101E2B, .98f);
    s.Colors[ImGuiCol_PopupBg] = color(0x101E2B);
    s.Colors[ImGuiCol_Border] = color(0x385363);
    s.Colors[ImGuiCol_BorderShadow] = color(0, .18f);
    s.Colors[ImGuiCol_FrameBg] = color(0x1A2C3C);
    s.Colors[ImGuiCol_FrameBgHovered] = color(0x29475A);
    s.Colors[ImGuiCol_FrameBgActive] = color(0x33596B);
    s.Colors[ImGuiCol_Button] = color(0x1A2C3C);
    s.Colors[ImGuiCol_ButtonHovered] = color(0x2E4F63);
    s.Colors[ImGuiCol_ButtonActive] = color(0x3B687E);
    s.Colors[ImGuiCol_CheckMark] = color(0x8CE6F5);
    s.Colors[ImGuiCol_SliderGrab] = color(0x72CDE5);
    s.Colors[ImGuiCol_SliderGrabActive] = color(0xB5EFFA);
    s.Colors[ImGuiCol_Header] = color(0x25465A);
    s.Colors[ImGuiCol_HeaderHovered] = color(0x345E74);
    s.Colors[ImGuiCol_HeaderActive] = color(0x416F86);
    s.Colors[ImGuiCol_Tab] = color(0x1A2C3C);
    s.Colors[ImGuiCol_TabHovered] = color(0x9F1D3E);
    s.Colors[ImGuiCol_TabSelected] = color(0xC91542);
    s.Colors[ImGuiCol_TabSelectedOverline] = color(0xF35C7D);
    s.Colors[ImGuiCol_TabDimmed] = color(0x142432);
    s.Colors[ImGuiCol_TabDimmedSelected] = color(0x671B32);
    s.Colors[ImGuiCol_TabDimmedSelectedOverline] = color(0xC91542);
    s.Colors[ImGuiCol_Separator] = color(0x385363);
    s.Colors[ImGuiCol_SeparatorHovered] = color(0x72CDE5);
    s.Colors[ImGuiCol_SeparatorActive] = color(0xB5EFFA);
    s.Colors[ImGuiCol_ResizeGrip] = color(0x72CDE5, .25f);
    s.Colors[ImGuiCol_ResizeGripHovered] = color(0x72CDE5, .65f);
    s.Colors[ImGuiCol_ResizeGripActive] = color(0x72CDE5);
    s.Colors[ImGuiCol_ScrollbarBg] = color(0x08131D);
    s.Colors[ImGuiCol_ScrollbarGrab] = color(0x526878);
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = color(0x7895A8);
    s.Colors[ImGuiCol_ScrollbarGrabActive] = color(0x9CBBCC);
    s.Colors[ImGuiCol_TableHeaderBg] = color(0x203647);
    s.Colors[ImGuiCol_TableBorderStrong] = color(0x385363);
    s.Colors[ImGuiCol_TableBorderLight] = color(0x263E4D);
    s.Colors[ImGuiCol_TableRowBg] = color(0x101E2B, 0);
    s.Colors[ImGuiCol_TableRowBgAlt] = color(0x294354, .32f);
    s.Colors[ImGuiCol_TextSelectedBg] = color(0x407D95, .7f);
    s.Colors[ImGuiCol_NavCursor] = color(0x72CDE5);
    s.ScaleAllSizes(scale);
}
static void section(const char *label) {
    if (heading_font) ImGui::PushFont(heading_font);
    ImGui::PushStyleColor(ImGuiCol_Separator, color(0x72CDE5, .8f));
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor();
    if (heading_font) ImGui::PopFont();
}
static bool combo(const char *label, int *value, const char *const *items, int count) {
    const float available = ImGui::GetContentRegionAvail().x;
    const float width = ImGui::CalcItemWidth();
    const float text = ImGui::CalcTextSize(label, nullptr, true).x;
    if (width + text + ImGui::GetStyle().ItemInnerSpacing.x > available) {
        ImGui::TextWrapped("%s", label);
        ImGui::SetNextItemWidth(available);
        // Empty strings hash to their seed. Keep the exact original widget and
        // popup IDs, with the original live label laid out above the input.
        const ImGuiID id = ImGui::GetID(label);
        ImGui::PushOverrideID(id);
        const bool changed = ImGui::Combo("", value, items, count);
        ImGui::PopID();
        ImGuiContext &g = *GImGui;
        IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags);
        return changed;
    }
    return ImGui::Combo(label, value, items, count);
}
static bool checkbox(const char *label, bool *value) {
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    const ImGuiStyle &s = ImGui::GetStyle();
    const float square = ImGui::GetFrameHeight();
    const float wrap = ImMax(1.f, ImGui::GetContentRegionAvail().x - square - s.ItemInnerSpacing.x);
    if (ImGui::CalcTextSize(label, nullptr, true).x <= wrap) return ImGui::Checkbox(label, value);
    if (window->SkipItems) return false;
    // Stock 1.91.9 checkbox behavior; only text measurement/rendering wraps.
    ImGuiContext &g = *GImGui;
    const ImGuiID id = window->GetID(label);
    const ImVec2 text = ImGui::CalcTextSize(label, nullptr, true, wrap);
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + square + s.ItemInnerSpacing.x + text.x,
                               pos.y + text.y + s.FramePadding.y * 2));
    ImGui::ItemSize(bb, s.FramePadding.y);
    const bool visible = ImGui::ItemAdd(bb, id);
    const bool multi = (g.LastItemData.ItemFlags & ImGuiItemFlags_IsMultiSelect) != 0;
    if (!visible && (!multi || !g.BoxSelectState.UnclipMode || !g.BoxSelectState.UnclipRect.Overlaps(bb))) {
        IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags | ImGuiItemStatusFlags_Checkable | (*value ? ImGuiItemStatusFlags_Checked : 0));
        return false;
    }
    bool checked = *value, hovered, held;
    if (multi) ImGui::MultiSelectItemHeader(id, &checked, nullptr);
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (multi) ImGui::MultiSelectItemFooter(id, &checked, &pressed);
    else if (pressed) checked = !checked;
    if (*value != checked) { *value = checked; pressed = true; ImGui::MarkItemEdited(id); }
    const ImRect check(pos, ImVec2(pos.x + square, pos.y + square));
    const bool mixed = (g.LastItemData.ItemFlags & ImGuiItemFlags_MixedValue) != 0;
    if (visible) {
        ImGui::RenderNavCursor(bb, id);
        ImGui::RenderFrame(check.Min, check.Max, ImGui::GetColorU32(held && hovered ? ImGuiCol_FrameBgActive : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), true, s.FrameRounding);
        const ImU32 check_color = ImGui::GetColorU32(ImGuiCol_CheckMark);
        if (mixed) {
            const float pad = ImMax(1.f, IM_TRUNC(square / 3.6f));
            window->DrawList->AddRectFilled(ImVec2(pos.x + pad, pos.y + pad), ImVec2(check.Max.x - pad, check.Max.y - pad), check_color, s.FrameRounding);
        } else if (*value) {
            const float pad = ImMax(1.f, IM_TRUNC(square / 6.f));
            ImGui::RenderCheckMark(window->DrawList, ImVec2(pos.x + pad, pos.y + pad), check_color, square - pad * 2);
        }
    }
    const ImVec2 label_pos(check.Max.x + s.ItemInnerSpacing.x, pos.y + s.FramePadding.y);
    if (g.LogEnabled) ImGui::LogRenderedText(&label_pos, mixed ? "[~]" : *value ? "[x]" : "[ ]");
    if (visible) ImGui::RenderTextWrapped(label_pos, label, ImGui::FindRenderedTextEnd(label), wrap);
    IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags | ImGuiItemStatusFlags_Checkable | (*value ? ImGuiItemStatusFlags_Checked : 0));
    return pressed;
}
// A same-line row is decorative; wrap it before it can push a widget offscreen.
static void beside(float required) {
    if (ImGui::GetCurrentWindow()->WorkRect.Max.x - ImGui::GetItemRectMax().x - ImGui::GetStyle().ItemSpacing.x > required) ImGui::SameLine();
}
struct Layout {
    ImVec2 origin, size;
    float scale, panel_x, panel_width, header, footer_top, content_bottom;
    ImVec2 buttons[5], button_sizes[5];
    float actions_bottom;
};
static Layout layout(ImVec2 origin, ImVec2 size, float scale, const char *const labels[5], bool precompiling) {
    Layout l{}; l.origin = origin; l.size = size; l.scale = scale;
    const float gap = 10 * scale, margin = 18 * scale;
    l.header = (size.x < 760 * scale ? 174 : 122) * scale;
    l.panel_x = size.x >= 1180 * scale ? size.x * .28f : margin;
    l.panel_width = size.x - l.panel_x - margin;
    // Keep the original submission order, but place PLAY at the far right.
    const int order[] = {1, 2, 3, 4, 0};
    float x = 0, y = 0;
    const float height = ImGui::GetFrameHeight() + 8 * scale;
    for (int index : order) {
        const float desired = ImGui::CalcTextSize(labels[index]).x + 24 * scale;
        const float width = ImMin(l.panel_width - 32 * scale, ImMax(desired, (index == 0 ? 150 : 0) * scale));
        if (x > 0 && x + width > l.panel_width - 32 * scale) { x = 0; y += height + gap; }
        l.buttons[index] = ImVec2(x, y); l.button_sizes[index] = ImVec2(width, height);
        x += width + gap;
    }
    // Right-align the complete last row, making PLAY the rightmost action.
    const float shift = ImMax(0.f, l.panel_width - 32 * scale - (x - gap));
    for (int i = 0; i < 5; i++) if (l.buttons[i].y == y) l.buttons[i].x += shift;
    const float actions_height = y + height;
    const float status_height = ImGui::GetTextLineHeight() * 2 + 10 * scale;
    const float progress = precompiling ? ImGui::GetFrameHeightWithSpacing() : 0;
    l.footer_top = size.y - margin - actions_height - status_height - progress - 24 * scale;
    l.content_bottom = l.footer_top - 12 * scale;
    for (int i = 0; i < 5; i++) {
        l.buttons[i].x += origin.x + l.panel_x + 16 * scale;
        l.buttons[i].y += origin.y + l.footer_top + 12 * scale + status_height + progress;
    }
    l.actions_bottom = origin.y + size.y - margin - 12 * scale;
    return l;
}
static void mark(ImDrawList *draw, ImVec2 center, float radius, ImU32 shade) {
    // Monado II publication branding: an authored Roman numeral, no game icon.
    for (int i = 0; i < 2; i++) {
        const float x = i == 0 ? -0.35f : 0.35f;
        draw->AddLine(ImVec2(center.x + radius * x, center.y - radius),
                      ImVec2(center.x + radius * x, center.y + radius), shade, 3);
    }
}
// Use SDL's cached sprite path for the illustration. The software renderer
// does not reliably draw this image through ImGui's indexed geometry batches.
static void render_art(SDL_Renderer *renderer, SDL_Texture *art) {
    if (!art) return;
    float w = 0, h = 0;
    int output_w = 0, output_h = 0;
    if (!SDL_GetTextureSize(art, &w, &h) || !SDL_GetRenderOutputSize(renderer, &output_w, &output_h) ||
        w <= 0 || h <= 0 || output_w <= 0 || output_h <= 0) return;
    SDL_FRect source{0, 0, w, h};
    const float aspect = (float)output_w / output_h;
    if (aspect > w / h) { source.h = w / aspect; source.y = (h - source.h) * .5f; }
    else source.w = h * aspect; // Optional local illustration stays left-anchored.
    SDL_RenderTexture(renderer, art, &source, nullptr);
}
static void backdrop(const Layout &l) {
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    const ImVec2 end(l.origin.x + l.size.x, l.origin.y + l.size.y);
    // A static scrim keeps branding readable; no animation/repaint timer.
    draw->AddRectFilledMultiColor(l.origin, ImVec2(end.x, l.origin.y + l.header),
        ink(0x07121D, .88f), ink(0x07121D, .94f), ink(0x07121D, .72f), ink(0x07121D, .22f));
    mark(draw, ImVec2(l.origin.x + l.size.x * .61f, l.origin.y + 40 * l.scale), 120 * l.scale, ink(0x72CDE5, .13f));
    const ImVec2 panel(l.origin.x + l.panel_x, l.origin.y + l.header);
    draw->AddRectFilled(ImVec2(panel.x + 5 * l.scale, panel.y + 5 * l.scale), ImVec2(end.x - 13 * l.scale, end.y - 13 * l.scale), ink(0, .35f), 4 * l.scale);
    draw->AddRectFilled(panel, ImVec2(end.x - 18 * l.scale, end.y - 18 * l.scale), ink(0x101E2B, .98f), 3 * l.scale);
    draw->AddRect(panel, ImVec2(end.x - 18 * l.scale, end.y - 18 * l.scale), ink(0x385363), 3 * l.scale);
    draw->AddLine(panel, ImVec2(end.x - 18 * l.scale, panel.y), ink(0xC91542), 2 * l.scale);
    draw->AddLine(ImVec2(panel.x, l.origin.y + l.footer_top), ImVec2(end.x - 18 * l.scale, l.origin.y + l.footer_top), ink(0xC91542, .8f), 2 * l.scale);
}
static void branding(const Layout &l, const char *subtitle) {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float s = l.scale;
    ImFont *brand = brand_font ? brand_font : ImGui::GetFont();
    const float brand_size = (l.size.x < 760 * s ? 36 : 44) * s;
    const ImVec2 pos(l.origin.x + 24 * s, l.origin.y + 12 * s);
    draw->AddText(brand, brand_size, ImVec2(pos.x + s, pos.y + 2 * s), ink(0, .5f), "MONADO II");
    draw->AddText(brand, brand_size, pos, ink(0xF4F5F3), "MONADO II");
    const float width = brand->CalcTextSizeA(brand_size, 10000, 0, "MONADO II").x;
    mark(draw, ImVec2(pos.x + width + 27 * s, pos.y + brand_size * .62f), 18 * s, ink(0xE32954));
    ImFont *heading = heading_font ? heading_font : ImGui::GetFont();
    draw->AddText(heading, 18 * s, ImVec2(l.origin.x + 24 * s, l.origin.y + 68 * s), ink(0xF4F5F3), "Xenoblade Chronicles: Definitive Edition", nullptr, l.size.x - 48 * s);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(l.origin.x + 24 * s, l.origin.y + 94 * s), ink(0xCCD8E3), subtitle);
}
static void language_position(const Layout &l, const char *label) {
    const float width = ImGui::CalcTextSize(label).x + 145 * l.scale;
    ImGui::SetCursorScreenPos(ImVec2(l.origin.x + (l.size.x < 760 * l.scale ? 24 * l.scale : l.size.x - width - 26 * l.scale),
                                   l.origin.y + (l.size.x < 760 * l.scale ? 130 : 24) * l.scale));
    ImGui::SetNextItemWidth(130 * l.scale);
}
static void footer_button(const Layout &l, int index) { ImGui::SetCursorScreenPos(l.buttons[index]); }
} // namespace launcher_theme
