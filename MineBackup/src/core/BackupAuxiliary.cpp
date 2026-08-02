#include "BackupManager.h"
#include "BackupManagerInternal.h"

#include "AppPaths.h"
#include "BackupChangeDetector.h"
#include "ConfigManager.h"
#include "DesktopServices.h"
#include "FolderRewindFormat.h"
#include "FolderRewindMetadataStore.h"
#include "Globals.h"
#include "Logging.h"
#include "PathRuleSet.h"
#include "TaskCoordinator.h"
#include "text_to_text.h"
#include "i18n.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

using namespace std;
using namespace BackupManagerInternal;

#define BACKUP_INFO(...) MB_LOG_PRINTF_INFO(minebackup::logging::LogCategory::Backup, "backup.progress", __VA_ARGS__)
#define BACKUP_WARNING(...) MB_LOG_PRINTF_WARNING(minebackup::logging::LogCategory::Backup, "backup.warning", __VA_ARGS__)
#define BACKUP_ERROR(...) MB_LOG_PRINTF_ERROR(minebackup::logging::LogCategory::Backup, "backup.error", __VA_ARGS__)

void AddBackupToWESnapshots(const Config& config, const wstring& worldName, const wstring& backupFile) {
	minebackup::logging::ScopedLogContext operationContext{{
		"operation_id", wstring_to_utf8(FolderRewindFormat::GenerateGuidString())},
		{"config_id", wstring_to_utf8(config.configId)},
		{"world", wstring_to_utf8(worldName)}};
	BACKUP_INFO(L("LOG_WE_INTEGRATION_START"), wstring_to_utf8(worldName).c_str());
	if (config.enableWEIntegration && !config.weSnapshotPath.empty() && !IsAsciiOnlyPath(config.weSnapshotPath)) {
		BACKUP_ERROR(L("ERROR_NON_ASCII_PATH"));
		BACKUP_ERROR(L("LOG_WE_INTEGRATION_FAILED"));
		return;
	}

	// 创建快照路径
	filesystem::path we_base_path = config.weSnapshotPath;
	if (we_base_path.empty()) {
		we_base_path = GetDocumentsPath();
		if (we_base_path.empty()) {
			BACKUP_ERROR("Could not determine Documents folder path.");
			BACKUP_ERROR(L("LOG_WE_INTEGRATION_FAILED"));
			return;
		}
		we_base_path /= "MineBackup-WE-Snap";
	}

	auto now = chrono::system_clock::now();
	auto in_time_t = chrono::system_clock::to_time_t(now);
	wstringstream ss;
	tm t;
	localtime_s(&t, &in_time_t);
	ss << put_time(&t, L"%Y-%m-%d-%H-%M-%S");

	filesystem::path final_snapshot_path = we_base_path / worldName / ss.str();

	error_code ec;
	filesystem::create_directories(final_snapshot_path, ec);
	if (ec) {
		BACKUP_ERROR("Failed to create snapshot directory: %s", ec.message().c_str());
		BACKUP_ERROR(L("LOG_WE_INTEGRATION_FAILED"));
		return;
	}
	BACKUP_INFO(L("LOG_WE_INTEGRATION_PATH_OK"), wstring_to_utf8(final_snapshot_path.wstring()).c_str());

	// WorldEdit 快照需要的核心文件/文件夹
	const vector<wstring> essential_parts = { L"region", L"poi", L"entities", L"level.dat" };

	// 还原链处理
	filesystem::path sourceDir = JoinPath(config.backupPath, worldName);
	filesystem::path targetBackupPath = sourceDir / backupFile;

	if ((backupFile.find(L"[Smart]") == wstring::npos && backupFile.find(L"[Full]") == wstring::npos) || !filesystem::exists(targetBackupPath)) {
		BACKUP_ERROR(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(backupFile).c_str());
		return;
	}

	// 收集所有相关的备份文件
	vector<filesystem::path> backupsToApply;

	if (backupFile.find(L"[Smart]") != wstring::npos) {
		// 寻找基础的完整备份
		filesystem::path baseFullBackup;
		auto baseFullTime = filesystem::file_time_type{};

		for (const auto& entry : filesystem::directory_iterator(sourceDir)) {
			if (entry.is_regular_file() && FolderRewindFormat::IsFullLikeBackupType(entry.path().filename().wstring())) {
				if (entry.last_write_time() < filesystem::last_write_time(targetBackupPath) && entry.last_write_time() > baseFullTime) {
					baseFullTime = entry.last_write_time();
					baseFullBackup = entry.path();
				}
			}
		}

		if (baseFullBackup.empty()) {
			BACKUP_ERROR(L("LOG_BACKUP_SMART_NO_FOUND"));
			return;
		}

		BACKUP_INFO(L("LOG_BACKUP_SMART_FOUND"), wstring_to_utf8(baseFullBackup.filename().wstring()).c_str());
		backupsToApply.push_back(baseFullBackup);

		// 收集从基础备份到目标备份之间的所有增量备份
		for (const auto& entry : filesystem::directory_iterator(sourceDir)) {
			if (entry.is_regular_file() && entry.path().filename().wstring().find(L"[Smart]") != wstring::npos) {
				if (entry.last_write_time() > baseFullTime && entry.last_write_time() <= filesystem::last_write_time(targetBackupPath)) {
					backupsToApply.push_back(entry.path());
				}
			}
		}
		// 按时间顺序排序
		sort(backupsToApply.begin(), backupsToApply.end(), [](const auto& a, const auto& b) {
			return filesystem::last_write_time(a) < filesystem::last_write_time(b);
			});
	}
	else {
		backupsToApply.push_back(targetBackupPath);
	}

	// 依次解压核心文件/文件夹
	for (size_t i = 0; i < backupsToApply.size(); ++i) {
		const auto& backup = backupsToApply[i];
		BACKUP_INFO(L("RESTORE_STEPS"), i + 1, backupsToApply.size(), wstring_to_utf8(backup.filename().wstring()).c_str());
		vector<wstring> arguments = {L"x", backup.wstring(), L"-o" + final_snapshot_path.wstring()};
		arguments.insert(arguments.end(), essential_parts.begin(), essential_parts.end());
		arguments.push_back(L"-r");
		arguments.push_back(L"-y");
		if (!RunInternalProcess(MakeInternalProcess(config.zipPath, std::move(arguments), {}, config.useLowPriority))) {
			BACKUP_ERROR(L("LOG_WE_INTEGRATION_FAILED"));
			return;
		}
	}

	// 修改 WorldEdit 配置文件（与原有实现一致）
	BACKUP_INFO(L("LOG_WE_INTEGRATION_CONFIG_UPDATE_START"));
	filesystem::path save_root(config.saveRoot);
	filesystem::path we_config_path;
	if (filesystem::exists(save_root.parent_path() / "config" / "worldedit" / "worldedit.properties")) {
		we_config_path = save_root.parent_path() / "config" / "worldedit" / "worldedit.properties";
	}
	else if (filesystem::exists(save_root / "config" / "worldedit" / "worldedit.properties")) {
		we_config_path = save_root / "config" / "worldedit" / "worldedit.properties";
	}
	else if (filesystem::exists(save_root / "worldedit.conf")) {
		we_config_path = save_root / "worldedit.conf";
	}

	if (!filesystem::exists(we_config_path)) {
		BACKUP_INFO(L("LOG_WE_INTEGRATION_CONFIG_NOT_FOUND"), wstring_to_utf8(we_config_path.wstring()).c_str());
		BACKUP_INFO(L("LOG_WE_INTEGRATION_SUCCESS"), wstring_to_utf8(worldName).c_str());
		return;
	}

	ifstream infile(we_config_path);
	vector<string> lines;
	string line;
	bool key_found = false;
	string new_line = "snapshots-dir=" + wstring_to_utf8(we_base_path.wstring());
	replace(new_line.begin(), new_line.end(), '\\', '/');

	while (getline(infile, line)) {
		if (line.rfind("snapshots-dir=", 0) == 0) {
			lines.push_back(new_line);
			key_found = true;
		}
		else {
			lines.push_back(line);
		}
	}
	infile.close();

	if (!key_found) {
		lines.push_back(new_line);
	}

	ofstream outfile(we_config_path);
	if (outfile.is_open()) {
		for (const auto& l : lines) {
			outfile << l << endl;
		}
		outfile.close();
		BACKUP_INFO(L("LOG_WE_INTEGRATION_CONFIG_UPDATE_SUCCESS"));
	}
	else {
		BACKUP_ERROR(L("LOG_WE_INTEGRATION_CONFIG_UPDATE_FAIL"));
		BACKUP_ERROR(L("LOG_WE_INTEGRATION_FAILED"));
		return;
	}

	BACKUP_INFO(L("LOG_WE_INTEGRATION_SUCCESS"), wstring_to_utf8(worldName).c_str());
}

void DoOthersBackup(const Config& config, filesystem::path backupWhat, const wstring& comment) {
	minebackup::logging::ScopedLogContext operationContext{{
		"operation_id", wstring_to_utf8(FolderRewindFormat::GenerateGuidString())},
		{"config_id", wstring_to_utf8(config.configId)},
		{"task", "folder_backup"}};
	if (config.pendingLocalBinding) {
		BACKUP_WARNING("This imported configuration is waiting for local path binding.");
		return;
	}
	BACKUP_INFO(L("LOG_BACKUP_OTHERS_START"));

	filesystem::path othersPath = backupWhat;
	backupWhat = backupWhat.filename().wstring();
	const std::wstring backupName = backupWhat.wstring();

	if (!filesystem::exists(othersPath) || !filesystem::is_directory(othersPath)) {
		BACKUP_ERROR(L("LOG_ERROR_OTHERS_NOT_FOUND"), wstring_to_utf8(othersPath.wstring()).c_str());
		BACKUP_INFO(L("LOG_BACKUP_OTHERS_END"));
		return;
	}

	FolderRewindFormat::StoragePaths storagePaths;
	if (!FolderRewindFormat::TryResolveStoragePaths(config.backupPath, backupName, othersPath.wstring(), storagePaths)) {
		BACKUP_ERROR("Invalid FolderRewind storage folder name for backup target: %s", wstring_to_utf8(backupName).c_str());
		BACKUP_INFO(L("LOG_BACKUP_OTHERS_END"));
		return;
	}

	filesystem::path destinationFolder = storagePaths.backupSubDir;
	wstring archiveFileName = FolderRewindFormat::GenerateArchiveFileName(L"Full", storagePaths.folderName, comment, config.zipFormat);
	wstring archivePath = (destinationFolder / archiveFileName).wstring();

	try {
		filesystem::create_directories(destinationFolder);
		filesystem::create_directories(storagePaths.metadataDir);
		BACKUP_INFO(L("LOG_BACKUP_DIR_IS"), wstring_to_utf8(destinationFolder.wstring()).c_str());
	}
	catch (const filesystem::filesystem_error& e) {
		BACKUP_ERROR(L("LOG_ERROR_CREATE_BACKUP_DIR"), e.what());
		BACKUP_INFO(L("LOG_BACKUP_OTHERS_END"));
		return;
	}

	BackupScanResult scanResult = BackupChangeDetector{}.Scan(
		othersPath,
		storagePaths.metadataDir,
		storagePaths.backupSubDir);
	if (scanResult.status == BackupScanStatus::ScanFailed) {
		BACKUP_ERROR("Failed to scan source directory for backup state.");
		BACKUP_INFO(L("LOG_BACKUP_OTHERS_END"));
		return;
	}
	auto currentState = std::move(scanResult.currentState);
	auto changeSet = std::move(scanResult.changes);
	changeSet.addedFiles.clear();
	for (const auto& pair : currentState) {
		changeSet.addedFiles.push_back(pair.first);
	}
	sort(changeSet.addedFiles.begin(), changeSet.addedFiles.end());
	changeSet.modifiedFiles.clear();
	changeSet.deletedFiles.clear();

	const int normalizedZipLevel = NormalizeCompressionLevel(config.zipMethod, config.zipLevel);
	auto arguments = SevenZipCreateArguments(config, normalizedZipLevel, archivePath);
	arguments.push_back(othersPath.wstring() + L"\\*");

	if (RunInternalProcess(MakeInternalProcess(config.zipPath, std::move(arguments), {}, config.useLowPriority))) {
		if (!UpdateMetadataFiles(storagePaths.metadataDir, archiveFileName, archiveFileName, L"Full", currentState, changeSet)) {
			BACKUP_ERROR("Failed to write FolderRewind metadata for backup: %s", wstring_to_utf8(archiveFileName).c_str());
			BACKUP_INFO(L("LOG_BACKUP_OTHERS_END"));
			return;
		}
		LimitBackupFiles(config, g_appState.realConfigIndex, destinationFolder.wstring(), config.keepCount);
		AddHistoryEntry(g_appState.currentConfigIndex, storagePaths.folderName, archiveFileName, L"Full", comment, othersPath.wstring());
	}

	BACKUP_INFO(L("LOG_BACKUP_OTHERS_END"));
}

// 避免仅以 worldIdx 作为 key 导致的冲突，使用{ configIdx, worldIdx }
void AutoBackupThreadFunction(int configIdx, int worldIdx, int intervalMinutes, stop_token stopToken) {
	minebackup::logging::ScopedLogContext taskContext{{
		"config_index", std::to_string(configIdx)},
		{"world_index", std::to_string(worldIdx)},
		{"task", "automatic_backup"}};
	{
		lock_guard<mutex> lock(g_appState.configsMutex);
		auto it = g_appState.configs.find(configIdx);
		if (it == g_appState.configs.end() || it->second.pendingLocalBinding) {
			BACKUP_WARNING("Automatic backup is disabled until local paths are bound.");
			return;
		}
	}
	BACKUP_INFO(L("LOG_AUTOBACKUP_START"), worldIdx, intervalMinutes);

	while (!stopToken.stop_requested()) {
		mutex waitMutex;
		condition_variable_any waitCondition;
		unique_lock waitLock(waitMutex);
		if (waitCondition.wait_for(waitLock, stopToken, chrono::minutes(intervalMinutes), [] { return false; })) {
			continue;
		}
		if (stopToken.stop_requested()) {
			BACKUP_INFO(L("LOG_AUTOBACKUP_STOPPED"), worldIdx);
			return;
		}

		BACKUP_INFO(L("LOG_AUTOBACKUP_ROUTINE"), worldIdx);
		MyFolder folder;
		{
			lock_guard<mutex> lock(g_appState.configsMutex);
			if (g_appState.configs.count(configIdx) && worldIdx >= 0 && worldIdx < g_appState.configs[configIdx].worlds.size()) {
				folder = {
					JoinPath(g_appState.configs[configIdx].saveRoot, g_appState.configs[configIdx].worlds[worldIdx].first).wstring(),
					g_appState.configs[configIdx].worlds[worldIdx].first,
					g_appState.configs[configIdx].worlds[worldIdx].second,
					g_appState.configs[configIdx],
					configIdx,
					worldIdx
				};
			}
			else {
				BACKUP_ERROR(L("ERROR_INVALID_WORLD_IN_TASK"), configIdx, worldIdx);
				return;
			}
		}
		TaskCoordinator::Instance().Submit(L"automatic backup run",
			{ TaskCoordinator::WorldResourceKey(folder.config.configId, folder.path) },
			[folder](stop_token) { DoBackup(folder); });
	}
}

void DoExportForSharing(Config tempConfig, wstring worldName, wstring worldPath, wstring outputPath, wstring description) {
	BACKUP_INFO(L("LOG_EXPORT_STARTED"), wstring_to_utf8(worldName).c_str());

	// 准备临时文件和路径
	filesystem::path temp_export_dir = GetAppPaths().runtimeRoot /
		(L"MineBackup_Export_" + FolderRewindFormat::GenerateGuidString());
	ScopedRuntimeArtifact tempExportCleanup(temp_export_dir);
	filesystem::path readme_path = temp_export_dir / L"readme.txt";

	try {
		// 清理并创建临时工作目录
		if (filesystem::exists(temp_export_dir)) {
			filesystem::remove_all(temp_export_dir);
		}
		filesystem::create_directories(temp_export_dir);

		// 如果有描述，创建 readme.txt
		if (!description.empty()) {
			ofstream readme_file(readme_path, ios::binary);
			if (readme_file.is_open()) {
				auto write_line = [&readme_file](const wstring& line) {
					string utf8 = wstring_to_utf8(line);
					readme_file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
					readme_file.put('\n');
				};

				write_line(L"[Name]");
				write_line(worldName);
				readme_file.put('\n');
				write_line(L"[Description]");
				write_line(description);
				readme_file.put('\n');
				write_line(L"[Exported by MineBackup]");
			}
		}

		// 收集并过滤文件
		vector<filesystem::path> files_to_export;
		const PathRuleSet exportRules(tempConfig.blacklist);
		for (const auto& entry : filesystem::recursive_directory_iterator(worldPath)) {
			if (!exportRules.Matches(entry.path(), worldPath, worldPath)) {
				files_to_export.push_back(entry.path());
			}
		}

		// 将 readme.txt 也加入待压缩列表
		if (!description.empty()) {
			files_to_export.push_back(readme_path);
		}

		if (files_to_export.empty()) {
			BACKUP_ERROR("No files left to export after applying blacklist.");
			return;
		}

		// 创建文件列表供 7z 使用
		wstring filelist_path = (temp_export_dir / L"filelist.txt").wstring();
		ofstream ofs{std::filesystem::path(filelist_path), ios::binary};
		for (const auto& file : files_to_export) {
			string utf8Path;
			if (file.wstring().rfind(worldPath, 0) == 0) {
				utf8Path = wstring_to_utf8(filesystem::relative(file, worldPath).wstring());
			}
			else {
				utf8Path = wstring_to_utf8(file.wstring());
			}
			ofs.write(utf8Path.data(), static_cast<std::streamsize>(utf8Path.size()));
			ofs.put('\n');
		}
		ofs.close();

		// 构建并执行 7z 命令
		const int normalizedZipLevel = NormalizeCompressionLevel(tempConfig.zipMethod, tempConfig.zipLevel);
		auto arguments = SevenZipCreateArguments(tempConfig, normalizedZipLevel, outputPath);
		arguments.push_back(L"@" + filelist_path);

		// 工作目录应为原始世界路径，以确保压缩包内路径正确
		if (RunInternalProcess(MakeInternalProcess(tempConfig.zipPath, std::move(arguments), worldPath,
			tempConfig.useLowPriority))) {
			BACKUP_INFO(L("LOG_EXPORT_SUCCESS"), wstring_to_utf8(outputPath).c_str());
			(void)GetDesktopServices()->RevealInFolder(
				filesystem::path(outputPath).parent_path(), filesystem::path(outputPath));
		}
		else {
			BACKUP_ERROR(L("LOG_EXPORT_FAILED"));
		}

	}
	catch (const exception& e) {
		BACKUP_ERROR("An exception occurred during export: %s", e.what());
	}

}
