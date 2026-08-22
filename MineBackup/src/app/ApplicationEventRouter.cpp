#include "ApplicationEventRouter.h"

#include "AppPaths.h"
#include "AppState.h"
#include "CloudSyncService.h"
#include "ConfigManager.h"
#include "Globals.h"
#include "Logging.h"
#include "PortableConfigDocument.h"
#include "PlatformCompat.h"
#include "Sha256.h"
#include "i18n.h"
#include "text_to_text.h"

#include <climits>
#include <cerrno>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>

using namespace std;

namespace {
	namespace EventType {
		constexpr wchar_t TaskFailed[] = L"task-failed";
		constexpr wchar_t AutoBackupFinished[] = L"auto-backup-finished";
		constexpr wchar_t UpdateCheckComplete[] = L"update-check-complete";
		constexpr wchar_t NoticeCheckComplete[] = L"notice-check-complete";
		constexpr wchar_t RcloneInstallComplete[] = L"rclone-install-complete";
		constexpr wchar_t KnotLinkInstallerComplete[] = L"knotlink-installer-complete";
		constexpr wchar_t KnotLinkEnableComplete[] = L"knotlink-settings-enable-complete";
		constexpr wchar_t KnotLinkStartupStatus[] = L"knotlink-startup-status";
		constexpr wchar_t PortableConfigPreview[] = L"portable-config-preview";
	}

	namespace EventField {
		constexpr wchar_t Success[] = L"success";
		constexpr wchar_t Available[] = L"available";
		constexpr wchar_t Tag[] = L"tag";
		constexpr wchar_t Notes[] = L"notes";
		constexpr wchar_t Version[] = L"version";
		constexpr wchar_t NeedsUpdate[] = L"needs-update";
		constexpr wchar_t Content[] = L"content";
		constexpr wchar_t ContentId[] = L"content-id";
		constexpr wchar_t Path[] = L"path";
		constexpr wchar_t LocalFingerprint[] = L"local-fingerprint";
		constexpr wchar_t Preview[] = L"preview";
		constexpr wchar_t ConfigIndex[] = L"config-index";
		constexpr wchar_t Action[] = L"action";
		constexpr wchar_t Payload[] = L"payload";
	}

	const wstring* RequiredValue(const TaskEvent& event, const wchar_t* field) {
		const auto value = event.values.find(field);
		if (value != event.values.end()) return &value->second;
		MB_LOG_ERROR(
			minebackup::logging::LogCategory::Task,
			"task.event.missing_field",
			"Application event '{}' is missing required field '{}'",
			wstring_to_utf8(event.type),
			wstring_to_utf8(field));
		return nullptr;
	}

	bool TryParseIndex(const wstring& value, int& result) {
		wchar_t* end = nullptr;
		errno = 0;
		const long parsed = wcstol(value.c_str(), &end, 10);
		if (errno != 0 || end == value.c_str() || *end != L'\0'
			|| parsed < 0 || parsed > INT_MAX) {
			return false;
		}
		result = static_cast<int>(parsed);
		return true;
	}

	void HandlePortablePreview(const TaskEvent& event) {
		const wstring* success = RequiredValue(event, EventField::Success);
		if (!success) return;
		if (*success != L"1") {
			const wstring detail = event.message.empty()
				? L"Unable to prepare the portable configuration preview."
				: event.message;
			MB_LOG_ERROR(
				minebackup::logging::LogCategory::Cloud,
				"cloud.portable_config.prepare_failed",
				"{}",
				wstring_to_utf8(detail));
			MessageBoxWin(
				L("PORTABLE_CONFIG_TITLE"),
				L("PORTABLE_CONFIG_PREPARE_FAILED"),
				2);
			return;
		}

		const wstring* fingerprint =
			RequiredValue(event, EventField::LocalFingerprint);
		const wstring* preview = RequiredValue(event, EventField::Preview);
		const wstring* indexText = RequiredValue(event, EventField::ConfigIndex);
		const wstring* action = RequiredValue(event, EventField::Action);
		const wstring* payload = RequiredValue(event, EventField::Payload);
		if (!fingerprint || !preview || !indexText || !action || !payload) return;

		map<int, Config> currentConfigs;
		{
			lock_guard<mutex> lock(g_appState.configsMutex);
			currentConfigs = g_appState.configs;
		}
		const string currentPortable =
			PortableConfigDocument::FromLocalConfigs(currentConfigs).Serialize();
		Sha256 currentHash;
		currentHash.Update(currentPortable.data(), currentPortable.size());
		if (utf8_to_wstring(currentHash.FinalHex()) != *fingerprint) {
			MessageBoxWin(
				L("PORTABLE_CONFIG_TITLE"),
				L("PORTABLE_CONFIG_CHANGED"),
				2);
			return;
		}
		if (!ConfirmMessageBox(
			L("PORTABLE_CONFIG_PREVIEW_TITLE"),
			wstring_to_utf8(*preview))) {
			MB_LOG_INFO(
				minebackup::logging::LogCategory::Cloud,
				"cloud.portable_config.cancelled",
				"Portable configuration transfer cancelled after preview");
			return;
		}

		int configIndex = -1;
		if (!TryParseIndex(*indexText, configIndex)) {
			MB_LOG_ERROR(
				minebackup::logging::LogCategory::Task,
				"task.event.invalid_field",
				"Portable configuration event has invalid config-index '{}'",
				wstring_to_utf8(*indexText));
			return;
		}

		if (*action == L"upload") {
			Config cloudConfig;
			{
				lock_guard<mutex> lock(g_appState.configsMutex);
				const auto config = g_appState.configs.find(configIndex);
				if (config == g_appState.configs.end()) return;
				cloudConfig = config->second;
			}
			const string serialized = wstring_to_utf8(*payload);
			TaskCoordinator::Instance().Submit(
				L"Commit portable configuration upload",
				{TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity)},
				[cloudConfig, serialized](stop_token) {
					CommitPortableConfigUpload(cloudConfig, serialized);
				});
			return;
		}

		PortableConfigDocument remote;
		wstring parseError;
		PortableConfigMergePreview appliedPreview;
		if (!PortableConfigDocument::Parse(
			wstring_to_utf8(*payload),
			remote,
			parseError)) {
			MB_LOG_ERROR(
				minebackup::logging::LogCategory::Cloud,
				"cloud.portable_config.parse_failed",
				"Portable configuration parse failed: {}",
				wstring_to_utf8(parseError));
			MessageBoxWin(
				L("PORTABLE_CONFIG_TITLE"),
				L("PORTABLE_CONFIG_INVALID"),
				2);
			return;
		}

		bool applied = false;
		{
			lock_guard<mutex> lock(g_appState.configsMutex);
			applied = PortableConfigDocument::ApplyImport(
				g_appState.configs,
				remote,
				appliedPreview,
				parseError);
		}
		if (applied) {
			SaveConfigs();
			MB_LOG_INFO(
				minebackup::logging::LogCategory::Cloud,
				"cloud.portable_config.imported",
				"Portable configuration import applied after confirmation");
			return;
		}
		MB_LOG_ERROR(
			minebackup::logging::LogCategory::Cloud,
			"cloud.portable_config.apply_failed",
			"Portable configuration apply failed: {}",
			wstring_to_utf8(parseError));
		MessageBoxWin(
			L("PORTABLE_CONFIG_TITLE"),
			L("PORTABLE_CONFIG_INVALID"),
			2);
	}
}

void ApplicationEventRouter::Dispatch(const vector<TaskEvent>& events) const {
	for (const TaskEvent& event : events) DispatchOne(event);
}

void ApplicationEventRouter::DispatchOne(const TaskEvent& event) const {
	if (event.type == EventType::TaskFailed) {
		MB_LOG_ERROR(
			minebackup::logging::LogCategory::Task,
			"task.background.failed",
			"Background task failed: {}",
			wstring_to_utf8(event.message));
		return;
	}
	if (event.type == EventType::AutoBackupFinished) {
		lock_guard<mutex> lock(g_appState.task_mutex);
		for (auto task = g_appState.g_active_auto_backups.begin();
			task != g_appState.g_active_auto_backups.end();) {
			if (task->second.taskName == event.message) {
				task = g_appState.g_active_auto_backups.erase(task);
			}
			else {
				++task;
			}
		}
		return;
	}
	if (event.type == EventType::UpdateCheckComplete) {
		const wstring* available = RequiredValue(event, EventField::Available);
		const wstring* tag = RequiredValue(event, EventField::Tag);
		const wstring* notes = RequiredValue(event, EventField::Notes);
		const wstring* success = RequiredValue(event, EventField::Success);
		if (!available || !tag || !notes || !success) return;
		g_NewVersionAvailable = *available == L"1";
		g_LatestVersionStr = wstring_to_utf8(*tag);
		g_ReleaseNotes = wstring_to_utf8(*notes);
		g_UpdateCheckDone = true;
		if (*success != L"1" && !event.message.empty()) {
			MB_LOG_ERROR(
				minebackup::logging::LogCategory::Network,
				"network.update_check.failed",
				"Update check failed: {}",
				wstring_to_utf8(event.message));
		}
		return;
	}
	if (event.type == EventType::NoticeCheckComplete) {
		const wstring* available = RequiredValue(event, EventField::Available);
		const wstring* content = RequiredValue(event, EventField::Content);
		const wstring* contentId = RequiredValue(event, EventField::ContentId);
		const wstring* success = RequiredValue(event, EventField::Success);
		if (!available || !content || !contentId || !success) return;
		g_NewNoticeAvailable = *available == L"1";
		g_NoticeContent = wstring_to_utf8(*content);
		g_NoticeUpdatedAt = wstring_to_utf8(*contentId);
		g_NoticeCheckDone = true;
		if (*success != L"1" && !event.message.empty()) {
			MB_LOG_ERROR(
				minebackup::logging::LogCategory::Network,
				"network.notice_check.failed",
				"Notice check failed: {}",
				wstring_to_utf8(event.message));
		}
		return;
	}
	if (event.type == EventType::RcloneInstallComplete) {
		const wstring* success = RequiredValue(event, EventField::Success);
		if (!success) return;
		g_RcloneInstallRunning = false;
		g_RcloneInstallSucceeded = *success == L"1";
		if (g_RcloneInstallSucceeded) {
			const wstring* path = RequiredValue(event, EventField::Path);
			if (!path) return;
			g_RcloneInstallMessage = MineFormatMessage(
				"RCLONE_INSTALL_SUCCESS_FORMAT",
				wstring_to_utf8(*path).c_str());
		}
		else {
			g_RcloneInstallMessage = event.message.empty()
				? utf8_to_wstring(L("RCLONE_INSTALL_FAILED"))
				: event.message;
			MB_LOG_ERROR(
				minebackup::logging::LogCategory::Process,
				"process.rclone.install_failed",
				"rclone installation failed: {}",
				wstring_to_utf8(g_RcloneInstallMessage));
		}
		return;
	}
	if (event.type == EventType::KnotLinkInstallerComplete) {
		const wstring* success = RequiredValue(event, EventField::Success);
		if (!success) return;
		g_KnotLinkInstallRunning = false;
		g_KnotLinkInstallSucceeded = *success == L"1";
		g_KnotLinkInstallMessage = g_KnotLinkInstallSucceeded
			? utf8_to_wstring(L("KNOTLINK_INSTALL_OPENED"))
			: (event.message.empty()
				? utf8_to_wstring(L("KNOTLINK_INSTALL_FAILED"))
				: event.message);
		if (!g_KnotLinkInstallSucceeded) {
			MB_LOG_ERROR(
				minebackup::logging::LogCategory::Network,
				"network.knotlink_installer.failed",
				"KnotLinkService installer download/open failed: {}",
				wstring_to_utf8(g_KnotLinkInstallMessage));
		}
		return;
	}
	if (event.type == EventType::KnotLinkEnableComplete) {
		const wstring* success = RequiredValue(event, EventField::Success);
		if (!success) return;
		g_enableKnotLink = *success == L"1";
		if (g_enableKnotLink) {
			SaveConfigs();
		}
		else {
			MB_LOG_ERROR(
				minebackup::logging::LogCategory::Network,
				"network.knotlink.enable_failed",
				"KnotLink could not be enabled; the setting was restored.");
		}
		return;
	}
	if (event.type == EventType::KnotLinkStartupStatus) {
		const wstring* needsUpdate = RequiredValue(event, EventField::NeedsUpdate);
		const wstring* version = RequiredValue(event, EventField::Version);
		if (!needsUpdate || !version) return;
		g_KnotLinkStartupNeedsUpdate = *needsUpdate == L"1";
		g_KnotLinkStartupVersion = wstring_to_utf8(*version);
		g_KnotLinkStartupStatusReady = true;
		return;
	}
	if (event.type == EventType::PortableConfigPreview) {
		HandlePortablePreview(event);
	}
}
