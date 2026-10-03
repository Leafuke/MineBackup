#pragma once

#include "DataModels.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

enum class OperationCode {
	Success,
	NoChanges,
	InvalidArguments,
	ProfileBusy,
	TargetNotFound,
	MigrationRequired,
	InvalidProfile,
	ToolUnavailable,
	BackupFailed,
	JobFailed,
	VerificationFailed,
	RestoreFailed,
	Cancelled,
	PartialSuccess
};

enum class DiagnosticSeverity {
	Info,
	Warning,
	Error
};

struct Diagnostic {
	std::string eventId;
	DiagnosticSeverity severity = DiagnosticSeverity::Info;
	std::string detail;
};

struct WorldRef {
	std::wstring configId;
	std::wstring relativePath;
};

enum class BackupOutcome {
	Created,
	NoChanges,
	Failed,
	Rejected
};

enum class CloudPostStatus {
	Skipped,
	Succeeded,
	Failed
};

struct CloudPostResult {
	CloudPostStatus status = CloudPostStatus::Skipped;
	std::vector<Diagnostic> diagnostics;
};

struct BackupResult {
	OperationCode code = OperationCode::BackupFailed;
	BackupOutcome outcome = BackupOutcome::Failed;
	std::filesystem::path archivePath;
	std::optional<HistoryEntry> historyEntry;
	CloudPostResult cloud;
	std::vector<Diagnostic> diagnostics;
};

const char* ToString(OperationCode code) noexcept;
const char* ToString(DiagnosticSeverity severity) noexcept;
const char* ToString(BackupOutcome outcome) noexcept;
const char* ToString(CloudPostStatus status) noexcept;

bool IsSuccessful(OperationCode code) noexcept;
int ToExitCode(OperationCode code) noexcept;
