#pragma once
#ifndef THEME_PALETTE_H
#define THEME_PALETTE_H

#include "imgui-all.h"
#include "ThemeManager.h"
#include "Logging.h"

namespace ThemePalette {

    enum class StatusColor {
        Success,
        Warning,
        Error,
        Info,
        Trace,
        Muted,
        Special
    };

    inline bool IsDarkMode() {
        if (ImGui::GetCurrentContext() == nullptr) {
            return false;
        }
        ImGuiStyle& style = ImGui::GetStyle();
        return ImGuiTheme::RelativeLuminance(style.Colors[ImGuiCol_WindowBg]) < 0.45f;
    }

    inline ImVec4 GetStatusColor(StatusColor status, bool isDark) {
        if (isDark) {
            switch (status) {
            case StatusColor::Success: return ImVec4(0.35f, 0.85f, 0.45f, 1.0f);
            case StatusColor::Warning: return ImVec4(1.00f, 0.78f, 0.28f, 1.0f);
            case StatusColor::Error:   return ImVec4(1.00f, 0.40f, 0.40f, 1.0f);
            case StatusColor::Info:    return ImVec4(0.55f, 0.78f, 0.98f, 1.0f);
            case StatusColor::Trace:   return ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
            case StatusColor::Muted:   return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
            case StatusColor::Special: return ImVec4(0.85f, 0.55f, 0.95f, 1.0f);
            }
        }
        else {
            switch (status) {
            case StatusColor::Success: return ImVec4(0.12f, 0.58f, 0.22f, 1.0f);
            case StatusColor::Warning: return ImVec4(0.72f, 0.42f, 0.02f, 1.0f);
            case StatusColor::Error:   return ImVec4(0.82f, 0.18f, 0.18f, 1.0f);
            case StatusColor::Info:    return ImVec4(0.08f, 0.42f, 0.78f, 1.0f);
            case StatusColor::Trace:   return ImVec4(0.40f, 0.40f, 0.40f, 1.0f);
            case StatusColor::Muted:   return ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
            case StatusColor::Special: return ImVec4(0.55f, 0.20f, 0.75f, 1.0f);
            }
        }
        return ImGui::GetCurrentContext() != nullptr
            ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
            : (isDark ? ImVec4(0.90f, 0.90f, 0.90f, 1.0f) : ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
    }

    inline ImVec4 GetStatusColor(StatusColor status) {
        return GetStatusColor(status, IsDarkMode());
    }

    inline ImVec4 GetStatusIconColor(StatusColor status, bool isDark) {
        if (isDark) {
            return GetStatusColor(status, true);
        }
        switch (status) {
        case StatusColor::Success: return ImVec4(0.169f, 0.588f, 0.278f, 1.0f); // #2B9647
        case StatusColor::Warning: return ImVec4(0.722f, 0.455f, 0.000f, 1.0f); // #B87400
        case StatusColor::Error:   return ImVec4(0.824f, 0.294f, 0.294f, 1.0f); // #D24B4B
        case StatusColor::Info:    return ImVec4(0.259f, 0.502f, 0.784f, 1.0f); // #4280C8
        case StatusColor::Special: return ImVec4(0.541f, 0.388f, 0.776f, 1.0f); // #8A63C6
        case StatusColor::Trace:   return GetStatusColor(StatusColor::Trace, false);
        case StatusColor::Muted:   return GetStatusColor(StatusColor::Muted, false);
        }
        return ImGui::GetCurrentContext() != nullptr
            ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
            : (isDark ? ImVec4(0.90f, 0.90f, 0.90f, 1.0f) : ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
    }

    inline ImVec4 GetStatusIconColor(StatusColor status) {
        return GetStatusIconColor(status, IsDarkMode());
    }

    inline ImVec4 GetLogLevelColor(minebackup::logging::LogLevel level, bool isDark) {
        using minebackup::logging::LogLevel;
        if (isDark) {
            switch (level) {
            case LogLevel::Trace:
                return GetStatusColor(StatusColor::Trace, true);
            case LogLevel::Debug:
                return GetStatusColor(StatusColor::Info, true);
            case LogLevel::Info:
                return ImGui::GetCurrentContext() != nullptr
                    ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
                    : ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
            case LogLevel::Warning:
                return GetStatusColor(StatusColor::Warning, true);
            case LogLevel::Error:
                return GetStatusColor(StatusColor::Error, true);
            case LogLevel::Critical:
                return ImVec4(1.00f, 0.25f, 0.60f, 1.0f);
            }
        }
        else {
            switch (level) {
            case LogLevel::Trace:
                return ImVec4(0.349f, 0.388f, 0.431f, 1.0f); // #59636E
            case LogLevel::Debug:
                return ImVec4(0.031f, 0.376f, 0.792f, 1.0f); // #0860CA
            case LogLevel::Info:
                return ImGui::GetCurrentContext() != nullptr
                    ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
                    : ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
            case LogLevel::Warning:
                return ImVec4(0.541f, 0.361f, 0.000f, 1.0f); // #8A5C00
            case LogLevel::Error:
                return ImVec4(0.757f, 0.169f, 0.204f, 1.0f); // #C12B34
            case LogLevel::Critical:
                return ImVec4(0.706f, 0.137f, 0.353f, 1.0f); // #B4235A
            }
        }
        return ImGui::GetCurrentContext() != nullptr
            ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
            : (isDark ? ImVec4(0.90f, 0.90f, 0.90f, 1.0f) : ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
    }

    inline ImVec4 GetLogLevelColor(minebackup::logging::LogLevel level) {
        return GetLogLevelColor(level, IsDarkMode());
    }

    inline ImVec4 GetLogBackgroundColor(bool isDark = IsDarkMode()) {
        if (ImGui::GetCurrentContext() == nullptr) {
            return isDark ? ImVec4(0.12f, 0.12f, 0.12f, 1.0f) : ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
        }
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec4 childBg = style.Colors[ImGuiCol_ChildBg];
        const ImVec4 windowBg = style.Colors[ImGuiCol_WindowBg];
        if (childBg.w > 0.0f) {
            return ImGuiTheme::CompositeOver(childBg, windowBg);
        }
        return windowBg;
    }

    inline ImVec4 GetLogMarkerColor(minebackup::logging::LogLevel level, bool isDark) {
        using minebackup::logging::LogLevel;
        const ImVec4 fg = GetLogLevelColor(level, isDark);
        const ImVec4 bg = GetLogBackgroundColor(isDark);
        float amount = 0.50f;
        switch (level) {
        case LogLevel::Trace:    amount = 0.65f; break;
        case LogLevel::Info:     amount = 0.62f; break;
        case LogLevel::Debug:    amount = 0.50f; break;
        case LogLevel::Warning:  amount = 0.40f; break;
        case LogLevel::Error:    amount = 0.35f; break;
        case LogLevel::Critical: amount = 0.30f; break;
        }
        ImVec4 marker = ImGuiTheme::Blend(fg, bg, amount);
        marker.w = 1.0f;
        return marker;
    }

    inline ImVec4 GetLogMarkerColor(minebackup::logging::LogLevel level) {
        return GetLogMarkerColor(level, IsDarkMode());
    }

    inline ImVec4 GetDangerButtonColor(bool isDark = IsDarkMode()) {
        return isDark ? ImVec4(0.85f, 0.25f, 0.25f, 1.0f) : ImVec4(0.80f, 0.20f, 0.20f, 1.0f);
    }

    inline ImVec4 GetDangerButtonHoveredColor(bool isDark = IsDarkMode()) {
        return isDark ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f) : ImVec4(0.70f, 0.15f, 0.15f, 1.0f);
    }

    inline ImVec4 GetSuccessButtonColor(bool isDark = IsDarkMode()) {
        return isDark ? ImVec4(0.20f, 0.65f, 0.30f, 1.0f) : ImVec4(0.18f, 0.60f, 0.28f, 1.0f);
    }

    inline ImVec4 GetSuccessButtonHoveredColor(bool isDark = IsDarkMode()) {
        return isDark ? ImVec4(0.25f, 0.75f, 0.35f, 1.0f) : ImVec4(0.14f, 0.50f, 0.22f, 1.0f);
    }

    inline ImVec4 GetSuccessButtonActiveColor(bool isDark = IsDarkMode()) {
        return isDark ? ImVec4(0.15f, 0.55f, 0.25f, 1.0f) : ImVec4(0.10f, 0.42f, 0.18f, 1.0f);
    }

} // namespace ThemePalette

#endif // THEME_PALETTE_H
