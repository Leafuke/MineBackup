#pragma once
#ifndef UI_HELPERS_H
#define UI_HELPERS_H

#include "imgui-all.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

inline float CalcButtonWidth(const char* text, float minWidth = 80.0f, float padding = 20.0f) {
	float textWidth = ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2 + padding;
	return (std::max)(textWidth, minWidth);
}

inline float CalcPairButtonWidth(const char* text1, const char* text2, float minWidth = 100.0f, float padding = 20.0f) {
	float w1 = ImGui::CalcTextSize(text1).x + ImGui::GetStyle().FramePadding.x * 2 + padding;
	float w2 = ImGui::CalcTextSize(text2).x + ImGui::GetStyle().FramePadding.x * 2 + padding;
	return (std::max)((std::max)(w1, w2), minWidth);
}

struct UiMetrics {
	float em = 20.0f;
	float lineHeight = 20.0f;
	float frameHeight = 28.0f;
	float spacingX = 8.0f;
	float spacingY = 6.0f;
	float smallGap = 4.0f;
	float sectionGap = 12.0f;
	float cardPadding = 10.0f;
	float minButtonHeight = 28.0f;

	float Em(float units) const {
		return em * units;
	}
};

inline UiMetrics GetUiMetrics() {
	const ImGuiStyle& style = ImGui::GetStyle();
	UiMetrics metrics;
	metrics.em = (std::max)(ImGui::GetFontSize(), 1.0f);
	metrics.lineHeight = (std::max)(ImGui::GetTextLineHeightWithSpacing(), metrics.em);
	metrics.frameHeight = (std::max)(ImGui::GetFrameHeight(), metrics.lineHeight);
	metrics.spacingX = (std::max)(style.ItemSpacing.x, metrics.em * 0.35f);
	metrics.spacingY = (std::max)(style.ItemSpacing.y, metrics.em * 0.30f);
	metrics.smallGap = (std::max)(style.ItemInnerSpacing.y, metrics.em * 0.20f);
	metrics.sectionGap = (std::max)(metrics.spacingY * 1.5f, metrics.em * 0.60f);
	metrics.cardPadding = (std::max)(style.WindowPadding.x, metrics.em * 0.50f);
	metrics.minButtonHeight = (std::max)(metrics.frameHeight, metrics.em * 1.35f);
	return metrics;
}

inline float GetStandardControlWidth(float maximumEm = 18.0f) {
	const UiMetrics metrics = GetUiMetrics();
	return (std::min)(ImGui::GetContentRegionAvail().x, metrics.Em(maximumEm));
}

inline void SetStandardControlWidth(float maximumEm = 18.0f) {
	ImGui::SetNextItemWidth(GetStandardControlWidth(maximumEm));
}

inline float GetStandardActionWidth(float maximumEm = 13.0f) {
	const UiMetrics metrics = GetUiMetrics();
	return (std::min)(ImGui::GetContentRegionAvail().x, metrics.Em(maximumEm));
}

struct SettingsResponsiveLayout {
	bool useSidebar = false;
	float sidebarWidth = 0.0f;
	float contentWidth = 0.0f;
};

inline SettingsResponsiveLayout ComputeSettingsResponsiveLayout(
	float availableWidth,
	float em,
	float spacingX = 0.0f) {
	const float safeEm = (std::max)(em, 1.0f);
	const float gap = spacingX > 0.0f ? spacingX : safeEm * 0.5f;
	const float safeWidth = (std::max)(availableWidth, 0.0f);
	SettingsResponsiveLayout result;
	result.useSidebar = safeWidth >= safeEm * 38.0f;
	if (result.useSidebar) {
		result.sidebarWidth = (std::clamp)(safeWidth * 0.18f, safeEm * 8.5f, safeEm * 11.0f);
		result.contentWidth = (std::max)(safeWidth - result.sidebarWidth - gap, 0.0f);
	}
	else {
		result.contentWidth = safeWidth;
	}
	return result;
}

struct HistoryResponsiveLayout {
	bool useSplitView = false;
	float listWidth = 0.0f;
	float detailsWidth = 0.0f;
};

struct UiScaleMigrationResult {
	float scale = 1.0f;
	bool migrated = false;
};

inline UiScaleMigrationResult MigrateUiScale(
	float configuredScale,
	float primaryDpiScale,
	bool usesLegacySemantics) {
	UiScaleMigrationResult result;
	result.scale = configuredScale;
	result.migrated = usesLegacySemantics;
	if (usesLegacySemantics) {
		const float safeDpi = primaryDpiScale > 0.0f ? primaryDpiScale : 1.0f;
		if (std::abs(result.scale - safeDpi) <= 0.05f) result.scale = 1.0f;
	}
	result.scale = (std::clamp)(result.scale, 0.75f, 2.5f);
	return result;
}

inline HistoryResponsiveLayout ComputeHistoryResponsiveLayout(
	float availableWidth,
	float em,
	float spacingX = 0.0f) {
	const float safeEm = (std::max)(em, 1.0f);
	const float gap = spacingX > 0.0f ? spacingX : safeEm * 0.5f;
	const float safeWidth = (std::max)(availableWidth, 0.0f);
	HistoryResponsiveLayout result;
	result.useSplitView = safeWidth >= safeEm * 38.0f;
	if (result.useSplitView) {
		const float usable = (std::max)(safeWidth - gap, 0.0f);
		result.listWidth = (std::clamp)(usable * 0.40f, safeEm * 18.0f, usable - safeEm * 22.0f);
		result.detailsWidth = (std::max)(usable - result.listWidth, 0.0f);
	}
	else {
		result.listWidth = safeWidth;
	}
	return result;
}

inline ImVec2 ClampWindowSizeToWorkArea(
	const ImVec2& desired,
	const ImVec2& workSize,
	float maximumWorkAreaFraction = 0.90f) {
	const float fraction = (std::clamp)(maximumWorkAreaFraction, 0.25f, 1.0f);
	return ImVec2(
		(std::max)(1.0f, (std::min)(desired.x, workSize.x * fraction)),
		(std::max)(1.0f, (std::min)(desired.y, workSize.y * fraction)));
}

inline void SetNextWindowSizeFromMetrics(
	const UiMetrics& metrics,
	float widthEm,
	float heightEm,
	ImGuiCond condition = ImGuiCond_FirstUseEver) {
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const ImVec2 desired(metrics.Em(widthEm), metrics.Em(heightEm));
	ImGui::SetNextWindowSize(
		ClampWindowSizeToWorkArea(desired, viewport->WorkSize),
		condition);
}

inline void SetNextWindowConstraintsFromMetrics(
	const UiMetrics& metrics,
	float minimumWidthEm,
	float minimumHeightEm) {
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const ImVec2 maximum = ClampWindowSizeToWorkArea(
		viewport->WorkSize, viewport->WorkSize, 0.98f);
	const ImVec2 minimum(
		(std::min)(metrics.Em(minimumWidthEm), maximum.x),
		(std::min)(metrics.Em(minimumHeightEm), maximum.y));
	ImGui::SetNextWindowSizeConstraints(minimum, maximum);
}

inline bool BeginUiCard(const char* id, const ImVec2& size = ImVec2(0.0f, 0.0f)) {
	const UiMetrics metrics = GetUiMetrics();
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, metrics.em * 0.28f);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(metrics.cardPadding, metrics.cardPadding));
	return ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
}

inline void EndUiCard() {
	ImGui::EndChild();
	ImGui::PopStyleVar(3);
}

inline void TextEllipsisWithTooltip(
	const char* text,
	float availableWidth,
	const ImVec4* color = nullptr) {
	const char* safeText = text ? text : "";
	const float safeWidth = (std::max)(availableWidth, 1.0f);
	const ImVec2 fullSize = ImGui::CalcTextSize(safeText);
	if (fullSize.x <= safeWidth) {
		if (color) ImGui::TextColored(*color, "%s", safeText);
		else ImGui::TextUnformatted(safeText);
		return;
	}

	const char* ellipsis = "...";
	const float ellipsisWidth = ImGui::CalcTextSize(ellipsis).x;
	const char* end = safeText + std::strlen(safeText);
	const char* clippedEnd = safeText;
	while (clippedEnd < end) {
		const char* next = clippedEnd + 1;
		while (next < end && (*next & 0xC0) == 0x80) ++next;
		if (ImGui::CalcTextSize(safeText, next).x + ellipsisWidth > safeWidth) break;
		clippedEnd = next;
	}
	std::string clipped(safeText, clippedEnd);
	clipped += ellipsis;
	if (color) ImGui::TextColored(*color, "%s", clipped.c_str());
	else ImGui::TextUnformatted(clipped.c_str());
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", safeText);
}

#endif // UI_HELPERS_H
