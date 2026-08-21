#include "CliJobResult.h"

#include "text_to_text.h"

#include <algorithm>
#include <utility>

using namespace std;

nlohmann::json BuildJobRunData(const JobRunResult& run) {
	size_t remainingDiagnosticBytes = kMaximumJobDiagnosticBytes;
	bool diagnosticsTruncated = false;
	nlohmann::json data{
		{"jobId", wstring_to_utf8(run.jobId)},
		{"stages", nlohmann::json::array()}};

	for (const auto& stage : run.stages) {
		nlohmann::json stageValue{
			{"stageId", wstring_to_utf8(stage.stageId)},
			{"code", ToString(stage.code)},
			{"skipped", stage.skipped},
			{"steps", nlohmann::json::array()}};
		for (const auto& step : stage.steps) {
			nlohmann::json diagnostics = nlohmann::json::array();
			for (const auto& item : step.diagnostics) {
				if (remainingDiagnosticBytes == 0) {
					diagnosticsTruncated = true;
					break;
				}

				const auto eventId = SanitizeUtf8(
					item.eventId, kMaximumSingleJobDiagnosticEventIdBytes);
				if (eventId.truncated) diagnosticsTruncated = true;
				const auto detail = SanitizeUtf8(item.detail, min(
					kMaximumSingleJobDiagnosticDetailBytes, remainingDiagnosticBytes));
				// 没有空间容纳一个完整 UTF-8 字符时，不写入空诊断对象，
				// 直接保留后续 step 的结果结构并标记发生了截断。
				if (!item.detail.empty() && detail.value.empty()) {
					diagnosticsTruncated = true;
					break;
				}

				diagnostics.push_back({
					{"eventId", eventId.value},
					{"severity", ToString(item.severity)},
					{"detail", detail.value}});
				remainingDiagnosticBytes -= detail.value.size();
				if (detail.truncated) {
					diagnosticsTruncated = true;
					break;
				}
			}
			stageValue["steps"].push_back({
				{"stepId", wstring_to_utf8(step.stepId)},
				{"code", ToString(step.code)},
				{"diagnostics", std::move(diagnostics)}});
		}
		data["stages"].push_back(std::move(stageValue));
	}

	if (diagnosticsTruncated) data["diagnosticsTruncated"] = true;
	return data;
}
