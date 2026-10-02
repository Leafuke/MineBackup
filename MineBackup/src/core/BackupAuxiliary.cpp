#include "CompressionPolicy.h"
#include "BackupManager.h"
#include "GameSessionManager.h"
#include "HistoryManager.h"
#include "MigrationCoordinator.h"
#include "RuntimeRetentionService.h"
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
#include "PlatformCompat.h"
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

    BackupRequest request;
    request.config = config;
    request.world = {config.configId, backupName};
    request.sourcePath = othersPath;
    request.comment = comment;
    request.auxiliarySource = true;
    BackupServiceDependencies dependencies;
    dependencies.paths = GetAppPaths();
    dependencies.addHistory = [config](const HistoryEntry& entry) {
        const auto configs = SnapshotConfigState().configs;
        if (none_of(configs.begin(), configs.end(), [&](const auto& pair) {
            return pair.second.configId == config.configId;
        }) || MigrationCoordinator::IsHistoryPersistenceBlocked()) return false;
        const auto result = GetHistoryRepository().Mutate(config.configId, GetAppPaths().HistoryFile(), configs, true,
            [&](vector<HistoryEntry>& entries) {
                if (any_of(entries.begin(), entries.end(), [&](const auto& old) {
                    return old.worldName == entry.worldName && old.backupFile == entry.backupFile;
                })) return false;
                entries.push_back(entry); return true;
            });
        return result.changed && result.persisted;
    };
    dependencies.enforceRetention = [](const BackupRequest& req, const HistoryEntry& entry, stop_token token) {
        RuntimeRetentionService retention(GetHistoryRepository(), GetAppPaths().HistoryFile(), SnapshotConfigState().configs, GetAppPaths());
        retention.Enforce(req, entry, token);
    };
    const auto result = BackupService(std::move(dependencies)).Run(request, TaskCoordinator::CurrentStopToken());
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.severity == DiagnosticSeverity::Error)
            BACKUP_ERROR("%s: %s", diagnostic.eventId.c_str(), diagnostic.detail.c_str());
        else if (diagnostic.severity == DiagnosticSeverity::Warning)
            BACKUP_WARNING("%s: %s", diagnostic.eventId.c_str(), diagnostic.detail.c_str());
    }

	BACKUP_INFO(L("LOG_BACKUP_OTHERS_END"));
}

// Resolve each timer tick using the captured stable configuration and source path.
void AutoBackupThreadFunction(int configIdx, int worldIdx, int intervalMinutes, stop_token stopToken, const MyFolder* initialTarget) {
	minebackup::logging::ScopedLogContext taskContext{{
		"config_index", std::to_string(configIdx)},
		{"world_index", std::to_string(worldIdx)},
		{"task", "automatic_backup"}};
    MyFolder initial;
    if (initialTarget) initial = *initialTarget;
    else {
        const auto configs = SnapshotConfigState().configs;
        const auto found = configs.find(configIdx);
        if (found == configs.end() || worldIdx < 0 || static_cast<size_t>(worldIdx) >= found->second.worlds.size()) return;
        const auto& [worldName, description] = found->second.worlds[worldIdx];
        initial = {JoinPath(found->second.saveRoot, worldName).wstring(), worldName, description, found->second, configIdx, worldIdx};
    }
    const auto identity = GameSessionWorldKey(initial);
    if (!ResolveSessionWorld(SnapshotConfigState().configs, identity)) return;
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
        const auto folder = ResolveSessionWorld(SnapshotConfigState().configs, identity);
        if (!folder) return;
        TaskCoordinator::Instance().Submit(L"automatic backup run",
            {TaskCoordinator::WorldResourceKey(identity.first, folder->path)}, [identity](stop_token token) {
                const auto current = ResolveSessionWorld(SnapshotConfigState().configs, identity);
                if (!token.stop_requested() && current) DoBackup(*current);
            });
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
		const int normalizedZipLevel = CompressionPolicy::NormalizeLevel(tempConfig.zipMethod, tempConfig.zipLevel);
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
