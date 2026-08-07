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
	case OperationCode::TaskFailed: return "task_failed";
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
	case OperationCode::TaskFailed:
		return 6;
	case OperationCode::ToolUnavailable:
		return 8;
	case OperationCode::Cancelled:
		return 9;
	case OperationCode::PartialSuccess:
		return 10;
	}
	return 5;
}

OperationCode AggregateSpecialTaskCodes(
	const std::vector<SpecialTaskResult>& tasks) noexcept {
	if (tasks.empty()) return OperationCode::Success;
	bool hasSuccess = false;
	bool hasFailure = false;
	bool allNoChanges = true;
	for (const auto& task : tasks) {
		if (task.code == OperationCode::Cancelled) return OperationCode::Cancelled;
		if (IsSuccessful(task.code)) {
			hasSuccess = true;
			if (task.code != OperationCode::NoChanges) allNoChanges = false;
		}
		else {
			hasFailure = true;
		}
	}
	if (hasSuccess && hasFailure) return OperationCode::PartialSuccess;
	if (hasFailure) return OperationCode::TaskFailed;
	return allNoChanges ? OperationCode::NoChanges : OperationCode::Success;
}
