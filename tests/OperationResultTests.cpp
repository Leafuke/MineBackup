#include "OperationResultTests.h"

#include "CliRenderer.h"
#include "OperationResult.h"
#include "json.hpp"

#include <array>
#include <string>
#include <vector>

void RunOperationResultTests(TestContext& test) {
	struct ExpectedCode {
		OperationCode code;
		const char* name;
		int exitCode;
	};
	const std::array expected{
		ExpectedCode{OperationCode::Success, "success", 0},
		ExpectedCode{OperationCode::NoChanges, "no_changes", 0},
		ExpectedCode{OperationCode::InvalidArguments, "invalid_arguments", 2},
		ExpectedCode{OperationCode::ProfileBusy, "profile_busy", 3},
		ExpectedCode{OperationCode::TargetNotFound, "target_not_found", 4},
		ExpectedCode{OperationCode::MigrationRequired, "migration_required", 5},
		ExpectedCode{OperationCode::InvalidProfile, "invalid_profile", 5},
		ExpectedCode{OperationCode::ToolUnavailable, "tool_unavailable", 8},
		ExpectedCode{OperationCode::BackupFailed, "backup_failed", 6},
		ExpectedCode{OperationCode::JobFailed, "job_failed", 6},
		ExpectedCode{OperationCode::VerificationFailed, "verification_failed", 6},
		ExpectedCode{OperationCode::RestoreFailed, "restore_failed", 7},
		ExpectedCode{OperationCode::Cancelled, "cancelled", 9},
		ExpectedCode{OperationCode::PartialSuccess, "partial_success", 10}};
	for (const auto& item : expected) {
		test.Expect(std::string(ToString(item.code)) == item.name,
			"operation code should have a stable JSON value");
		test.Expect(ToExitCode(item.code) == item.exitCode,
			"operation code should have the documented process exit code");
	}
	test.Expect(IsSuccessful(OperationCode::Success)
			&& IsSuccessful(OperationCode::NoChanges)
			&& !IsSuccessful(OperationCode::PartialSuccess),
		"only success and no_changes should be fully successful");

	test.Expect(std::string(ToString(DiagnosticSeverity::Info)) == "info"
			&& std::string(ToString(DiagnosticSeverity::Warning)) == "warning"
			&& std::string(ToString(DiagnosticSeverity::Error)) == "error",
		"diagnostic severity values should be stable and non-localized");

	CliResult small{"job.run", OperationCode::Success};
	small.data["value"] = "ordinary";
	const auto ordinary = SerializeCliEnvelope(small);
	const auto boundedOrdinary = SerializeCliEnvelopeForControlChannel(small);
	test.Expect(nlohmann::json::parse(ordinary) == nlohmann::json::parse(boundedOrdinary)
			&& !nlohmann::json::parse(boundedOrdinary).contains("responseTruncated"),
		"Control-channel serialization should preserve ordinary envelope semantics");

	std::string escaping;
	escaping.reserve(4u * 1024u * 1024u);
	const std::string escapingUnit = "\"\\\n\r\t";
	while (escaping.size() < 4u * 1024u * 1024u) escaping += escapingUnit;
	CliResult failed{"job.run", OperationCode::JobFailed};
	failed.data["escaping"] = escaping;
	const auto failedPayload = SerializeCliEnvelopeForControlChannel(failed);
	const auto failedEnvelope = nlohmann::json::parse(failedPayload, nullptr, false);
	bool hasTruncationDiagnostic = false;
	if (!failedEnvelope.is_discarded() && failedEnvelope.contains("diagnostics")) {
		for (const auto& diagnostic : failedEnvelope["diagnostics"]) {
			hasTruncationDiagnostic |= diagnostic.value("eventId", "")
				== "cli.response.truncated";
		}
	}
	test.Expect(failedPayload.size() <= kMaximumControlCliEnvelopeBytes
			&& !failedEnvelope.is_discarded()
			&& failedEnvelope["command"] == "job.run"
			&& failedEnvelope["code"] == "job_failed"
			&& !failedEnvelope["ok"].get<bool>()
			&& failedEnvelope["data"].value("responseTruncated", false)
			&& hasTruncationDiagnostic,
		"Oversized failed control responses should use a bounded semantic fallback");

	CliResult successful{"job.run", OperationCode::Success};
	successful.data["invalidUtf8"] = std::string("bad\xFF", 4);
	const auto successfulPayload = SerializeCliEnvelopeForControlChannel(successful);
	const auto successfulEnvelope = nlohmann::json::parse(successfulPayload, nullptr, false);
	test.Expect(successfulPayload.size() <= kMaximumControlCliEnvelopeBytes
			&& !successfulEnvelope.is_discarded()
			&& successfulEnvelope["command"] == "job.run"
			&& successfulEnvelope["code"] == "success"
			&& successfulEnvelope["ok"].get<bool>()
			&& successfulEnvelope["data"].value("responseTruncated", false),
		"Serialization exceptions should preserve successful control-response semantics");
}
