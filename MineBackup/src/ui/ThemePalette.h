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
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }

    inline ImVec4 GetStatusColor(StatusColor status) {
        return GetStatusColor(status, IsDarkMode());
    }

    inline ImVec4 GetLogLevelColor(minebackup::logging::LogLevel level, bool isDark) {
        using minebackup::logging::LogLevel;
        switch (level) {
        case LogLevel::Trace:
            return GetStatusColor(StatusColor::Trace, isDark);
        case LogLevel::Debug:
            return GetStatusColor(StatusColor::Info, isDark);
        case LogLevel::Info:
            return isDark ? ImVec4(0.90f, 0.90f, 0.90f, 1.0f) : ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
        case LogLevel::Warning:
            return GetStatusColor(StatusColor::Warning, isDark);
        case LogLevel::Error:
            return GetStatusColor(StatusColor::Error, isDark);
        case LogLevel::Critical:
            return isDark ? ImVec4(1.00f, 0.25f, 0.60f, 1.0f) : ImVec4(0.80f, 0.05f, 0.40f, 1.0f);
        }
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }

    inline ImVec4 GetLogLevelColor(minebackup::logging::LogLevel level) {
        return GetLogLevelColor(level, IsDarkMode());
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
