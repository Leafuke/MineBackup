#include "OperationResultTests.h"

#include "OperationResult.h"

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
}
