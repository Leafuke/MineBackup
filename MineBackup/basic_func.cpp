#include "json.hpp"
#include <string>
#include <map>
#include <filesystem>
#include <fstream>
#include <Windows.h>
using namespace std;

// ע����ѯ
string GetRegistryValue(const string& keyPath, const string& valueName)
{
	HKEY hKey;
	string valueData;
	if (RegOpenKeyExA(HKEY_CURRENT_USER, keyPath.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		char buffer[1024];
		DWORD dataSize = sizeof(buffer);
		if (RegGetValueA(hKey, NULL, valueName.c_str(), RRF_RT_ANY, NULL, buffer, &dataSize) == ERROR_SUCCESS) {
			valueData = buffer;
		}
		RegCloseKey(hKey);
	}
	else
		return "";
	return valueData;
}

wstring GetLastOpenTime(const wstring& worldPath) {
	try {
		auto ftime = filesystem::last_write_time(worldPath);
		// ת��Ϊ system_clock::time_point
		auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
			ftime - filesystem::file_time_type::clock::now()
			+ chrono::system_clock::now()
		);
		time_t cftime = chrono::system_clock::to_time_t(sctp);
		wchar_t buf[64];
		struct tm timeinfo;
		//wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", localtime(&cftime));//localtimeҪ���ɸ���ȫ��localtime
		if (localtime_s(&timeinfo, &cftime) == 0) {
			wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &timeinfo);
			return buf;
		}
		else {
			return L"N/A";
		}
	}
	catch (...) {
		return L"N/A";
	}
}

wstring GetLastBackupTime(const wstring& backupDir) {
	time_t latest = 0;
	try {
		if (filesystem::exists(backupDir)) {
			for (const auto& entry : filesystem::directory_iterator(backupDir)) {
				if (entry.is_regular_file()) {
					auto ftime = entry.last_write_time();
					// ת��Ϊ system_clock::time_point
					auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
						ftime - filesystem::file_time_type::clock::now()
						+ chrono::system_clock::now()
					);
					time_t cftime = chrono::system_clock::to_time_t(sctp);
					if (cftime > latest) latest = cftime;
				}
			}
		}
		if (latest == 0) return L"/";
		wchar_t buf[64];
		struct tm timeinfo;
		//wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", localtime(&cftime));//localtimeҪ���ɸ���ȫ��localtime
		if (localtime_s(&timeinfo, &latest) == 0) {
			wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &timeinfo);
			return buf;
		}
		else {
			return L"N/A";
		}
	}
	catch (...) {
		return L"N/A";
	}
}

wstring utf8_to_wstring(const string& str);
string wstring_to_utf8(const wstring& str);

// �����ļ��Ĺ�ϣֵ������һ���򵥵�ʵ�֣��ܲ��ϸ��գ�
size_t CalculateFileHash(const filesystem::path& filepath) {
	ifstream file(filepath, ios::binary);
	if (!file) return 0;

	string content((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
	hash<string> hasher;
	return hasher(content);
}
// ��Ϊȫ�ֱ�������������޸�
map<wstring, size_t> currentState;

// ����������ö������
enum class BackupCheckResult {
	NO_CHANGE,
	CHANGES_DETECTED,
	FORCE_FULL_BACKUP_METADATA_INVALID,
	FORCE_FULL_BACKUP_BASE_MISSING
};

// ȫ�ֱ��� currentState ������Ҫ�����������������ڲ�

// λ�� basic_func.cpp
vector<filesystem::path> GetChangedFiles(
	const filesystem::path& worldPath,
	const filesystem::path& metadataPath,
	const filesystem::path& backupPath, // ��Ҫ���뱸��·���Թ���֤
	BackupCheckResult& out_result,
	map<wstring, size_t>& out_currentState // ����ǰ״̬����������������
) {
	out_result = BackupCheckResult::NO_CHANGE;
	out_currentState.clear();
	vector<filesystem::path> changedFiles;
	map<wstring, size_t> lastState;
	filesystem::path metadataFile = metadataPath / L"metadata.json";

	// 1. ��ȡ����֤Ԫ����
	if (!filesystem::exists(metadataFile)) {
		out_result = BackupCheckResult::FORCE_FULL_BACKUP_METADATA_INVALID;
		// Ԫ���ݲ����ڣ�ɨ�������ļ������أ��Ա�����״���������
		for (const auto& entry : filesystem::recursive_directory_iterator(worldPath)) {
			if (entry.is_regular_file()) {
				out_currentState[filesystem::relative(entry.path(), worldPath).wstring()] = CalculateFileHash(entry.path());
			}
		}
		return {}; // ���ؿ��б���Ϊ�����ļ�״̬����¼�� out_currentState ����
	}

	nlohmann::json metadata;
	wstring basedOnBackupFile;
	try {
		ifstream f(metadataFile);
		metadata = nlohmann::json::parse(f);
		basedOnBackupFile = utf8_to_wstring(metadata.at("basedOnBackupFile"));
		for (auto& [key, val] : metadata.at("fileStates").items()) {
			lastState[utf8_to_wstring(key)] = val.get<size_t>();
		}
	}
	catch (const nlohmann::json::exception& e) {
		// Ԫ�����ļ��𻵻��ʽ����
		out_result = BackupCheckResult::FORCE_FULL_BACKUP_METADATA_INVALID;
		// ͬ����Ҫɨ�������ļ�
		for (const auto& entry : filesystem::recursive_directory_iterator(worldPath)) {
			if (entry.is_regular_file()) {
				out_currentState[filesystem::relative(entry.path(), worldPath).wstring()] = CalculateFileHash(entry.path());
			}
		}
		return {};
	}

	// 2. ������֤�����Ԫ���������Ļ�׼�����ļ��Ƿ����
	if (!filesystem::exists(backupPath / basedOnBackupFile)) {
		out_result = BackupCheckResult::FORCE_FULL_BACKUP_BASE_MISSING;
		// ��׼�ļ����û�ɾ����Ԫ����ʧЧ��ɨ�������ļ��Խ����µ���������
		for (const auto& entry : filesystem::recursive_directory_iterator(worldPath)) {
			if (entry.is_regular_file()) {
				out_currentState[filesystem::relative(entry.path(), worldPath).wstring()] = CalculateFileHash(entry.path());
			}
		}
		return {};
	}

	// 3. ���㵱ǰ״̬�����ϴ�״̬�Ƚ�
	if (filesystem::exists(worldPath)) {
		for (const auto& entry : filesystem::recursive_directory_iterator(worldPath)) {
			if (entry.is_regular_file()) {
				filesystem::path relativePath = filesystem::relative(entry.path(), worldPath);
				size_t currentHash = CalculateFileHash(entry.path());
				out_currentState[relativePath.wstring()] = currentHash;
				// ����ļ����µģ����߹�ϣֵ��ͬ�����ж�Ϊ�Ѹ���
				if (lastState.find(relativePath.wstring()) == lastState.end() || lastState[relativePath.wstring()] != currentHash) {
					changedFiles.push_back(entry.path());
				}
			}
		}
	}

	if (!changedFiles.empty()) {
		out_result = BackupCheckResult::CHANGES_DETECTED;
	}

	return changedFiles;
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


// ��ȡ�Ѹ��ĵ��ļ��б�������״̬�ļ�
//wstring utf8_to_wstring(const string& str);
//vector<filesystem::path> GetChangedFiles(const filesystem::path& worldPath, const filesystem::path& metadataPath) {
//	vector<filesystem::path> changedFiles;
//	map<wstring, size_t> lastState;
//	filesystem::path stateFilePath = metadataPath / L"backup_state.txt";
//	// 1. ��ȡ��һ�ε�״̬
//	ifstream stateFileIn(stateFilePath);
//	if (stateFileIn.is_open()) {
//		string path; // txt��ǧ�����пո�
//		size_t hash;
//		while (stateFileIn >> path >> hash) {
//			lastState[utf8_to_wstring(path)] = hash;
//		}
//		stateFileIn.close();
//	}
//
//	// 2. ���㵱ǰ״̬�����ϴ�״̬�Ƚ�
//	if (filesystem::exists(worldPath)) {
//		for (const auto& entry : filesystem::recursive_directory_iterator(worldPath)) {
//			if (entry.is_regular_file()) {
//				filesystem::path relativePath = filesystem::relative(entry.path(), worldPath);
//				size_t currentHash = CalculateFileHash(entry.path());
//				currentState[relativePath.wstring()] = currentHash;
//
//				// ����ļ����µģ����߹�ϣֵ��ͬ�����ж�Ϊ�Ѹ���
//				if (lastState.find(relativePath.wstring()) == lastState.end() || lastState[relativePath.wstring()] != currentHash) {
//					changedFiles.push_back(entry.path());
//				}
//			}
//		}
//	}
//	return changedFiles;
//}
// �µĺ�����ר�����ڱ���״̬�ļ� ��filesystem�汾�޸�Ϊwofstream��ͼ�����������
//void SaveStateFile(const filesystem::path& metadataPath) {
//	filesystem::path stateFilePath = metadataPath / L"backup_state.txt";
//	wofstream stateFileOut(stateFilePath, ios::trunc);
//	stateFileOut.imbue(locale(stateFileOut.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
//	for (const auto& pair : currentState) {
//		stateFileOut << pair.first << L" " << pair.second << endl;
//	}
//	stateFileOut.close();
//}

bool checkWorldName(const wstring& world, const vector<pair<wstring, wstring>>& worldList) {
	for (const pair<wstring, wstring>& worldLi : worldList) {
		if (world == worldLi.first)
			return false;
	}
	return true;
}

// ��������������������
void SetAutoStart(const string& appName, const wstring& appPath, int configId, bool enable) {
	HKEY hKey;
	const wstring keyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

	// LSTATUS��Windows API�б�׼��������
	LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, KEY_WRITE, &hKey);

	if (status == ERROR_SUCCESS) {
		if (enable) {
			wstring command = L"\"" + appPath + L"\" -specialcfg " + to_wstring(configId);
			// RegSetValueExW ��Ҫ6������: HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD
			RegSetValueExW(
				hKey,
				utf8_to_wstring(appName).c_str(),
				0,
				REG_SZ,
				(const BYTE*)command.c_str(),
				(DWORD)((command.length() + 1) * sizeof(wchar_t))
			);
		}
		else {
			// RegDeleteValueW ��Ҫ2������: HKEY, LPCWSTR
			RegDeleteValueW(hKey, utf8_to_wstring(appName).c_str());
		}
		RegCloseKey(hKey);
	}
}

wstring SanitizeFileName(const wstring& input) {
	wstring output = input;
	const wstring invalid_chars = L"\\/:*?\"<>|";
	for (wchar_t& c : output) {
		if (invalid_chars.find(c) != wstring::npos) {
			c = L'_'; // ���Ƿ��ַ��滻Ϊ�»���
		}
	}
	return output;
}

// һ���򵥵ġ����������JSONֵ������
string find_json_value(const string& json, const string& key) {
	string search_key = "\"" + key + "\":\"";
	size_t start_pos = json.find(search_key);
	if (start_pos == string::npos) {
		// ���Բ��ҷ��ַ���ֵ (�� "key": true)
		search_key = "\"" + key + "\":";
		start_pos = json.find(search_key);
		if (start_pos == string::npos) return "";

		start_pos += search_key.length();
		while (start_pos < json.length() && isspace(json[start_pos])) start_pos++; // �����ո�

		size_t end_pos = start_pos;
		while (end_pos < json.length() && json[end_pos] != ',' && json[end_pos] != '}') end_pos++;

		string val = json.substr(start_pos, end_pos - start_pos);
		// ȥ��ĩβ�Ŀո�
		val.erase(val.find_last_not_of(" \n\r\t") + 1);
		return val;
	}

	start_pos += search_key.length();
	size_t end_pos = json.find("\"", start_pos);
	if (end_pos == string::npos) return "";

	return json.substr(start_pos, end_pos - start_pos);
}