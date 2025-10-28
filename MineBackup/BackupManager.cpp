#include "Broadcast.h"
#include "BackupManager.h"
#include "AppState.h"
#include "text_to_text.h"
#include "i18n.h"
#include "Console.h"
#include "HistoryManager.h"
#include "json.hpp"
#include <filesystem>
#include <sstream>
using namespace std;


extern enum class BackupCheckResult {
	NO_CHANGE,
	CHANGES_DETECTED,
	FORCE_FULL_BACKUP_METADATA_INVALID,
	FORCE_FULL_BACKUP_BASE_MISSING
};

wstring SanitizeFileName(const wstring& input);
bool RunCommandInBackground(wstring command, Console& console, bool useLowPriority, const wstring& workingDirectory = L"");
vector<filesystem::path> GetChangedFiles(const filesystem::path& worldPath, const filesystem::path& metadataPath, const filesystem::path& backupPath, BackupCheckResult& out_result, map<wstring, size_t>& out_currentState);
bool is_blacklisted(const filesystem::path& file_to_check, const filesystem::path& backup_source_root, const filesystem::path& original_world_root, const vector<wstring>& blacklist);
bool IsFileLocked(const wstring& path);

extern vector<wstring> restoreWhitelist;
extern bool isSafeDelete;



// �������գ������ȱ���
wstring CreateWorldSnapshot(const filesystem::path& worldPath, const wstring& snapshotPath, Console& console) {
	try {
		// ����һ��Ψһ����ʱĿ¼
		filesystem::path tempDir;
		if (snapshotPath.size() >= 2 && filesystem::exists(snapshotPath)) {
			tempDir = snapshotPath + L"\\MineBackup_Snapshot\\" + worldPath.filename().wstring();
		}
		else {
			tempDir = filesystem::temp_directory_path() / L"MineBackup_Snapshot" / worldPath.filename();
		}

		// ����ɵ���ʱĿ¼���ڣ��������
		if (filesystem::exists(tempDir)) {
			error_code ec_remove;
			filesystem::remove_all(tempDir, ec_remove);
			if (ec_remove) {
				console.AddLog("[Error] Failed to clean up old snapshot directory: %s", ec_remove.message().c_str());
				// ��ʹ����ʧ��Ҳ���Լ����������Ĵ������ܻ�ʧ�ܲ�������
			}
		}
		filesystem::create_directories(tempDir);
		console.AddLog(L("LOG_BACKUP_HOT_INFO"));

		// �ݹ鸴�ƣ������Ժ��Ե����ļ�����
		auto copyOptions = filesystem::copy_options::recursive | filesystem::copy_options::overwrite_existing;
		error_code ec;
		filesystem::copy(worldPath, tempDir, copyOptions, ec);

		if (ec) {
			// ��Ȼ�����˴��󣨿�����ĳ���ļ��������ˣ������󲿷��ļ������Ѿ����Ƴɹ�
			console.AddLog(L("LOG_BACKUP_HOT_INFO2"), ec.message().c_str());
			wstring xcopyCmd = L"xcopy \"" + worldPath.wstring() + L"\" \"" + tempDir.wstring() + L"\" /s /e /y /c";
			RunCommandInBackground(xcopyCmd, console, false);
		}
		else {
			console.AddLog(L("LOG_BACKUP_HOT_INFO3"), wstring_to_utf8(tempDir.wstring()).c_str());
		}
		// ���Ӷ�����ʱ��ȷ���ļ�ϵͳ�������ر��� xcopy����ȫ���
		this_thread::sleep_for(chrono::milliseconds(500));

		return tempDir.wstring();

	}
	catch (const filesystem::filesystem_error& e) {
		console.AddLog(L("LOG_BACKUP_HOT_INFO4"), e.what());
		return L"";
	}
}

void UpdateMetadataFile(const filesystem::path& metadataPath, const wstring& newBackupFile, const wstring& basedOnBackupFile, const map<wstring, size_t>& currentState) {
	filesystem::create_directories(metadataPath);
	filesystem::path metadataFile = metadataPath / L"metadata.json";

	nlohmann::json metadata;
	metadata["version"] = 1;
	metadata["lastBackupFile"] = wstring_to_utf8(newBackupFile);
	metadata["basedOnBackupFile"] = wstring_to_utf8(basedOnBackupFile);

	nlohmann::json fileStates = nlohmann::json::object();
	for (const auto& pair : currentState) {
		fileStates[wstring_to_utf8(pair.first)] = pair.second;
	}
	metadata["fileStates"] = fileStates;

	ofstream o(metadataFile, ios::trunc);
	o << metadata.dump(2); // �����ո�����
}


// ���Ʊ����ļ��������������Զ�ɾ����ɵ�
void LimitBackupFiles(const Config &config, const int &configIndex, const wstring& folderPath, int limit, Console* console)
{
	if (limit <= 0) return;
	namespace fs = filesystem;
	vector<fs::directory_entry> files;

	// �ռ����г����ļ�
	try {
		if (!fs::exists(folderPath) || !fs::is_directory(folderPath))
			return;
		for (const auto& entry : fs::directory_iterator(folderPath)) {
			if (entry.is_regular_file())
				files.push_back(entry);
		}
	}
	catch (const fs::filesystem_error& e) {
		if (console) console->AddLog(L("LOG_ERROR_SCAN_BACKUP_DIR"), e.what());
		return;
	}

	// ���δ�������ƣ����账��
	if ((int)files.size() <= limit) return;

	const auto& history_it = g_appState.g_history.find(configIndex);
	bool history_available = (history_it != g_appState.g_history.end());

	// �����д��ʱ������������ɵ���ǰ��
	sort(files.begin(), files.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
		return fs::last_write_time(a) < fs::last_write_time(b);
		});

	vector<fs::directory_entry> deletable_files;
	for (const auto& file : files) {
		bool is_important = false;
		if (history_available) {
			for (const auto& entry : history_it->second) {
				if (entry.worldName == file.path().parent_path().filename().wstring() && entry.backupFile == file.path().filename().wstring()) {
					if (entry.isImportant) {
						is_important = true;
					}
					break;
				}
			}
		}

		if (!is_important) {
			deletable_files.push_back(file);
		}
	}

	// �����ɾ�����ļ��������㣬�Ͳ�����ɾ��
	if ((int)files.size() - (int)deletable_files.size() >= limit) {
		if (console) console->AddLog("[Info] Cannot delete more files; remaining backups are marked as important.");
		return;
	}

	int to_delete_count = (int)files.size() - limit;
	for (int i = 0; i < to_delete_count && i < deletable_files.size(); ++i) {
		const auto& file_to_delete = deletable_files[i];
		try {
			if (files[i].path().filename().wstring().find(L"[Smart]") == 0 || files[i + 1].path().filename().wstring().find(L"[Smart]") == 0) // ��������ܱ��ݣ�����ɾ����������������ݣ������ǻ���
			{
				if (console) console->AddLog(L("LOG_WARNING_DELETE_SMART_BACKUP"), wstring_to_utf8(files[i].path().filename().wstring()).c_str());
			}
			
			if (isSafeDelete) {
				// �� history ���ҵ���һ���ȫɾ��
				if (history_available) {
					for (const auto& entry : history_it->second) {
						if (entry.worldName == file_to_delete.path().parent_path().filename().wstring() && entry.backupFile == file_to_delete.path().filename().wstring()) {
							DoSafeDeleteBackup(config, entry, configIndex, *console);
							break;
						}
					}
				}
			}
			else {
				fs::remove(file_to_delete);
			}
			if (console) console->AddLog(L("LOG_DELETE_OLD_BACKUP"), wstring_to_utf8(files[i].path().filename().wstring()).c_str());
		}
		catch (const fs::filesystem_error& e) {
			if (console) console->AddLog(L("LOG_ERROR_DELETE_BACKUP"), e.what());
		}
	}
}


// ִ�е�������ı��ݲ�����
// ����:
//   - config: ��ǰʹ�õ����á�
//   - world:  Ҫ���ݵ����磨����+��������
//   - console: ���̨��������ã����������־��Ϣ��
   //- commend: �û�ע��
void DoBackup(const Config config, const pair<wstring, wstring> world, Console& console, const wstring& comment) {
	console.AddLog(L("LOG_BACKUP_START_HEADER"));
	console.AddLog(L("LOG_BACKUP_PREPARE"), wstring_to_utf8(world.first).c_str());

	if (!filesystem::exists(config.zipPath)) {
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND"), wstring_to_utf8(config.zipPath).c_str());
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND_HINT"));
		return;
	}

	// ׼��·��
	wstring originalSourcePath = config.saveRoot + L"\\" + world.first;
	wstring sourcePath = originalSourcePath; // Ĭ��ʹ��ԭʼ·��
	wstring destinationFolder = config.backupPath + L"\\" + world.first;
	wstring metadataFolder = config.backupPath + L"\\_metadata\\" + world.first; // Ԫ�����ļ���
	wstring command;
	wstring archivePath;
	wstring archiveNameBase = world.second.empty() ? world.first : world.second;

	if (!comment.empty()) {
		archiveNameBase += L" [" + SanitizeFileName(comment) + L"]";
	}


	// ���ɴ�ʱ������ļ���
	time_t now = time(0);
	tm ltm;
	localtime_s(&ltm, &now);
	wchar_t timeBuf[160];
	wcsftime(timeBuf, sizeof(timeBuf), L"%Y-%m-%d_%H-%M-%S", &ltm);
	archivePath = destinationFolder + L"\\" + L"[" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;

	// ��������Ŀ���ļ��У���������ڣ�
	try {
		filesystem::create_directories(destinationFolder);
		filesystem::create_directories(metadataFolder); // ȷ��Ԫ�����ļ��д���
		console.AddLog(L("LOG_BACKUP_DIR_IS"), wstring_to_utf8(destinationFolder).c_str());
	}
	catch (const filesystem::filesystem_error& e) {
		console.AddLog(L("LOG_ERROR_CREATE_BACKUP_DIR"), e.what());
		return;
	}

	// ��������ȱ���
	if (config.hotBackup) {
		BroadcastEvent("event=pre_hot_backup;");
		wstring snapshotPath = CreateWorldSnapshot(sourcePath, config.snapshotPath, console);
		if (!snapshotPath.empty()) {
			sourcePath = snapshotPath; // ������ճɹ�����������в��������ڿ���·��
			this_thread::sleep_for(chrono::milliseconds(200));//�ڴ������պ���������ʱ�����ļ�ϵͳ��Ӧʱ��
			//originalSourcePath = snapshotPath;
		}
		else {
			console.AddLog(L("LOG_ERROR_SNAPSHOT"));
			return;
		}
	}

	bool forceFullBackup = true;
	if (filesystem::exists(destinationFolder)) {
		for (const auto& entry : filesystem::directory_iterator(destinationFolder)) {
			if (entry.is_regular_file() && entry.path().filename().wstring().find(L"[Full]") != wstring::npos) {
				forceFullBackup = false;
				break;
			}
		}
	}
	if (forceFullBackup)
		console.AddLog(L("LOG_FORCE_FULL_BACKUP"));

	// ���Ʊ���������
	bool forceFullBackupDueToLimit = false;
	if (config.backupMode == 2 && config.maxSmartBackupsPerFull > 0 && !forceFullBackup) {
		vector<filesystem::path> worldBackups;
		try {
			for (const auto& entry : filesystem::directory_iterator(destinationFolder)) {
				if (entry.is_regular_file()) {
					worldBackups.push_back(entry.path());
				}
			}
		}
		catch (const filesystem::filesystem_error& e) {
			console.AddLog(L("LOG_ERROR_SCAN_BACKUP_DIR"), e.what());
		}

		if (!worldBackups.empty()) {
			sort(worldBackups.begin(), worldBackups.end(), [](const auto& a, const auto& b) {
				return filesystem::last_write_time(a) < filesystem::last_write_time(b);
				});

			int smartCount = 0;
			bool fullFound = false;
			for (auto it = worldBackups.rbegin(); it != worldBackups.rend(); ++it) {
				wstring filename = it->filename().wstring();
				if (filename.find(L"[Full]") != wstring::npos) {
					fullFound = true;
					break;
				}
				if (filename.find(L"[Smart]") != wstring::npos) {
					++smartCount;
				}
			}

			if (fullFound && smartCount >= config.maxSmartBackupsPerFull) {
				forceFullBackupDueToLimit = true;
				console.AddLog(L("LOG_FORCE_FULL_BACKUP_LIMIT_REACHED"), config.maxSmartBackupsPerFull);
			}
		}
	}

	// --- �µ�ͳһ�ļ������߼� ---

	vector<filesystem::path> candidate_files;
	BackupCheckResult checkResult;
	map<wstring, size_t> currentState;
	candidate_files = GetChangedFiles(sourcePath, metadataFolder, destinationFolder, checkResult, currentState);
	// ���ݼ����������־��¼
	if (checkResult == BackupCheckResult::NO_CHANGE && config.skipIfUnchanged) {
		console.AddLog(L("LOG_NO_CHANGE_FOUND"));
		return;
	}
	else if (checkResult == BackupCheckResult::FORCE_FULL_BACKUP_METADATA_INVALID) {
		console.AddLog(L("LOG_METADATA_INVALID"));
	}
	else if (checkResult == BackupCheckResult::FORCE_FULL_BACKUP_BASE_MISSING && config.backupMode == 2) {
		console.AddLog(L("LOG_BASE_BACKUP_NOT_FOUND"));
	}

	forceFullBackup = (checkResult == BackupCheckResult::FORCE_FULL_BACKUP_METADATA_INVALID ||
		checkResult == BackupCheckResult::FORCE_FULL_BACKUP_BASE_MISSING ||
		forceFullBackupDueToLimit) || forceFullBackup;

	// ���ݱ���ģʽȷ����ѡ�ļ��б�
	if (config.backupMode == 2 && !forceFullBackup) { // ���ܱ���ģʽ

		// GetChangedFiles ���ص����Ѹı���ļ��б�
		candidate_files = GetChangedFiles(sourcePath, metadataFolder, destinationFolder, checkResult, currentState);
		// ... (���� checkResult ���߼����ֲ���, �� LOG_NO_CHANGE_FOUND ��)
	}
	else { // ��ͨ���ݻ�ǿ����������
		// ��ѡ�б���Դ·���µ������ļ�
		try {
			candidate_files.clear();
			for (const auto& entry : filesystem::recursive_directory_iterator(sourcePath)) {
				if (entry.is_regular_file()) {
					candidate_files.push_back(entry.path());
				}
			}
		}
		catch (const filesystem::filesystem_error& e) {
			console.AddLog("[Error] Failed to scan source directory %s: %s", wstring_to_utf8(sourcePath).c_str(), e.what());
			if (config.hotBackup) {
				filesystem::remove_all(sourcePath);
			}
			return;
		}
	}

	// ���˺�ѡ�ļ��б�Ӧ�ú�����
	vector<filesystem::path> files_to_backup;
	for (const auto& file : candidate_files) {
		if (!is_blacklisted(file, sourcePath, originalSourcePath, config.blacklist)) {
			files_to_backup.push_back(file);
			//console.AddLog("%s", wstring_to_utf8(file.wstring()).c_str());
		}
	}

	// ������˺�û���ļ���Ҫ���ݣ�����ǰ����
	if (files_to_backup.empty()) {
		console.AddLog(L("LOG_NO_CHANGE_FOUND"));
		if (config.hotBackup) {
			filesystem::remove_all(sourcePath);
		}
		return;
	}

	// �������ļ��б�д����ʱ�ļ�����7z��ȡ
	filesystem::path tempDir = filesystem::temp_directory_path() / L"MineBackup_Filelist";
	filesystem::create_directories(tempDir);
	wstring filelist_path = (tempDir / (L"_filelist.txt")).wstring();

	wofstream ofs(filelist_path);
	if (ofs.is_open()) {
		// ʹ��UTF-8����д���ļ��б�
		ofs.imbue(locale(ofs.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
		for (const auto& file : files_to_backup) {
			// д������ڱ���Դ�����·��
			ofs << filesystem::relative(file, sourcePath).wstring() << endl;
		}
		ofs.close();
		{
			HANDLE h = CreateFileW(filelist_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (h != INVALID_HANDLE_VALUE) {
				FlushFileBuffers(h); // ǿ�ưѻ�������д��
				CloseHandle(h);
			}
		}
	}
	else {
		console.AddLog("[Error] Failed to create temporary file list for 7-Zip.");
		if (config.hotBackup) {
			filesystem::remove_all(sourcePath);
		}
		return;
	}

	wstring backupTypeStr; // ������ʷ��¼
	wstring basedOnBackupFile; // ����Ԫ���ݼ�¼���ܱ��ݻ��ڵ����������ļ�

	if (config.backupMode == 1 || forceFullBackup) // ��ͨ����
	{
		backupTypeStr = L"Full";
		archivePath = destinationFolder + L"\\" + L"[Full][" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;
		command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -m0=" + config.zipMethod + L" -mx=" + to_wstring(config.zipLevel) +
			L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" \"" + archivePath + L"\"" + L" @" + filelist_path;
		// ��������
		basedOnBackupFile = filesystem::path(archivePath).filename().wstring();
	}
	else if (config.backupMode == 2) // ���ܱ���
	{
		backupTypeStr = L"Smart";

		if (files_to_backup.empty()) {
			console.AddLog(L("LOG_NO_CHANGE_FOUND"));
			if (config.hotBackup) // �������
				filesystem::remove_all(sourcePath);
			return; // û�б仯��ֱ�ӷ���
		}

		console.AddLog(L("LOG_BACKUP_SMART_INFO"), files_to_backup.size());

		// ���ܱ�����Ҫ�ҵ��������ڵ��ļ�
		// �����ͨ���ٴζ�ȡԪ���ݻ�ã�GetChangedFiles �ڲ��Ѿ���֤��������
		nlohmann::json oldMetadata;
		ifstream f(metadataFolder + L"\\metadata.json");
		oldMetadata = nlohmann::json::parse(f);
		basedOnBackupFile = utf8_to_wstring(oldMetadata.at("lastBackupFile"));

		// 7z ֧���� @�ļ��� �ķ�ʽ����ָ��Ҫѹ�����ļ���������Ҫ���ݵ��ļ�·��д��һ���ı��ļ����ⳬ��cmd 8191�޳�
		archivePath = destinationFolder + L"\\" + L"[Smart][" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;

		command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -m0=" + config.zipMethod + L" -mx=" + to_wstring(config.zipLevel) +
			L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" \"" + archivePath + L"\"" + L" @" + filelist_path;
	}
	else if (config.backupMode == 3) // ���Ǳ��� - v1.7.8 ��ʱ�Ƴ�����ģʽ�ĺ���������
	{
		backupTypeStr = L"Overwrite";
		console.AddLog(L("LOG_OVERWRITE"));
		filesystem::path latestBackupPath;
		auto latest_time = filesystem::file_time_type{}; // Ĭ�Ϲ��������Сʱ��㣬����Ҫ::min()
		bool found = false;

		for (const auto& entry : filesystem::directory_iterator(destinationFolder)) {
			if (entry.is_regular_file() && entry.path().extension().wstring() == L"." + config.zipFormat) {
				if (entry.last_write_time() > latest_time) {
					latest_time = entry.last_write_time();
					latestBackupPath = entry.path();
					found = true;
				}
			}
		}
		if (found) {
			console.AddLog(L("LOG_FOUND_LATEST"), wstring_to_utf8(latestBackupPath.filename().wstring()).c_str());
			command = L"\"" + config.zipPath + L"\" u \"" + latestBackupPath.wstring() + L"\" \"" + sourcePath + L"\\*\" -mx=" + to_wstring(config.zipLevel);
			archivePath = latestBackupPath.wstring(); // ��¼�����µ��ļ�
		}
		else {
			console.AddLog(L("LOG_NO_BACKUP_FOUND"));
			archivePath = destinationFolder + L"\\" + L"[Full][" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;
			command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -m0=" + config.zipMethod + L" -mx=" + to_wstring(config.zipLevel)+ L" -m0=" + config.zipMethod  +
				L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" -spf \"" + archivePath + L"\"" + L" \"" + sourcePath + L"\\*\"";
			// -spf ǿ��ʹ������·����-spf2 ʹ�����·��
		}
	}
	// �ں�̨�߳���ִ������
	if (RunCommandInBackground(command, console, config.useLowPriority, sourcePath)) // ����Ŀ¼���ܶ���
	{
		console.AddLog(L("LOG_BACKUP_END_HEADER"));

		// �����ļ���С���
		try {
			if (filesystem::exists(archivePath)) {
				uintmax_t fileSize = filesystem::file_size(archivePath);
				// ��ֵ����Ϊ 10 KB
				if (fileSize < 10240) {
					console.AddLog(L("BACKUP_FILE_TOO_SMALL_WARNING"), wstring_to_utf8(filesystem::path(archivePath).filename().wstring()).c_str());
					// �㲥һ������
					BroadcastEvent("event=backup_warning;type=file_too_small;");
				}
			}
		}
		catch (const filesystem::filesystem_error& e) {
			console.AddLog("[Error] Could not check backup file size: %s", e.what());
		}

		if (g_appState.realConfigIndex != -1)
			LimitBackupFiles(config, g_appState.realConfigIndex, destinationFolder, config.keepCount, &console);
		else
			LimitBackupFiles(config, g_appState.currentConfigIndex, destinationFolder, config.keepCount, &console);

		UpdateMetadataFile(metadataFolder, filesystem::path(archivePath).filename().wstring(), basedOnBackupFile, currentState);
		// ��ʷ��¼
		if (g_appState.realConfigIndex != -1)
			AddHistoryEntry(g_appState.realConfigIndex, world.first, filesystem::path(archivePath).filename().wstring(), backupTypeStr, comment);
		else
			AddHistoryEntry(g_appState.currentConfigIndex, world.first, filesystem::path(archivePath).filename().wstring(), backupTypeStr, comment);
		g_appState.realConfigIndex = -1; // ����
		// �㲥һ���ɹ��¼�
		string payload = "event=backup_success;config=" + to_string(g_appState.currentConfigIndex) + ";world=" + wstring_to_utf8(world.first) + ";file=" + wstring_to_utf8(filesystem::path(archivePath).filename().wstring());
		BroadcastEvent(payload);

		// ��ͬ���߼�
		if (config.cloudSyncEnabled && !config.rclonePath.empty() && !config.rcloneRemotePath.empty()) {
			console.AddLog(L("CLOUD_SYNC_START"));
			wstring rclone_command = L"\"" + config.rclonePath + L"\" copy \"" + archivePath + L"\" \"" + config.rcloneRemotePath + L"/" + world.first + L"\" --progress";
			// ����һ���߳���ִ����ͬ��������������������
			thread([rclone_command, &console, config]() {
				RunCommandInBackground(rclone_command, console, config.useLowPriority);
				console.AddLog(L("CLOUD_SYNC_FINISH"));
				}).detach();
		}
	}
	else {
		BroadcastEvent("event=backup_failed;config=" + to_string(g_appState.currentConfigIndex) + ";world=" + wstring_to_utf8(world.first) + ";error=command_failed");
	}


	filesystem::remove(filesystem::temp_directory_path() / L"MineBackup_Snapshot" / L"7z.txt");
	if (config.hotBackup && sourcePath != (config.saveRoot + L"\\" + world.first)) {
		console.AddLog(L("LOG_CLEAN_SNAPSHOT"));
		error_code ec;
		filesystem::remove_all(sourcePath, ec);
		if (ec) console.AddLog(L("LOG_WARNING_CLEAN_SNAPSHOT"), ec.message().c_str());
	}
}
void DoOthersBackup(const Config config, filesystem::path backupWhat, const wstring& comment) {
	console.AddLog(L("LOG_BACKUP_OTHERS_START"));

	filesystem::path saveRoot(config.saveRoot);

	filesystem::path othersPath = backupWhat;
	backupWhat = backupWhat.filename().wstring(); // ֻ���������ļ�����

	//filesystem::path modsPath = saveRoot.parent_path() / "mods";

	if (!filesystem::exists(othersPath) || !filesystem::is_directory(othersPath)) {
		console.AddLog(L("LOG_ERROR_OTHERS_NOT_FOUND"), wstring_to_utf8(othersPath.wstring()).c_str());
		console.AddLog(L("LOG_BACKUP_OTHERS_END"));
		return;
	}

	filesystem::path destinationFolder;
	wstring archiveNameBase;

	destinationFolder = filesystem::path(config.backupPath) / backupWhat;
	archiveNameBase = backupWhat;

	if (!comment.empty()) {
		archiveNameBase += L" [" + SanitizeFileName(comment) + L"]";
	}

	// Timestamp
	time_t now = time(0);
	tm ltm;
	localtime_s(&ltm, &now);
	wchar_t timeBuf[160];
	wcsftime(timeBuf, sizeof(timeBuf), L"%Y-%m-%d_%H-%M-%S", &ltm);
	wstring archivePath = destinationFolder.wstring() + L"\\" + L"[" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;

	try {
		filesystem::create_directories(destinationFolder);
		console.AddLog(L("LOG_BACKUP_DIR_IS"), wstring_to_utf8(destinationFolder.wstring()).c_str());
	}
	catch (const filesystem::filesystem_error& e) {
		console.AddLog(L("LOG_ERROR_CREATE_BACKUP_DIR"), e.what());
		console.AddLog(L("LOG_BACKUP_OTHERS_END"));
		return;
	}

	wstring command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -m0=" + config.zipMethod + L" -mx=" + to_wstring(config.zipLevel) +
		L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" \"" + archivePath + L"\"" + L" \"" + othersPath.wstring() + L"\\*\"";

	if (RunCommandInBackground(command, console, config.useLowPriority)) {
		LimitBackupFiles(config, g_appState.realConfigIndex, destinationFolder.wstring(), config.keepCount, &console);
		// ������������ӵ���ʷ
		AddHistoryEntry(g_appState.currentConfigIndex, backupWhat, filesystem::path(archivePath).filename().wstring(), backupWhat, comment);
	}

	console.AddLog(L("LOG_BACKUP_OTHERS_END"));
}
void DoRestore2(const Config config, const wstring& worldName, const filesystem::path& fullBackupPath, Console& console, int restoreMethod) {
	console.AddLog(L("LOG_RESTORE_START_HEADER"));
	console.AddLog(L("LOG_RESTORE_PREPARE"), wstring_to_utf8(worldName).c_str());
	console.AddLog(L("LOG_RESTORE_USING_FILE"), wstring_to_utf8(fullBackupPath.wstring()).c_str());

	if (!filesystem::exists(config.zipPath)) {
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND"), wstring_to_utf8(config.zipPath).c_str());
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND_HINT"));
		return;
	}

	wstring destinationFolder = config.saveRoot + L"\\" + worldName;

	if (restoreMethod == 0) { // Clean Restore
		console.AddLog(L("LOG_DELETING_EXISTING_WORLD"), wstring_to_utf8(destinationFolder).c_str());
		try {
			if (filesystem::exists(destinationFolder)) {
				filesystem::remove_all(destinationFolder);
			}
		}
		catch (const filesystem::filesystem_error& e) {
			console.AddLog("[ERROR] Failed to delete existing world folder: %s. Continuing with overwrite.", e.what());
		}
	}

	// For a manually selected file, we treat it as a single restore operation.
	// Smart Restore logic does not apply as we don't know the history.
	wstring command = L"\"" + config.zipPath + L"\" x \"" + fullBackupPath.wstring() + L"\" -o\"" + destinationFolder + L"\" -y";
	RunCommandInBackground(command, console, config.useLowPriority);

	console.AddLog(L("LOG_RESTORE_END_HEADER"));
}
// restoreMethod: 0=Clean Restore, 1=Overwrite Restore, 2=�����µ�ѡ�����򸲸ǻ�ԭ
void DoRestore(const Config config, const wstring& worldName, const wstring& backupFile, Console& console, int restoreMethod, const string& customRestoreList) {
	console.AddLog(L("LOG_RESTORE_START_HEADER"));
	console.AddLog(L("LOG_RESTORE_PREPARE"), wstring_to_utf8(worldName).c_str());
	console.AddLog(L("LOG_RESTORE_USING_FILE"), wstring_to_utf8(backupFile).c_str());

	// ���7z.exe�Ƿ����
	if (!filesystem::exists(config.zipPath)) {
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND"), wstring_to_utf8(config.zipPath).c_str());
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND_HINT"));
		return;
	}

	// ׼��·��
	wstring sourceDir = config.backupPath + L"\\" + worldName;
	wstring destinationFolder = config.saveRoot + L"\\" + worldName;
	filesystem::path targetBackupPath = filesystem::path(sourceDir) / backupFile;

	// ��鱸���ļ��Ƿ����
	if ((backupFile.find(L"[Smart]") == wstring::npos && backupFile.find(L"[Full]") == wstring::npos) || !filesystem::exists(sourceDir + L"\\" + backupFile)) {
		console.AddLog(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(backupFile).c_str());
		return;
	}

	// ��ԭǰ��������Ƿ���������
	if (IsFileLocked(destinationFolder + L"\\session.lock")) {
		int msgboxID = MessageBoxW(
			NULL,
			utf8_to_wstring(L("RESTORE_OVER_RUNNING_WORLD_MSG")).c_str(),
			utf8_to_wstring(L("RESTORE_OVER_RUNNING_WORLD_TITLE")).c_str(),
			MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2
		);
		if (msgboxID == IDNO) {
			console.AddLog("[Info] Restore cancelled by user due to active game session.");
			return;
		}
	}

	if (restoreMethod == 0) {
		console.AddLog(L("LOG_DELETING_EXISTING_WORLD"), wstring_to_utf8(destinationFolder).c_str());
		bool deletion_ok = true;
		if (filesystem::exists(destinationFolder)) {
			try {
				for (const auto& entry : filesystem::directory_iterator(destinationFolder)) {
					// ʹ�� is_blacklisted �����ж��Ƿ��ڰ�������
					if (is_blacklisted(entry.path(), destinationFolder, destinationFolder, restoreWhitelist)) {
						console.AddLog(L("LOG_SKIPPING_WHITELISTED_ITEM"), wstring_to_utf8(entry.path().filename().wstring()).c_str());
						continue;
					}

					console.AddLog(L("LOG_DELETING_EXISTING_WORLD_ITEM"), wstring_to_utf8(entry.path().filename().wstring()).c_str());
					error_code ec;
					if (entry.is_directory()) {
						filesystem::remove_all(entry.path(), ec);
					}
					else {
						filesystem::remove(entry.path(), ec);
					}
					if (ec) {
						console.AddLog(L("LOG_DELETION_ERROR"), wstring_to_utf8(entry.path().filename().wstring()).c_str(), ec.message().c_str());
						deletion_ok = false; // ���ɾ��ʧ��
					}
				}
			}
			catch (const filesystem::filesystem_error& e) {
				console.AddLog("[Error] An exception occurred during pre-restore cleanup: %s.", e.what());
				deletion_ok = false;
			}
		}
		if (!deletion_ok) {
			console.AddLog(L("ERROR_CLEAN_RESTORE_FAILED"));
			return; // ��ֹ��ԭ�Ա�������
		}
	}

	// �ռ�������صı����ļ�
	vector<filesystem::path> backupsToApply;

	// ���Ŀ�����������ݣ�ֱ�ӻ�ԭ��
	if (backupFile.find(L"[Smart]") != wstring::npos) { // Ŀ������������
		// Ѱ�һ�������������
		filesystem::path baseFullBackup;
		auto baseFullTime = filesystem::file_time_type{};

		// ���������ԭ�����ҵ��������ڵ���������
		if (restoreMethod == 1 || restoreMethod == 0) {
			for (const auto& entry : filesystem::directory_iterator(sourceDir)) {
				if (entry.is_regular_file() && entry.path().filename().wstring().find(L"[Full]") != wstring::npos) {
					if (entry.last_write_time() < filesystem::last_write_time(targetBackupPath) && entry.last_write_time() > baseFullTime) {
						baseFullTime = entry.last_write_time();
						baseFullBackup = entry.path();
					}
				}
			}

			if (baseFullBackup.empty()) {
				console.AddLog(L("LOG_BACKUP_SMART_NO_FOUND"));
				return;
			}

			console.AddLog(L("LOG_BACKUP_SMART_FOUND"), wstring_to_utf8(baseFullBackup.filename().wstring()).c_str());
			backupsToApply.push_back(baseFullBackup);
			// �ռ��ӻ������ݵ�Ŀ�걸��֮���������������
			for (const auto& entry : filesystem::directory_iterator(sourceDir)) {
				if (entry.is_regular_file() && entry.path().filename().wstring().find(L"[Smart]") != wstring::npos) {
					if (entry.last_write_time() > baseFullTime && entry.last_write_time() <= filesystem::last_write_time(targetBackupPath)) {
						backupsToApply.push_back(entry.path());
					}
				}
			}
		}
		else if (restoreMethod == 2) {
			// ����ԭ���������Smart���ݿ�ʼ��һֱ��Ŀ�걸��
			for (const auto& entry : filesystem::directory_iterator(sourceDir)) {
				if (entry.is_regular_file()) { // ����Ҫ����Smart��Full��ȫ����ԭ��ȥ
					if (entry.last_write_time() > filesystem::last_write_time(targetBackupPath)) {
						backupsToApply.push_back(entry.path());
					}
				}
			}
		}
	}
	else { //�����������ݴ���
		backupsToApply.push_back(targetBackupPath);
	}

	// ��ʽ: "C:\7z.exe" x "Դѹ����·��" -o"Ŀ���ļ���·��" -y
	// 'x' ��ʾ��·����ѹ, '-o' ָ�����Ŀ¼, '-y' ��ʾ��������ʾ�ش��ǡ������縲���ļ���

	if (restoreMethod == 2)
	{
		// ��ʱ����������������ҪӦ�õı���
		sort(backupsToApply.begin(), backupsToApply.end(), [](const auto& a, const auto& b) {
			return filesystem::last_write_time(a) > filesystem::last_write_time(b);
			});
	}
	else {
		// ��ʱ��˳������������ҪӦ�õı���
		sort(backupsToApply.begin(), backupsToApply.end(), [](const auto& a, const auto& b) {
			return filesystem::last_write_time(a) < filesystem::last_write_time(b);
			});
	}

	wstring filesToExtractStr;
	// �����Զ��廹ԭģʽ�¹����ļ��б�
	if (restoreMethod == 3 && !customRestoreList.empty()) {
		console.AddLog(L("LOG_CUSTOM_RESTORE_START"));
		stringstream ss(customRestoreList);
		string item;
		while (getline(ss, item, ',')) {
			item.erase(0, item.find_first_not_of(" \t\n\r"));
			item.erase(item.find_last_not_of(" \t\n\r") + 1);
			if (!item.empty()) {
				filesToExtractStr += L" \"" + utf8_to_wstring(item) + L"\"";
			}
		}
	}


	// ����ִ�л�ԭ
	for (size_t i = 0; i < backupsToApply.size(); ++i) {
		const auto& backup = backupsToApply[i];
		console.AddLog(L("RESTORE_STEPS"), i + 1, backupsToApply.size(), wstring_to_utf8(backup.filename().wstring()).c_str());
		wstring command = L"\"" + config.zipPath + L"\" x \"" + backup.wstring() + L"\" -o\"" + destinationFolder + L"\" -y" + filesToExtractStr;
		RunCommandInBackground(command, console, config.useLowPriority);
	}
	console.AddLog(L("LOG_RESTORE_END_HEADER"));
	BroadcastEvent("event=restore_success;config=" + to_string(g_appState.currentConfigIndex) + ";world=" + wstring_to_utf8(worldName) + ";backup=" + wstring_to_utf8(backupFile));
	return;
}

void DoDeleteBackup(const Config& config, const HistoryEntry& entryToDelete, int &configIndex, Console& console) {
	console.AddLog(L("LOG_PRE_TO_DELETE"), wstring_to_utf8(entryToDelete.backupFile).c_str());

	filesystem::path backupDir = config.backupPath + L"\\" + entryToDelete.worldName;
	vector<filesystem::path> filesToDelete;
	filesToDelete.push_back(backupDir / entryToDelete.backupFile);

	// ִ��ɾ������
	for (const auto& path : filesToDelete) {
		try {
			if (filesystem::exists(path)) {
				filesystem::remove(path);
				console.AddLog("  - %s OK", wstring_to_utf8(path.filename().wstring()).c_str());
				// ����ʷ��¼���Ƴ���Ӧ��Ŀ
				RemoveHistoryEntry(configIndex, path.filename().wstring());
			}
			else {
				console.AddLog(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(entryToDelete.backupFile).c_str());
				RemoveHistoryEntry(configIndex, path.filename().wstring());
			}
		}
		catch (const filesystem::filesystem_error& e) {
			console.AddLog(L("LOG_ERROR_DELETE_BACKUP"), wstring_to_utf8(path.filename().wstring()).c_str(), e.what());
		}
	}
	SaveHistory(); // ������ʷ��¼�ĸ���
}

void DoSafeDeleteBackup(const Config& config, const HistoryEntry& entryToDelete, int configIndex, Console& console) {
	console.AddLog(L("LOG_SAFE_DELETE_START"), wstring_to_utf8(entryToDelete.backupFile).c_str());

	if (entryToDelete.isImportant) {
		console.AddLog(L("LOG_SAFE_DELETE_ABORT_IMPORTANT"), wstring_to_utf8(entryToDelete.backupFile).c_str());
		return;
	}

	filesystem::path backupDir = config.backupPath + L"\\" + entryToDelete.worldName;
	filesystem::path pathToDelete = backupDir / entryToDelete.backupFile;
	const HistoryEntry* nextEntryRaw = nullptr;

	// Create a sorted list of history entries for this world to reliably find the next one
	vector<const HistoryEntry*> worldHistory;
	for (const auto& entry : g_appState.g_history[configIndex]) {
		if (entry.worldName == entryToDelete.worldName) {
			worldHistory.push_back(&entry);
		}
	}
	sort(worldHistory.begin(), worldHistory.end(), [](const auto* a, const auto* b) {
		return a->timestamp_str < b->timestamp_str;
		});

	for (size_t i = 0; i < worldHistory.size(); ++i) {
		if (worldHistory[i]->backupFile == entryToDelete.backupFile) {
			if (i + 1 < worldHistory.size()) {
				nextEntryRaw = worldHistory[i + 1];
			}
			break;
		}
	}

	if (!nextEntryRaw || nextEntryRaw->backupType == L"Full") {
		console.AddLog(L("LOG_SAFE_DELETE_END_OF_CHAIN"));
		DoDeleteBackup(config, entryToDelete, configIndex, console);
		return;
	}

	if (nextEntryRaw->isImportant) {
		console.AddLog(L("LOG_SAFE_DELETE_ABORT_IMPORTANT_TARGET"), wstring_to_utf8(nextEntryRaw->backupFile).c_str());
		return;
	}

	const HistoryEntry nextEntry = *nextEntryRaw;
	filesystem::path pathToMergeInto = backupDir / nextEntry.backupFile;
	console.AddLog(L("LOG_SAFE_DELETE_MERGE_INFO"), wstring_to_utf8(entryToDelete.backupFile).c_str(), wstring_to_utf8(nextEntry.backupFile).c_str());

	filesystem::path tempExtractDir = filesystem::temp_directory_path() / L"MineBackup_Merge";

	try {
		filesystem::remove_all(tempExtractDir);
		filesystem::create_directories(tempExtractDir);

		console.AddLog(L("LOG_SAFE_DELETE_STEP_1"));
		wstring cmdExtract = L"\"" + config.zipPath + L"\" x \"" + pathToDelete.wstring() + L"\" -o\"" + tempExtractDir.wstring() + L"\" -y";
		if (!RunCommandInBackground(cmdExtract, console, config.useLowPriority)) {
			throw runtime_error("Failed to extract source archive.");
		}

		console.AddLog(L("LOG_SAFE_DELETE_STEP_2"));
		auto original_mod_time = filesystem::last_write_time(pathToMergeInto);

		wstring cmdMerge = L"\"" + config.zipPath + L"\" a \"" + pathToMergeInto.wstring() + L"\" .\\*";
		if (!RunCommandInBackground(cmdMerge, console, config.useLowPriority, tempExtractDir.wstring())) {
			filesystem::last_write_time(pathToMergeInto, original_mod_time);
			throw runtime_error("Failed to merge files into the target archive.");
		}
		filesystem::last_write_time(pathToMergeInto, original_mod_time);

		filesystem::path finalArchivePath = pathToMergeInto;
		wstring finalBackupType = nextEntry.backupType;
		wstring finalBackupFile = nextEntry.backupFile;

		if (entryToDelete.backupType == L"Full") {
			console.AddLog(L("LOG_SAFE_DELETE_STEP_3"));
			finalBackupType = L"Full";
			wstring newFilename = nextEntry.backupFile;
			size_t pos = newFilename.find(L"[Smart]");
			if (pos != wstring::npos) {
				newFilename.replace(pos, 7, L"[Full]");
				finalBackupFile = newFilename;
				filesystem::path newPath = backupDir / newFilename;
				filesystem::rename(pathToMergeInto, newPath);
				finalArchivePath = newPath;
				console.AddLog(L("LOG_SAFE_DELETE_RENAMED"), wstring_to_utf8(newFilename).c_str());
			}
		}
		else {
			console.AddLog(L("LOG_SAFE_DELETE_STEP_3_SKIP"));
		}

		console.AddLog(L("LOG_SAFE_DELETE_STEP_4"));
		filesystem::remove(pathToDelete);
		RemoveHistoryEntry(configIndex, entryToDelete.backupFile);

		for (auto& entry : g_appState.g_history[configIndex]) {
			if (entry.worldName == nextEntry.worldName && entry.backupFile == nextEntry.backupFile) {
				entry.backupFile = finalBackupFile;
				entry.backupType = finalBackupType;
				break;
			}
		}

		filesystem::remove_all(tempExtractDir);
		console.AddLog(L("LOG_SAFE_DELETE_SUCCESS"));

	}
	catch (const exception& e) {
		console.AddLog(L("LOG_SAFE_DELETE_FATAL_ERROR"), e.what());
		filesystem::remove_all(tempExtractDir);
	}
}

// ������� worldIdx ��Ϊ key ���µĳ�ͻ��ʹ��{ configIdx, worldIdx }
void AutoBackupThreadFunction(int configIdx, int worldIdx, int intervalMinutes, Console* console, atomic<bool>& stop_flag) {
	auto key = make_pair(configIdx, worldIdx);
	console->AddLog(L("LOG_AUTOBACKUP_START"), worldIdx, intervalMinutes);

	while (true) {
		// �ȴ�ָ����ʱ�䣬��ÿ����һ���Ƿ���Ҫֹͣ
		for (int i = 0; i < intervalMinutes * 60; ++i) {
			// ���޸���ֱ�Ӽ�鴫���ԭ�����ã����������
			if (stop_flag) { // ���� stop_flag.load()
				console->AddLog(L("LOG_AUTOBACKUP_STOPPED"), worldIdx);
				return; // �̰߳�ȫ���˳�
			}
			this_thread::sleep_for(chrono::seconds(1));
		}

		// ����ڳ�ʱ��ĵȴ��󣬷�����Ҫֹͣ����ִ�б���ֱ���˳�
		if (stop_flag) {
			console->AddLog(L("LOG_AUTOBACKUP_STOPPED"), worldIdx);
			return;
		}

		// ʱ�䵽�ˣ���ʼ����
		console->AddLog(L("LOG_AUTOBACKUP_ROUTINE"), worldIdx);
		{
			lock_guard<mutex> lock(g_appState.configsMutex);
			if (g_appState.configs.count(configIdx) && worldIdx >= 0 && worldIdx < g_appState.configs[configIdx].worlds.size()) {
				DoBackup(g_appState.configs[configIdx], g_appState.configs[configIdx].worlds[worldIdx], *console);
			}
			else {
				console->AddLog(L("ERROR_INVALID_WORLD_IN_TASK"), configIdx, worldIdx);
				// ������Ч���˳����Ƴ�
				lock_guard<mutex> lock2(g_appState.task_mutex);
				if (g_appState.g_active_auto_backups.count(key)) {
					g_appState.g_active_auto_backups.erase(key);
				}
				return;
			}
		}
	}
}
