#include "menu/draw.h"
#include "menu/menu.h"
#include "menu/style.h"

namespace nl {
namespace {

using namespace nl::style;

ImVec4 to_vec4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

} // namespace

void fade_vertices(ImDrawList* dl, int from, float alpha, float dy)
{
    for (int i = from; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        v.col = draw::with_alpha(v.col, alpha);
        v.pos.y += dy;
    }
}

void apply_imgui_style()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(0.0f, 0.0f);
    s.WindowBorderSize = 0.0f;
    s.WindowRounding = layout::rounding;
    s.PopupRounding = 6.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameRounding = 4.0f;
    s.FrameBorderSize = 0.0f;
    s.GrabRounding = 4.0f;
    s.ItemSpacing = ImVec2(0.0f, 0.0f);
    s.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    s.ScaleAllSizes(ui_scale);
    s.FontScaleDpi = ui_scale;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = to_vec4(col::label);
    c[ImGuiCol_TextDisabled] = to_vec4(col::section_header);
    c[ImGuiCol_WindowBg] = to_vec4(col::main_bg);
    c[ImGuiCol_PopupBg] = to_vec4(col::popup_bg);
    c[ImGuiCol_Border] = to_vec4(col::popup_border);
    c[ImGuiCol_FrameBg] = to_vec4(col::combo_bg);
    c[ImGuiCol_FrameBgHovered] = to_vec4(col::combo_bg_hovered);
    c[ImGuiCol_FrameBgActive] = to_vec4(col::combo_bg_hovered);
    c[ImGuiCol_Button] = to_vec4(col::combo_bg);
    c[ImGuiCol_ButtonHovered] = to_vec4(col::combo_bg_hovered);
    c[ImGuiCol_ButtonActive] = to_vec4(col::popup_item_hover);
    c[ImGuiCol_CheckMark] = to_vec4(col::accent);
    c[ImGuiCol_SliderGrab] = to_vec4(col::accent);
    c[ImGuiCol_SliderGrabActive] = to_vec4(col::toggle_on_knob);
    c[ImGuiCol_Header] = to_vec4(col::popup_item_hover);
    c[ImGuiCol_HeaderHovered] = to_vec4(col::popup_item_hover);
    c[ImGuiCol_HeaderActive] = to_vec4(col::tab_bg_active);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.70f, 0.84f, 1.0f, 0.30f);
    c[ImGuiCol_NavCursor] = to_vec4(col::accent);
}

} // namespace nl
