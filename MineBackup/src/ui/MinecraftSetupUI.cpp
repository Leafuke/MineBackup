#include "MinecraftSetupUI.h"

#include "i18n.h"
#include "imgui-all.h"
#include "ThemePalette.h"
#include "text_to_text.h"

#include <cctype>
#include <string>

using namespace std;

namespace {

string ReadinessKey(const string& code) {
	string key = "WIZARD_READINESS_";
	for (unsigned char character : code) {
		key.push_back(character == '-' || character == '.'
			? '_' : static_cast<char>(toupper(character)));
	}
	return key;
}

} // namespace

const char* MinecraftReadinessIssueLabel(const ReadinessIssue& issue) {
	const string key = ReadinessKey(issue.code);
	const char* translated = L(key.c_str());
	return string(translated) == key ? issue.code.c_str() : translated;
}

void DrawMinecraftReadinessIssues(const BatchReadinessResult& readiness) {
	for (const auto& issue : readiness.report.issues) {
		const ImVec4 color = issue.severity == ReadinessSeverity::Blocking
			? ThemePalette::GetStatusColor(ThemePalette::StatusColor::Error)
			: issue.severity == ReadinessSeverity::Warning
				? ThemePalette::GetStatusColor(ThemePalette::StatusColor::Warning)
				: ThemePalette::GetStatusColor(ThemePalette::StatusColor::Info);
		ImGui::TextColored(color, "• %s", MinecraftReadinessIssueLabel(issue));
		if (!issue.relatedPath.empty()) {
			ImGui::Indent();
			ImGui::TextWrapped("%s",
				wstring_to_utf8(issue.relatedPath.wstring()).c_str());
			ImGui::Unindent();
		}
		if (!issue.detail.empty()) {
			ImGui::Indent();
			ImGui::TextWrapped("%s", wstring_to_utf8(issue.detail).c_str());
			ImGui::Unindent();
		}
	}
}
