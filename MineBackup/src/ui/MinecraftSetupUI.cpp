#include "MinecraftSetupUI.h"

#include "i18n.h"
#include "imgui-all.h"
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
			? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
			: issue.severity == ReadinessSeverity::Warning
				? ImVec4(1.0f, 0.75f, 0.25f, 1.0f)
				: ImVec4(0.45f, 0.7f, 1.0f, 1.0f);
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
