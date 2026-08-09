#include "OperationResult.h"

const char* ToString(OperationCode code) noexcept {
	switch (code) {
	case OperationCode::Success: return "success";
	case OperationCode::NoChanges: return "no_changes";
	case OperationCode::InvalidArguments: return "invalid_arguments";
	case OperationCode::ProfileBusy: return "profile_busy";
	case OperationCode::TargetNotFound: return "target_not_found";
	case OperationCode::MigrationRequired: return "migration_required";
	case OperationCode::InvalidProfile: return "invalid_profile";
	case OperationCode::ToolUnavailable: return "tool_unavailable";
	case OperationCode::BackupFailed: return "backup_failed";
	case OperationCode::JobFailed: return "job_failed";
	case OperationCode::VerificationFailed: return "verification_failed";
	case OperationCode::RestoreFailed: return "restore_failed";
	case OperationCode::Cancelled: return "cancelled";
	case OperationCode::PartialSuccess: return "partial_success";
	}
	return "invalid_profile";
}

const char* ToString(DiagnosticSeverity severity) noexcept {
	switch (severity) {
	case DiagnosticSeverity::Info: return "info";
	case DiagnosticSeverity::Warning: return "warning";
	case DiagnosticSeverity::Error: return "error";
	}
	return "error";
}

const char* ToString(BackupOutcome outcome) noexcept {
	switch (outcome) {
	case BackupOutcome::Created: return "created";
	case BackupOutcome::NoChanges: return "no_changes";
	case BackupOutcome::Failed: return "failed";
	case BackupOutcome::Rejected: return "rejected";
	}
	return "failed";
}

const char* ToString(CloudPostStatus status) noexcept {
	switch (status) {
	case CloudPostStatus::Skipped: return "skipped";
	case CloudPostStatus::Succeeded: return "succeeded";
	case CloudPostStatus::Failed: return "failed";
	}
	return "failed";
}

bool IsSuccessful(OperationCode code) noexcept {
	return code == OperationCode::Success || code == OperationCode::NoChanges;
}

int ToExitCode(OperationCode code) noexcept {
	switch (code) {
	case OperationCode::Success:
	case OperationCode::NoChanges:
		return 0;
	case OperationCode::InvalidArguments:
		return 2;
	case OperationCode::ProfileBusy:
		return 3;
	case OperationCode::TargetNotFound:
		return 4;
	case OperationCode::MigrationRequired:
	case OperationCode::InvalidProfile:
		return 5;
	case OperationCode::BackupFailed:
	case OperationCode::JobFailed:
	case OperationCode::VerificationFailed:
		return 6;
	case OperationCode::RestoreFailed:
		return 7;
	case OperationCode::ToolUnavailable:
		return 8;
	case OperationCode::Cancelled:
		return 9;
	case OperationCode::PartialSuccess:
		return 10;
	}
	return 5;
}
