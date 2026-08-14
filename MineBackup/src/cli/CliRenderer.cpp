#include "CliRenderer.h"

#include "text_to_text.h"

#include <algorithm>
#include <exception>
#include <iostream>
#include <limits>
#include <utility>

using namespace std;

namespace {

bool ParseCode(const string& value, OperationCode& code) {
	const pair<const char*, OperationCode> values[]{
		{"success", OperationCode::Success},
		{"no_changes", OperationCode::NoChanges},
		{"invalid_arguments", OperationCode::InvalidArguments},
		{"profile_busy", OperationCode::ProfileBusy},
		{"target_not_found", OperationCode::TargetNotFound},
		{"migration_required", OperationCode::MigrationRequired},
		{"invalid_profile", OperationCode::InvalidProfile},
		{"tool_unavailable", OperationCode::ToolUnavailable},
		{"backup_failed", OperationCode::BackupFailed},
		{"job_failed", OperationCode::JobFailed},
		{"verification_failed", OperationCode::VerificationFailed},
		{"restore_failed", OperationCode::RestoreFailed},
		{"cancelled", OperationCode::Cancelled},
		{"partial_success", OperationCode::PartialSuccess}};
	for (const auto& [name, current] : values) {
		if (value == name) {
			code = current;
			return true;
		}
	}
	return false;
}

bool ParseSeverity(const string& value, DiagnosticSeverity& severity) {
	if (value == "info") severity = DiagnosticSeverity::Info;
	else if (value == "warning") severity = DiagnosticSeverity::Warning;
	else if (value == "error") severity = DiagnosticSeverity::Error;
	else return false;
	return true;
}

} // namespace

nlohmann::json BuildCliEnvelope(const CliResult& result) {
	constexpr size_t kMaximumDiagnosticDetailBytes = 256u * 1024u;
	constexpr size_t kMaximumDiagnosticBytes = 1024u * 1024u;
	nlohmann::json diagnostics = nlohmann::json::array();
	size_t diagnosticBytes = 0;
	bool diagnosticsTruncated = false;
	for (const auto& item : result.diagnostics) {
		if (diagnosticBytes >= kMaximumDiagnosticBytes) {
			diagnosticsTruncated = true;
			break;
		}
		const size_t remaining = kMaximumDiagnosticBytes - diagnosticBytes;
		const auto sanitized = SanitizeUtf8(
			item.detail, min(kMaximumDiagnosticDetailBytes, remaining));
		string detail = sanitized.value;
		if (sanitized.invalidUtf8Replaced) detail += ";diagnosticUtf8Replaced=true";
		if (sanitized.truncated) detail += ";diagnosticTruncated=true";
		diagnosticBytes += detail.size();
		diagnostics.push_back({
			{"eventId", SanitizeUtf8(item.eventId, kMaximumDiagnosticDetailBytes).value},
			{"severity", ToString(item.severity)},
			{"detail", detail}});
	}
	if (diagnosticsTruncated) {
		diagnostics.push_back({
			{"eventId", "cli.diagnostics.truncated"},
			{"severity", "warning"},
			{"detail", "Diagnostic output exceeded the serialization budget."}});
	}
	return {
		{"schemaVersion", 1},
		{"command", result.command},
		{"ok", IsSuccessful(result.code)},
		{"code", ToString(result.code)},
		{"data", result.data},
		{"diagnostics", diagnostics}};
}

string SerializeCliEnvelope(const CliResult& result) {
	try {
		return BuildCliEnvelope(result).dump();
	}
	catch (const exception&) {
		CliResult fallback{
			result.command.empty() ? "unknown" : result.command,
			OperationCode::JobFailed};
		fallback.diagnostics.push_back({
			"cli.response.serialization_failed", DiagnosticSeverity::Error, {}});
		try {
			return BuildCliEnvelope(fallback).dump();
		}
		catch (const exception&) {
			return R"({"schemaVersion":1,"command":"unknown","ok":false,"code":"job_failed","data":{},"diagnostics":[{"eventId":"cli.response.serialization_failed","severity":"error","detail":""}]})";
		}
	}
}

bool ParseCliEnvelope(const string& payload, CliResult& result) {
	const auto envelope = nlohmann::json::parse(payload, nullptr, false);
	if (envelope.is_discarded() || !envelope.is_object()
		|| envelope.value("schemaVersion", 0) != 1
		|| !envelope.contains("command") || !envelope["command"].is_string()
		|| !envelope.contains("code") || !envelope["code"].is_string()
		|| !envelope.contains("data") || !envelope.contains("diagnostics")
		|| !envelope["diagnostics"].is_array()) return false;
	OperationCode code;
	if (!ParseCode(envelope["code"].get<string>(), code)) return false;
	CliResult parsed;
	parsed.command = envelope["command"].get<string>();
	parsed.code = code;
	parsed.data = envelope["data"];
	for (const auto& value : envelope["diagnostics"]) {
		if (!value.is_object()
			|| !value.contains("eventId") || !value["eventId"].is_string()
			|| !value.contains("severity") || !value["severity"].is_string()
			|| !value.contains("detail") || !value["detail"].is_string()) return false;
		Diagnostic diagnostic;
		diagnostic.eventId = value["eventId"].get<string>();
		diagnostic.detail = value["detail"].get<string>();
		if (!ParseSeverity(value["severity"].get<string>(), diagnostic.severity)) return false;
		parsed.diagnostics.push_back(std::move(diagnostic));
	}
	result = std::move(parsed);
	return true;
}

void RenderCliResult(const CliResult& result, bool jsonOutput) {
	if (jsonOutput) {
		cout << SerializeCliEnvelope(result) << '\n';
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
