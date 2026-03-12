#pragma once
#include "imgui.h"
#include "json.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>

void EnableDarkModeWin(bool enable);
#ifndef _WIN32
inline void EnableDarkModeWin(bool) {}
#endif

namespace ImGuiTheme {

    inline ImVec4 Hex(unsigned int hex, float alpha = 1.0f) {
        float r = ((hex >> 16) & 0xFF) / 255.0f;
        float g = ((hex >> 8) & 0xFF) / 255.0f;
        float b = ((hex >> 0) & 0xFF) / 255.0f;
        return ImVec4(r, g, b, alpha);
    }

    inline std::string ImVec4ToHex(const ImVec4& color) {
        std::stringstream ss;
        int r = static_cast<int>(std::round(color.x * 255.0f));
        int g = static_cast<int>(std::round(color.y * 255.0f));
        int b = static_cast<int>(std::round(color.z * 255.0f));

        ss << std::uppercase << std::hex << std::setfill('0')
            << std::setw(2) << r
            << std::setw(2) << g
            << std::setw(2) << b;

        return ("#" + ss.str());
    }

    inline void ApplyImGuiLight() {

        EnableDarkModeWin(false);
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 8.0f;
        style.FrameRounding = 5.0f;
        style.GrabRounding = 5.0f;
        style.PopupRounding = 5.0f;
        style.ScrollbarRounding = 5.0f;
        style.ChildRounding = 8.0f;
        style.TabRounding = 5.0f;

        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.TabBorderSize = 0.0f;

        style.WindowPadding = ImVec2(8, 8);
        style.FramePadding = ImVec2(6, 4);
        style.ItemSpacing = ImVec2(8, 6);
        style.ScrollbarSize = 14.0f;

        style.AntiAliasedLines = true;
        style.AntiAliasedLinesUseTex = true;
        style.AntiAliasedFill = true;

        ImGui::StyleColorsLight();
	}
    inline void ApplyImGuiDark() {
        EnableDarkModeWin(true);
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 8.0f;
        style.FrameRounding = 5.0f;
        style.GrabRounding = 5.0f;
        style.PopupRounding = 5.0f;
        style.ScrollbarRounding = 5.0f;
        style.ChildRounding = 8.0f;
        style.TabRounding = 5.0f;

        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.TabBorderSize = 0.0f;

        style.WindowPadding = ImVec2(8, 8);
        style.FramePadding = ImVec2(6, 4);
        style.ItemSpacing = ImVec2(8, 6);
        style.ScrollbarSize = 14.0f;

        style.AntiAliasedLines = true;
        style.AntiAliasedLinesUseTex = true;
        style.AntiAliasedFill = true;

        ImGui::StyleColorsDark();
    }
    inline void ApplyImGuiClassic() {
        EnableDarkModeWin(true);
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 8.0f;
        style.FrameRounding = 5.0f;
        style.GrabRounding = 5.0f;
        style.PopupRounding = 5.0f;
        style.ScrollbarRounding = 5.0f;
        style.ChildRounding = 8.0f;
        style.TabRounding = 5.0f;

        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.TabBorderSize = 0.0f;

        style.WindowPadding = ImVec2(8, 8);
        style.FramePadding = ImVec2(6, 4);
        style.ItemSpacing = ImVec2(8, 6);
        style.ScrollbarSize = 14.0f;

        style.AntiAliasedLines = true;
        style.AntiAliasedLinesUseTex = true;
        style.AntiAliasedFill = true;

        ImGui::StyleColorsClassic();
    }

    inline void ApplyWindows11(bool dark_mode) {
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;

        style.WindowRounding = 6.0f;
        style.ChildRounding = 6.0f;
        style.FrameRounding = 4.0f;
        style.GrabRounding = 3.0f;
        style.PopupRounding = 4.0f;
        style.ScrollbarRounding = 8.0f;
        style.TabRounding = 4.0f;

        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 1.0f;
        style.TabBorderSize = 0.0f;

        style.WindowPadding = ImVec2(8, 8);
        style.FramePadding = ImVec2(6, 4);
        style.ItemSpacing = ImVec2(8, 6);
        style.ScrollbarSize = 14.0f;

        if (dark_mode) {
            EnableDarkModeWin(true);
            ImVec4 bg_base = Hex(0x1E1E1E);
            ImVec4 bg_child = Hex(0x282828);
            ImVec4 bg_popup = Hex(0x2A2A2A);
            ImVec4 border = Hex(0x4A4A4A);
            ImVec4 accent = Hex(0x4CC2FF);
            ImVec4 text = Hex(0xFFFFFF);

            colors[ImGuiCol_Text] = text;
            colors[ImGuiCol_TextDisabled] = Hex(0xA0A0A0);
            colors[ImGuiCol_WindowBg] = bg_base;
            colors[ImGuiCol_ChildBg] = bg_child;
            colors[ImGuiCol_PopupBg] = bg_popup;
            colors[ImGuiCol_Border] = border;
            colors[ImGuiCol_BorderShadow] = Hex(0x000000, 0.0f);

            colors[ImGuiCol_FrameBg] = Hex(0x333333);
            colors[ImGuiCol_FrameBgHovered] = Hex(0x3E3E3E);
            colors[ImGuiCol_FrameBgActive] = Hex(0x454545);

            colors[ImGuiCol_TitleBg] = bg_base;
            colors[ImGuiCol_TitleBgActive] = bg_base;
            colors[ImGuiCol_TitleBgCollapsed] = Hex(0x1E1E1E, 0.75f);
            colors[ImGuiCol_MenuBarBg] = Hex(0x1B1B1B);

            colors[ImGuiCol_Header] = Hex(0x383838);
            colors[ImGuiCol_HeaderHovered] = Hex(0x404040);
            colors[ImGuiCol_HeaderActive] = Hex(0x4D4D4D);

            colors[ImGuiCol_Separator] = border;
            colors[ImGuiCol_SeparatorHovered] = accent;
            colors[ImGuiCol_SeparatorActive] = Hex(0x76D6FF);

            colors[ImGuiCol_ResizeGrip] = Hex(0x333333);
            colors[ImGuiCol_ResizeGripHovered] = accent;
            colors[ImGuiCol_ResizeGripActive] = Hex(0x76D6FF);

            colors[ImGuiCol_InputTextCursor] = text;

            colors[ImGuiCol_Tab] = Hex(0x222222);
            colors[ImGuiCol_TabHovered] = Hex(0x343434);
            colors[ImGuiCol_TabSelected] = bg_child;
            colors[ImGuiCol_TabSelectedOverline] = accent;
            colors[ImGuiCol_TabDimmed] = Hex(0x222222);
            colors[ImGuiCol_TabDimmedSelected] = bg_child;
            colors[ImGuiCol_TabDimmedSelectedOverline] = Hex(0x4A4A4A);

            colors[ImGuiCol_DockingPreview] = Hex(0x4CC2FF, 0.70f);
            colors[ImGuiCol_DockingEmptyBg] = Hex(0x1A1A1A);

            colors[ImGuiCol_Button] = Hex(0x333333);
            colors[ImGuiCol_ButtonHovered] = Hex(0x3D3D3D);
            colors[ImGuiCol_ButtonActive] = Hex(0x454545);

            colors[ImGuiCol_CheckMark] = accent;
            colors[ImGuiCol_SliderGrab] = accent;
            colors[ImGuiCol_SliderGrabActive] = Hex(0x76D6FF);
            colors[ImGuiCol_DragDropTarget] = accent;
            colors[ImGuiCol_DragDropTargetBg] = Hex(0x4CC2FF, 0.12f);
            colors[ImGuiCol_TreeLines] = Hex(0x4A4A4A);
            colors[ImGuiCol_UnsavedMarker] = Hex(0xFFD633);

            colors[ImGuiCol_PlotLines] = Hex(0x9CDCFE);
            colors[ImGuiCol_PlotLinesHovered] = Hex(0xFF6040);
            colors[ImGuiCol_PlotHistogram] = Hex(0xE5A00D);
            colors[ImGuiCol_PlotHistogramHovered] = Hex(0xFF9A1A);

            colors[ImGuiCol_TableHeaderBg] = Hex(0x303030);
            colors[ImGuiCol_TableBorderStrong] = Hex(0x4A4A4A);
            colors[ImGuiCol_TableBorderLight] = Hex(0x3A3A3A);
            colors[ImGuiCol_TableRowBg] = Hex(0x000000, 0.0f);
            colors[ImGuiCol_TableRowBgAlt] = Hex(0xFFFFFF, 0.03f);

            colors[ImGuiCol_TextLink] = accent;
            colors[ImGuiCol_TextSelectedBg] = Hex(0x4CC2FF, 0.35f);
            colors[ImGuiCol_NavCursor] = accent;
            colors[ImGuiCol_NavWindowingHighlight] = Hex(0xFFFFFF, 0.70f);
            colors[ImGuiCol_NavWindowingDimBg] = Hex(0x000000, 0.20f);
            colors[ImGuiCol_ModalWindowDimBg] = Hex(0x000000, 0.55f);

            colors[ImGuiCol_ScrollbarBg] = Hex(0x000000, 0.06f);
            colors[ImGuiCol_ScrollbarGrab] = Hex(0x3F3F3F);
            colors[ImGuiCol_ScrollbarGrabHovered] = Hex(0x4C4C4C);
            colors[ImGuiCol_ScrollbarGrabActive] = Hex(0x5A5A5A);
        }
        else {
            EnableDarkModeWin(false);
            ImVec4 bg_base = Hex(0xF3F3F3);
            ImVec4 bg_child = Hex(0xFFFFFF);
            ImVec4 border = Hex(0xE5E5E5);
            ImVec4 border_str = Hex(0xD0D0D0);
            ImVec4 accent = Hex(0x0067C0);
            ImVec4 text = Hex(0x000000);

            colors[ImGuiCol_Text] = text;
            colors[ImGuiCol_TextDisabled] = Hex(0x808080);
            colors[ImGuiCol_WindowBg] = bg_base;
            colors[ImGuiCol_ChildBg] = bg_child;
            colors[ImGuiCol_PopupBg] = Hex(0xFFFFFF);

            colors[ImGuiCol_Border] = border;
            colors[ImGuiCol_BorderShadow] = Hex(0x000000, 0.0f);

            colors[ImGuiCol_FrameBg] = Hex(0xFFFFFF);
            colors[ImGuiCol_FrameBgHovered] = Hex(0xF7F7F7);
            colors[ImGuiCol_FrameBgActive] = Hex(0xEBEBEB);

            colors[ImGuiCol_TitleBg] = bg_base;
            colors[ImGuiCol_TitleBgActive] = bg_base;
            colors[ImGuiCol_TitleBgCollapsed] = Hex(0xF3F3F3, 0.75f);
            colors[ImGuiCol_MenuBarBg] = Hex(0xECECEC);

            colors[ImGuiCol_Header] = Hex(0xECECEC);
            colors[ImGuiCol_HeaderHovered] = Hex(0xE0E0E0);
            colors[ImGuiCol_HeaderActive] = Hex(0xD6D6D6);

            colors[ImGuiCol_Separator] = border;
            colors[ImGuiCol_SeparatorHovered] = accent;
            colors[ImGuiCol_SeparatorActive] = Hex(0x005A9E);

            colors[ImGuiCol_ResizeGrip] = Hex(0xE5E5E5);
            colors[ImGuiCol_ResizeGripHovered] = accent;
            colors[ImGuiCol_ResizeGripActive] = Hex(0x005A9E);

            colors[ImGuiCol_InputTextCursor] = text;

            colors[ImGuiCol_Tab] = Hex(0xF3F3F3);
            colors[ImGuiCol_TabHovered] = Hex(0xE6E6E6);
            colors[ImGuiCol_TabSelected] = Hex(0xFFFFFF);
            colors[ImGuiCol_TabSelectedOverline] = accent;
            colors[ImGuiCol_TabDimmed] = Hex(0xF3F3F3);
            colors[ImGuiCol_TabDimmedSelected] = Hex(0xFFFFFF);
            colors[ImGuiCol_TabDimmedSelectedOverline] = Hex(0xD0D0D0);

            colors[ImGuiCol_DockingPreview] = Hex(0x0067C0, 0.70f);
            colors[ImGuiCol_DockingEmptyBg] = Hex(0xF0F0F0);

            colors[ImGuiCol_Button] = Hex(0xFFFFFF);
            colors[ImGuiCol_ButtonHovered] = Hex(0xFBFBFB);
            colors[ImGuiCol_ButtonActive] = Hex(0xF0F0F0);

            colors[ImGuiCol_CheckMark] = accent;
            colors[ImGuiCol_SliderGrab] = Hex(0x666666);
            colors[ImGuiCol_SliderGrabActive] = accent;

            colors[ImGuiCol_TableHeaderBg] = Hex(0xECECEC);
            colors[ImGuiCol_TableBorderStrong] = Hex(0xD0D0D0);
            colors[ImGuiCol_TableBorderLight] = Hex(0xE0E0E0);
            colors[ImGuiCol_TableRowBg] = Hex(0x000000, 0.0f);
            colors[ImGuiCol_TableRowBgAlt] = Hex(0x000000, 0.03f);

            colors[ImGuiCol_TextLink] = accent;
            colors[ImGuiCol_TextSelectedBg] = Hex(0x0067C0, 0.25f);
            colors[ImGuiCol_NavCursor] = accent;
            colors[ImGuiCol_NavWindowingHighlight] = Hex(0xFFFFFF, 0.70f);
            colors[ImGuiCol_NavWindowingDimBg] = Hex(0x000000, 0.20f);
            colors[ImGuiCol_ModalWindowDimBg] = Hex(0x000000, 0.40f);

            colors[ImGuiCol_DragDropTarget] = accent;
            colors[ImGuiCol_DragDropTargetBg] = Hex(0x0067C0, 0.10f);
            colors[ImGuiCol_TreeLines] = Hex(0xD0D0D0);
            colors[ImGuiCol_UnsavedMarker] = Hex(0xD4A017);

            colors[ImGuiCol_PlotLines] = Hex(0x0067C0);
            colors[ImGuiCol_PlotLinesHovered] = Hex(0xD03030);
            colors[ImGuiCol_PlotHistogram] = Hex(0xC77C00);
            colors[ImGuiCol_PlotHistogramHovered] = Hex(0xE5A00D);

            style.FrameBorderSize = 1.0f;
            colors[ImGuiCol_Border] = border_str;

            colors[ImGuiCol_ScrollbarBg] = Hex(0x000000, 0.05f);
            colors[ImGuiCol_ScrollbarGrab] = Hex(0xD9D9D9);
            colors[ImGuiCol_ScrollbarGrabHovered] = Hex(0xCFCFCF);
            colors[ImGuiCol_ScrollbarGrabActive] = Hex(0xC5C5C5);
        }
    }

    inline void ApplyVSCodeDark() {
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;

        style.WindowRounding = 4.0f;
        style.ChildRounding = 4.0f;
        style.FrameRounding = 2.0f;
        style.GrabRounding = 2.0f;
        style.PopupRounding = 4.0f;
        style.ScrollbarRounding = 0.0f;
        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.WindowPadding = ImVec2(10, 10);
        style.FramePadding = ImVec2(8, 5);
        style.ItemSpacing = ImVec2(8, 8);

        ImVec4 bg_main = Hex(0x1E1E1E);
        ImVec4 bg_child = Hex(0x252526);
        ImVec4 border = Hex(0x3C3C3C);
        ImVec4 text = Hex(0xD4D4D4);
        ImVec4 text_dim = Hex(0x858585);
        ImVec4 accent_blue = Hex(0x3794FF);

        colors[ImGuiCol_Text] = text;
        colors[ImGuiCol_TextDisabled] = text_dim;
        colors[ImGuiCol_WindowBg] = bg_main;
        colors[ImGuiCol_ChildBg] = bg_child;
        colors[ImGuiCol_PopupBg] = Hex(0x252526);

        colors[ImGuiCol_Border] = border;
        colors[ImGuiCol_BorderShadow] = Hex(0x000000, 0.0f);
        colors[ImGuiCol_MenuBarBg] = Hex(0x1A1A1A);

        colors[ImGuiCol_Header] = Hex(0x094771, 0.35f);
        colors[ImGuiCol_HeaderHovered] = Hex(0x094771, 0.45f);
        colors[ImGuiCol_HeaderActive] = Hex(0x094771, 0.55f);

        colors[ImGuiCol_FrameBg] = Hex(0x0E0E0E);
        colors[ImGuiCol_FrameBgHovered] = Hex(0x1B1B1B);
        colors[ImGuiCol_FrameBgActive] = Hex(0x2C2C2C);

        colors[ImGuiCol_Button] = Hex(0x2D2D2D);
        colors[ImGuiCol_ButtonHovered] = Hex(0x3C3C3C);
        colors[ImGuiCol_ButtonActive] = Hex(0x2E5D9A);

        colors[ImGuiCol_Tab] = bg_main;
        colors[ImGuiCol_TabHovered] = border;
        colors[ImGuiCol_TabSelected] = bg_child;
        colors[ImGuiCol_TabSelectedOverline] = accent_blue;
        colors[ImGuiCol_TabDimmed] = bg_main;
        colors[ImGuiCol_TabDimmedSelected] = bg_child;

        colors[ImGuiCol_DockingPreview] = Hex(0x3794FF, 0.70f);
        colors[ImGuiCol_DockingEmptyBg] = Hex(0x1A1A1A);

        colors[ImGuiCol_ScrollbarBg] = bg_main;
        colors[ImGuiCol_ScrollbarGrab] = Hex(0x4D4D4D);
        colors[ImGuiCol_ScrollbarGrabHovered] = Hex(0x5A5A5A);
        colors[ImGuiCol_ScrollbarGrabActive] = Hex(0x6A6A6A);

        colors[ImGuiCol_Separator] = border;
        colors[ImGuiCol_SeparatorHovered] = accent_blue;
        colors[ImGuiCol_SeparatorActive] = Hex(0x5AA2FF);

        colors[ImGuiCol_CheckMark] = accent_blue;
        colors[ImGuiCol_SliderGrab] = accent_blue;
        colors[ImGuiCol_SliderGrabActive] = Hex(0x5AA2FF);
        colors[ImGuiCol_ResizeGrip] = Hex(0x2D2D2D);
        colors[ImGuiCol_ResizeGripHovered] = accent_blue;
        colors[ImGuiCol_ResizeGripActive] = Hex(0x5AA2FF);

        colors[ImGuiCol_InputTextCursor] = text;

        colors[ImGuiCol_TableHeaderBg] = Hex(0x252526);
        colors[ImGuiCol_TableBorderStrong] = border;
        colors[ImGuiCol_TableBorderLight] = Hex(0x333333);

        colors[ImGuiCol_TextLink] = accent_blue;
        colors[ImGuiCol_TextSelectedBg] = Hex(0x094771, 0.45f);
        colors[ImGuiCol_NavCursor] = accent_blue;
        colors[ImGuiCol_NavWindowingHighlight] = Hex(0xFFFFFF, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg] = Hex(0x000000, 0.20f);
        colors[ImGuiCol_ModalWindowDimBg] = Hex(0x000000, 0.55f);

        colors[ImGuiCol_TitleBg] = bg_main;
        colors[ImGuiCol_TitleBgActive] = bg_main;
        colors[ImGuiCol_TitleBgCollapsed] = Hex(0x1E1E1E, 0.75f);

        colors[ImGuiCol_TabDimmedSelectedOverline] = Hex(0x3C3C3C);

        colors[ImGuiCol_TableRowBg] = Hex(0x000000, 0.0f);
        colors[ImGuiCol_TableRowBgAlt] = Hex(0xFFFFFF, 0.03f);

        colors[ImGuiCol_DragDropTarget] = accent_blue;
        colors[ImGuiCol_DragDropTargetBg] = Hex(0x3794FF, 0.12f);
        colors[ImGuiCol_TreeLines] = Hex(0x3C3C3C);
        colors[ImGuiCol_UnsavedMarker] = Hex(0xE8AB53);

        colors[ImGuiCol_PlotLines] = Hex(0x9CDCFE);
        colors[ImGuiCol_PlotLinesHovered] = Hex(0xFF6040);
        colors[ImGuiCol_PlotHistogram] = Hex(0xCE9178);
        colors[ImGuiCol_PlotHistogramHovered] = Hex(0xFF9A1A);
    }

    inline void ApplyNord(bool dark_mode) {
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;

        style.WindowRounding = 4.0f;
        style.ChildRounding = 4.0f;
        style.FrameRounding = 3.0f;
        style.GrabRounding = 3.0f;
        style.PopupRounding = 4.0f;
        style.ScrollbarRounding = 4.0f;
        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.WindowPadding = ImVec2(10, 10);
        style.FramePadding = ImVec2(8, 5);
        style.ItemSpacing = ImVec2(8, 8);

        if (dark_mode) {
            EnableDarkModeWin(true);
            ImVec4 bg_main = Hex(0x2E3440);
            ImVec4 bg_child = Hex(0x3B4252);
            ImVec4 border = Hex(0x4C566A);
            ImVec4 text = Hex(0xECEFF4);
            ImVec4 text_dim = Hex(0xD8DEE9);
            ImVec4 accent = Hex(0x88C0D0); // nord8

            colors[ImGuiCol_Text] = text;
            colors[ImGuiCol_TextDisabled] = text_dim;
            colors[ImGuiCol_WindowBg] = bg_main;
            colors[ImGuiCol_ChildBg] = bg_child;
            colors[ImGuiCol_PopupBg] = bg_child;

            colors[ImGuiCol_Border] = border;
            colors[ImGuiCol_BorderShadow] = Hex(0x000000, 0.0f);
            colors[ImGuiCol_MenuBarBg] = Hex(0x2B303B);

            colors[ImGuiCol_Header] = Hex(0x81A1C1, 0.25f);
            colors[ImGuiCol_HeaderHovered] = Hex(0x81A1C1, 0.35f);
            colors[ImGuiCol_HeaderActive] = Hex(0x81A1C1, 0.45f);

            colors[ImGuiCol_FrameBg] = Hex(0x3B4252);
            colors[ImGuiCol_FrameBgHovered] = Hex(0x4C566A);
            colors[ImGuiCol_FrameBgActive] = Hex(0x5E81AC);

            colors[ImGuiCol_Button] = Hex(0x4C566A);
            colors[ImGuiCol_ButtonHovered] = Hex(0x5E81AC);
            colors[ImGuiCol_ButtonActive] = Hex(0x81A1C1);

            colors[ImGuiCol_Tab] = bg_main;
            colors[ImGuiCol_TabHovered] = border;
            colors[ImGuiCol_TabSelected] = bg_child;
            colors[ImGuiCol_TabSelectedOverline] = accent;
            colors[ImGuiCol_TabDimmed] = bg_main;
            colors[ImGuiCol_TabDimmedSelected] = bg_child;
            colors[ImGuiCol_TabDimmedSelectedOverline] = Hex(0x4C566A);

            colors[ImGuiCol_DockingPreview] = Hex(0x88C0D0, 0.70f);
            colors[ImGuiCol_DockingEmptyBg] = Hex(0x2B303B);

            colors[ImGuiCol_ScrollbarBg] = bg_main;
            colors[ImGuiCol_ScrollbarGrab] = Hex(0x4C566A);
            colors[ImGuiCol_ScrollbarGrabHovered] = Hex(0x5E81AC);
            colors[ImGuiCol_ScrollbarGrabActive] = Hex(0x81A1C1);

            colors[ImGuiCol_CheckMark] = accent;
            colors[ImGuiCol_SliderGrab] = accent;
            colors[ImGuiCol_SliderGrabActive] = Hex(0x81A1C1);

            colors[ImGuiCol_Separator] = border;
            colors[ImGuiCol_SeparatorHovered] = accent;
            colors[ImGuiCol_SeparatorActive] = Hex(0x81A1C1);

            colors[ImGuiCol_ResizeGrip] = Hex(0x4C566A);
            colors[ImGuiCol_ResizeGripHovered] = accent;
            colors[ImGuiCol_ResizeGripActive] = Hex(0x81A1C1);

            colors[ImGuiCol_InputTextCursor] = text;

            colors[ImGuiCol_TableHeaderBg] = Hex(0x3B4252);
            colors[ImGuiCol_TableBorderStrong] = border;
            colors[ImGuiCol_TableBorderLight] = Hex(0x434C5E);
            colors[ImGuiCol_TableRowBg] = Hex(0x000000, 0.0f);
            colors[ImGuiCol_TableRowBgAlt] = Hex(0xFFFFFF, 0.03f);

            colors[ImGuiCol_TextLink] = accent;
            colors[ImGuiCol_TextSelectedBg] = Hex(0x81A1C1, 0.35f);
            colors[ImGuiCol_NavCursor] = accent;
            colors[ImGuiCol_NavWindowingHighlight] = Hex(0xECEFF4, 0.70f);
            colors[ImGuiCol_NavWindowingDimBg] = Hex(0x000000, 0.20f);
            colors[ImGuiCol_ModalWindowDimBg] = Hex(0x000000, 0.55f);

            colors[ImGuiCol_DragDropTarget] = accent;
            colors[ImGuiCol_DragDropTargetBg] = Hex(0x88C0D0, 0.12f);
            colors[ImGuiCol_TreeLines] = Hex(0x4C566A);
            colors[ImGuiCol_UnsavedMarker] = Hex(0xEBCB8B);

            colors[ImGuiCol_PlotLines] = Hex(0x88C0D0);
            colors[ImGuiCol_PlotLinesHovered] = Hex(0xBF616A);
            colors[ImGuiCol_PlotHistogram] = Hex(0xEBCB8B);
            colors[ImGuiCol_PlotHistogramHovered] = Hex(0xD08770);
        }
        else {
            EnableDarkModeWin(false);
            ImVec4 bg_main = Hex(0xECEFF4);
            ImVec4 bg_child = Hex(0xE5E9F0);
            ImVec4 border = Hex(0xD8DEE9);
            ImVec4 text = Hex(0x2E3440);
            ImVec4 text_dim = Hex(0x4C566A);
            ImVec4 accent = Hex(0x5E81AC);

            colors[ImGuiCol_Text] = text;
            colors[ImGuiCol_TextDisabled] = text_dim;
            colors[ImGuiCol_WindowBg] = bg_main;
            colors[ImGuiCol_ChildBg] = bg_child;
            colors[ImGuiCol_PopupBg] = Hex(0xFFFFFF);

            colors[ImGuiCol_Border] = border;
            colors[ImGuiCol_BorderShadow] = Hex(0x000000, 0.0f);
            colors[ImGuiCol_MenuBarBg] = Hex(0xEAEFF7);

            colors[ImGuiCol_Header] = Hex(0x5E81AC, 0.20f);
            colors[ImGuiCol_HeaderHovered] = Hex(0x5E81AC, 0.30f);
            colors[ImGuiCol_HeaderActive] = Hex(0x5E81AC, 0.40f);

            colors[ImGuiCol_FrameBg] = Hex(0xFFFFFF);
            colors[ImGuiCol_FrameBgHovered] = Hex(0xF6F8FA);
            colors[ImGuiCol_FrameBgActive] = Hex(0xE5E9F0);

            colors[ImGuiCol_Button] = Hex(0xFFFFFF);
            colors[ImGuiCol_ButtonHovered] = Hex(0xF2F4F8);
            colors[ImGuiCol_ButtonActive] = Hex(0xE5E9F0);

            colors[ImGuiCol_Tab] = bg_main;
            colors[ImGuiCol_TabHovered] = border;
            colors[ImGuiCol_TabSelected] = bg_child;
            colors[ImGuiCol_TabSelectedOverline] = accent;
            colors[ImGuiCol_TabDimmed] = bg_main;
            colors[ImGuiCol_TabDimmedSelected] = bg_child;
            colors[ImGuiCol_TabDimmedSelectedOverline] = Hex(0xD8DEE9);

            colors[ImGuiCol_DockingPreview] = Hex(0x5E81AC, 0.70f);
            colors[ImGuiCol_DockingEmptyBg] = Hex(0xE5E9F0);

            colors[ImGuiCol_ScrollbarBg] = Hex(0x000000, 0.04f);
            colors[ImGuiCol_ScrollbarGrab] = Hex(0xC7D0DA);
            colors[ImGuiCol_ScrollbarGrabHovered] = Hex(0xB8C4D1);
            colors[ImGuiCol_ScrollbarGrabActive] = Hex(0xA7B7C8);

            colors[ImGuiCol_CheckMark] = accent;
            colors[ImGuiCol_SliderGrab] = accent;
            colors[ImGuiCol_SliderGrabActive] = Hex(0x81A1C1);

            colors[ImGuiCol_Separator] = border;
            colors[ImGuiCol_SeparatorHovered] = accent;
            colors[ImGuiCol_SeparatorActive] = Hex(0x81A1C1);

            colors[ImGuiCol_ResizeGrip] = Hex(0xD8DEE9);
            colors[ImGuiCol_ResizeGripHovered] = accent;
            colors[ImGuiCol_ResizeGripActive] = Hex(0x81A1C1);

            colors[ImGuiCol_InputTextCursor] = text;

            colors[ImGuiCol_TableHeaderBg] = Hex(0xE5E9F0);
            colors[ImGuiCol_TableBorderStrong] = border;
            colors[ImGuiCol_TableBorderLight] = Hex(0xDFE3EB);
            colors[ImGuiCol_TableRowBg] = Hex(0x000000, 0.0f);
            colors[ImGuiCol_TableRowBgAlt] = Hex(0x000000, 0.03f);

            colors[ImGuiCol_TextLink] = accent;
            colors[ImGuiCol_TextSelectedBg] = Hex(0x5E81AC, 0.25f);
            colors[ImGuiCol_NavCursor] = accent;
            colors[ImGuiCol_NavWindowingHighlight] = Hex(0xFFFFFF, 0.70f);
            colors[ImGuiCol_NavWindowingDimBg] = Hex(0x000000, 0.20f);
            colors[ImGuiCol_ModalWindowDimBg] = Hex(0x000000, 0.40f);

            colors[ImGuiCol_DragDropTarget] = accent;
            colors[ImGuiCol_DragDropTargetBg] = Hex(0x5E81AC, 0.10f);
            colors[ImGuiCol_TreeLines] = Hex(0xD8DEE9);
            colors[ImGuiCol_UnsavedMarker] = Hex(0xD08770);

            colors[ImGuiCol_PlotLines] = Hex(0x5E81AC);
            colors[ImGuiCol_PlotLinesHovered] = Hex(0xBF616A);
            colors[ImGuiCol_PlotHistogram] = Hex(0xEBCB8B);
            colors[ImGuiCol_PlotHistogramHovered] = Hex(0xD08770);
        }
    }

    inline void ApplySolarized(bool dark_mode) {
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;

        style.WindowRounding = 4.0f;
        style.ChildRounding = 4.0f;
        style.FrameRounding = 3.0f;
        style.GrabRounding = 3.0f;
        style.PopupRounding = 4.0f;
        style.ScrollbarRounding = 4.0f;
        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.WindowPadding = ImVec2(10, 10);
        style.FramePadding = ImVec2(8, 5);
        style.ItemSpacing = ImVec2(8, 8);

        // Solarized ��/������ɫ
        if (dark_mode) {
            ImVec4 base = Hex(0x002B36);
            ImVec4 child = Hex(0x073642);
            ImVec4 border = Hex(0x586E75);
            ImVec4 text = Hex(0xEEE8D5);
            ImVec4 text_dim = Hex(0x93A1A1);
            ImVec4 accent = Hex(0x268BD2);

            colors[ImGuiCol_Text] = text;
            colors[ImGuiCol_TextDisabled] = text_dim;
            colors[ImGuiCol_WindowBg] = base;
            colors[ImGuiCol_ChildBg] = child;
            colors[ImGuiCol_PopupBg] = child;
            colors[ImGuiCol_Border] = border;
            colors[ImGuiCol_MenuBarBg] = Hex(0x00212B);

            colors[ImGuiCol_Header] = Hex(0x268BD2, 0.25f);
            colors[ImGuiCol_HeaderHovered] = Hex(0x268BD2, 0.35f);
            colors[ImGuiCol_HeaderActive] = Hex(0x268BD2, 0.45f);

            colors[ImGuiCol_FrameBg] = Hex(0x0A3642);
            colors[ImGuiCol_FrameBgHovered] = Hex(0x164B58);
            colors[ImGuiCol_FrameBgActive] = Hex(0x1D5A68);

            colors[ImGuiCol_Button] = Hex(0x164B58);
            colors[ImGuiCol_ButtonHovered] = Hex(0x1D5A68);
            colors[ImGuiCol_ButtonActive] = Hex(0x268BD2);

            colors[ImGuiCol_Tab] = base;
            colors[ImGuiCol_TabHovered] = border;
            colors[ImGuiCol_TabSelected] = child;
            colors[ImGuiCol_TabSelectedOverline] = accent;
            colors[ImGuiCol_TabDimmed] = base;
            colors[ImGuiCol_TabDimmedSelected] = child;

            colors[ImGuiCol_DockingPreview] = Hex(0x268BD2, 0.70f);
            colors[ImGuiCol_DockingEmptyBg] = Hex(0x00212B);

            colors[ImGuiCol_ScrollbarBg] = base;
            colors[ImGuiCol_ScrollbarGrab] = Hex(0x3B4D54);
            colors[ImGuiCol_ScrollbarGrabHovered] = Hex(0x4A5F67);
            colors[ImGuiCol_ScrollbarGrabActive] = Hex(0x5C727A);

            colors[ImGuiCol_Separator] = border;
            colors[ImGuiCol_SeparatorHovered] = accent;
            colors[ImGuiCol_SeparatorActive] = Hex(0x2AA3F0);

            colors[ImGuiCol_ResizeGrip] = Hex(0x073642);
            colors[ImGuiCol_ResizeGripHovered] = accent;
            colors[ImGuiCol_ResizeGripActive] = Hex(0x2AA3F0);

            colors[ImGuiCol_InputTextCursor] = text;

            colors[ImGuiCol_CheckMark] = accent;
            colors[ImGuiCol_SliderGrab] = accent;
            colors[ImGuiCol_SliderGrabActive] = Hex(0x2AA3F0);

            colors[ImGuiCol_TextLink] = accent;
            colors[ImGuiCol_TextSelectedBg] = Hex(0x268BD2, 0.35f);
            colors[ImGuiCol_NavCursor] = accent;
            colors[ImGuiCol_NavWindowingHighlight] = Hex(0xEEE8D5, 0.70f);
            colors[ImGuiCol_NavWindowingDimBg] = Hex(0x000000, 0.20f);
            colors[ImGuiCol_ModalWindowDimBg] = Hex(0x000000, 0.55f);

            colors[ImGuiCol_BorderShadow] = Hex(0x000000, 0.0f);
            colors[ImGuiCol_TitleBg] = base;
            colors[ImGuiCol_TitleBgActive] = base;
            colors[ImGuiCol_TitleBgCollapsed] = Hex(0x002B36, 0.75f);
            colors[ImGuiCol_TabDimmedSelectedOverline] = Hex(0x586E75);

            colors[ImGuiCol_TableHeaderBg] = Hex(0x073642);
            colors[ImGuiCol_TableBorderStrong] = Hex(0x586E75);
            colors[ImGuiCol_TableBorderLight] = Hex(0x2A4A56);
            colors[ImGuiCol_TableRowBg] = Hex(0x000000, 0.0f);
            colors[ImGuiCol_TableRowBgAlt] = Hex(0xFFFFFF, 0.03f);

            colors[ImGuiCol_DragDropTarget] = accent;
            colors[ImGuiCol_DragDropTargetBg] = Hex(0x268BD2, 0.12f);
            colors[ImGuiCol_TreeLines] = Hex(0x586E75);
            colors[ImGuiCol_UnsavedMarker] = Hex(0xB58900);

            colors[ImGuiCol_PlotLines] = Hex(0x2AA198);
            colors[ImGuiCol_PlotLinesHovered] = Hex(0xDC322F);
            colors[ImGuiCol_PlotHistogram] = Hex(0xB58900);
            colors[ImGuiCol_PlotHistogramHovered] = Hex(0xCB4B16);
        }
        else {
            ImVec4 base = Hex(0xFDF6E3);
            ImVec4 child = Hex(0xEEE8D5);
            ImVec4 border = Hex(0x93A1A1);
            ImVec4 text = Hex(0x073642);
            ImVec4 text_dim = Hex(0x586E75);
            ImVec4 accent = Hex(0x268BD2);

            colors[ImGuiCol_Text] = text;
            colors[ImGuiCol_TextDisabled] = text_dim;
            colors[ImGuiCol_WindowBg] = base;
            colors[ImGuiCol_ChildBg] = child;
            colors[ImGuiCol_PopupBg] = Hex(0xFFFFFF);
            colors[ImGuiCol_Border] = border;
            colors[ImGuiCol_MenuBarBg] = Hex(0xF7F1DE);

            colors[ImGuiCol_Header] = Hex(0x268BD2, 0.20f);
            colors[ImGuiCol_HeaderHovered] = Hex(0x268BD2, 0.28f);
            colors[ImGuiCol_HeaderActive] = Hex(0x268BD2, 0.36f);

            colors[ImGuiCol_FrameBg] = Hex(0xFFFFFF);
            colors[ImGuiCol_FrameBgHovered] = Hex(0xF5F0E3);
            colors[ImGuiCol_FrameBgActive] = Hex(0xEEE8D5);

            colors[ImGuiCol_Button] = Hex(0xFFFFFF);
            colors[ImGuiCol_ButtonHovered] = Hex(0xF5F0E3);
            colors[ImGuiCol_ButtonActive] = Hex(0xEEE8D5);

            colors[ImGuiCol_Tab] = base;
            colors[ImGuiCol_TabHovered] = border;
            colors[ImGuiCol_TabSelected] = child;
            colors[ImGuiCol_TabSelectedOverline] = accent;
            colors[ImGuiCol_TabDimmed] = base;
            colors[ImGuiCol_TabDimmedSelected] = child;

            colors[ImGuiCol_DockingPreview] = Hex(0x268BD2, 0.70f);
            colors[ImGuiCol_DockingEmptyBg] = Hex(0xEEE8D5);

            colors[ImGuiCol_ScrollbarBg] = Hex(0x000000, 0.05f);
            colors[ImGuiCol_ScrollbarGrab] = Hex(0xC3CEC8);
            colors[ImGuiCol_ScrollbarGrabHovered] = Hex(0xB3C0BA);
            colors[ImGuiCol_ScrollbarGrabActive] = Hex(0xA1B0A9);

            colors[ImGuiCol_Separator] = border;
            colors[ImGuiCol_SeparatorHovered] = accent;
            colors[ImGuiCol_SeparatorActive] = Hex(0x2AA3F0);

            colors[ImGuiCol_ResizeGrip] = Hex(0xEEE8D5);
            colors[ImGuiCol_ResizeGripHovered] = accent;
            colors[ImGuiCol_ResizeGripActive] = Hex(0x2AA3F0);

            colors[ImGuiCol_InputTextCursor] = text;

            colors[ImGuiCol_CheckMark] = accent;
            colors[ImGuiCol_SliderGrab] = accent;
            colors[ImGuiCol_SliderGrabActive] = Hex(0x2AA3F0);

            colors[ImGuiCol_TextLink] = accent;
            colors[ImGuiCol_TextSelectedBg] = Hex(0x268BD2, 0.25f);
            colors[ImGuiCol_NavCursor] = accent;
            colors[ImGuiCol_NavWindowingHighlight] = Hex(0xFFFFFF, 0.70f);
            colors[ImGuiCol_NavWindowingDimBg] = Hex(0x000000, 0.20f);
            colors[ImGuiCol_ModalWindowDimBg] = Hex(0x000000, 0.40f);

            colors[ImGuiCol_BorderShadow] = Hex(0x000000, 0.0f);
            colors[ImGuiCol_TitleBg] = base;
            colors[ImGuiCol_TitleBgActive] = base;
            colors[ImGuiCol_TitleBgCollapsed] = Hex(0xFDF6E3, 0.75f);
            colors[ImGuiCol_TabDimmedSelectedOverline] = Hex(0x93A1A1);

            colors[ImGuiCol_TableHeaderBg] = Hex(0xEEE8D5);
            colors[ImGuiCol_TableBorderStrong] = Hex(0x93A1A1);
            colors[ImGuiCol_TableBorderLight] = Hex(0xC8C4B6);
            colors[ImGuiCol_TableRowBg] = Hex(0x000000, 0.0f);
            colors[ImGuiCol_TableRowBgAlt] = Hex(0x000000, 0.03f);

            colors[ImGuiCol_DragDropTarget] = accent;
            colors[ImGuiCol_DragDropTargetBg] = Hex(0x268BD2, 0.10f);
            colors[ImGuiCol_TreeLines] = Hex(0x93A1A1);
            colors[ImGuiCol_UnsavedMarker] = Hex(0xB58900);

            colors[ImGuiCol_PlotLines] = Hex(0x2AA198);
            colors[ImGuiCol_PlotLinesHovered] = Hex(0xDC322F);
            colors[ImGuiCol_PlotHistogram] = Hex(0xB58900);
            colors[ImGuiCol_PlotHistogramHovered] = Hex(0xCB4B16);
        }
    }

    inline void ApplyCustom() {
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;
        try {
            std::ifstream file("custom_theme.json");
            if (file.is_open()) {
                nlohmann::json j;
                file >> j;
                style.WindowRounding = j.value("window_rounding", 0.0f);
                style.ChildRounding = j.value("child_rounding", 0.0f);
                style.FrameRounding = j.value("frame_rounding", 0.0f);
                style.GrabRounding = j.value("grab_rounding", 0.0f);
                style.PopupRounding = j.value("popup_rounding", 0.0f);
                style.ScrollbarRounding = j.value("scrollbar_rounding", 0.0f);
                style.TabRounding = j.value("tab_rounding", 0.0f);

                style.WindowBorderSize = j.value("window_border_size", 0.0f);
                style.ChildBorderSize = j.value("child_border_size", 0.0f);
                style.PopupBorderSize = j.value("popup_border_size", 0.0f);
                style.FrameBorderSize = j.value("frame_border_size", 0.0f);
                style.TabBorderSize = j.value("tab_border_size", 0.0f);

                style.WindowPadding = ImVec2(
                    j.value("window_padding_x", 8),
                    j.value("window_padding_y", 8)
                );
                style.FramePadding = ImVec2(
                    j.value("frame_padding_x", 6),
                    j.value("frame_padding_y", 4)
                );
                style.ItemSpacing = ImVec2(
                    j.value("item_spacing_x", 8),
                    j.value("item_spacing_y", 6)
                );
                style.ScrollbarSize = j.value("scrollbar_size", 14.0f);

                for (auto& [key, value] : j["colors"].items()) {
                    // hex: "0xRRGGBB"
                    std::string hexstr = value["hex"].get<std::string>();
                    unsigned int hex = std::stoul(hexstr.substr(1), nullptr, 16); // ����"#"
                    float alpha = value.value("alpha", 1.0f);
                    ImVec4 col = Hex(hex, alpha);
                    ImGuiCol col_index = ImGuiCol_COUNT;
                    for (int i = 0; i < ImGuiCol_COUNT; i++) {
                        if (std::string(ImGui::GetStyleColorName(static_cast<ImGuiCol>(i))) == key) {
                            col_index = static_cast<ImGuiCol>(i);
                            break;
                        }
                    }
                    if (col_index != ImGuiCol_COUNT)
                        colors[col_index] = col;
                }
            }
        }
        catch (...) {
            ApplySolarized(true);
        }
    }

    inline void WriteDefaultCustomTheme() {
        nlohmann::json j;
        ImGuiStyle& style = ImGui::GetStyle();
        j["window_rounding"] = style.WindowRounding;
        j["child_rounding"] = style.ChildRounding;
        j["frame_rounding"] = style.FrameRounding;
        j["grab_rounding"] = style.GrabRounding;
        j["popup_rounding"] = style.PopupRounding;
        j["scrollbar_rounding"] = style.ScrollbarRounding;
        j["tab_rounding"] = style.TabRounding;

        j["window_border_size"] = style.WindowBorderSize;
        j["child_border_size"] = style.ChildBorderSize;
        j["popup_border_size"] = style.PopupBorderSize;
        j["frame_border_size"] = style.FrameBorderSize;
        j["tab_border_size"] = style.TabBorderSize;

        j["window_padding_x"] = style.WindowPadding.x;
        j["window_padding_y"] = style.WindowPadding.y;
        j["frame_padding_x"] = style.FramePadding.x;
        j["frame_padding_y"] = style.FramePadding.y;
        j["item_spacing_x"] = style.ItemSpacing.x;
        j["item_spacing_y"] = style.ItemSpacing.y;
        j["scrollbar_size"] = style.ScrollbarSize;

        for (int i = 0; i < ImGuiCol_COUNT; i++) {
            ImVec4 col = style.Colors[i];
            j["colors"][ImGui::GetStyleColorName(static_cast<ImGuiCol>(i))] = {
                {"hex", ImVec4ToHex(col)},
                {"alpha", col.w}
            };
        }

        std::ofstream file("custom_theme.json");
        if (file.is_open()) {
            file << j.dump(4);
            file.close();
        }
    }
}