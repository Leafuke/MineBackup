#include "CliRenderer.h"

#include <iostream>

using namespace std;

void RenderCliResult(const CliResult& result, bool jsonOutput) {
	if (jsonOutput) {
		nlohmann::json diagnostics = nlohmann::json::array();
		for (const auto& item : result.diagnostics) {
			diagnostics.push_back({
				{"eventId", item.eventId},
				{"severity", ToString(item.severity)},
				{"detail", item.detail}});
		}
		nlohmann::json envelope{
			{"schemaVersion", 1},
			{"command", result.command},
			{"ok", IsSuccessful(result.code)},
			{"code", ToString(result.code)},
			{"data", result.data},
			{"diagnostics", diagnostics}};
		cout << envelope.dump() << '\n';
		return;
	}
	for (const auto& item : result.diagnostics) {
		ostream& stream = item.severity == DiagnosticSeverity::Error ? cerr : cout;
		stream << '[' << ToString(item.severity) << "] " << item.eventId;
		if (!item.detail.empty()) stream << ": " << item.detail;
		stream << '\n';
	}
	if (!result.data.empty()) cout << result.data.dump(2) << '\n';
}
