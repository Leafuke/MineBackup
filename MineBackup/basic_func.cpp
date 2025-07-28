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
// ��ȡ�Ѹ��ĵ��ļ��б�������״̬�ļ�
//string utf8_to_gbk(const string& utf8);
vector<filesystem::path> GetChangedFiles(const filesystem::path& worldPath, const filesystem::path& metadataPath) {
	vector<filesystem::path> changedFiles;
	map<wstring, size_t> lastState;
	filesystem::path stateFilePath = metadataPath / L"backup_state.txt";
	// 1. ��ȡ��һ�ε�״̬
	wifstream stateFileIn(stateFilePath);
	//stateFileIn.imbue(locale(stateFileIn.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
	if (stateFileIn.is_open()) {
		wstring path;
		size_t hash;
		while (stateFileIn >> path >> hash) {
			lastState[path] = hash;
		}
		stateFileIn.close();
	}

	/*wofstream out("D:/awatest.txt", ios::binary);
	out.imbue(locale(out.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));*/
	// 2. ���㵱ǰ״̬�����ϴ�״̬�Ƚ�
	if (filesystem::exists(worldPath)) {
		for (const auto& entry : filesystem::recursive_directory_iterator(worldPath)) {
			if (entry.is_regular_file()) {
				filesystem::path relativePath = filesystem::relative(entry.path(), worldPath);
				size_t currentHash = CalculateFileHash(entry.path());
				//out << entry.path() << endl;
				currentState[relativePath.wstring()] = currentHash;

				// ����ļ����µģ����߹�ϣֵ��ͬ�����ж�Ϊ�Ѹ���
				if (lastState.find(relativePath.wstring()) == lastState.end() || lastState[relativePath.wstring()] != currentHash) {
					changedFiles.push_back(entry.path());
				}
			}
		}
	}
	//out.close();
	return changedFiles;
}
// �µĺ�����ר�����ڱ���״̬�ļ� ��filesystem�汾�޸�Ϊwofstream��ͼ�����������
void SaveStateFile(const filesystem::path& metadataPath) {
	filesystem::path stateFilePath = metadataPath / L"backup_state.txt";
	wofstream stateFileOut(stateFilePath, ios::trunc);
	for (const auto& pair : currentState) {
		stateFileOut << pair.first << L" " << pair.second << endl;
	}
	stateFileOut.close();
}
//void SaveStateFile(const filesystem::path& metadataPath) {
//	filesystem::path stateFilePath = metadataPath / L"backup_state.txt";
//	wofstream out(stateFilePath, ios::binary);
//	if (!out.is_open()) {
//		return;
//	}
//	out.imbue(locale(out.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
//	for (const auto& pair : currentState) {
//		out << pair.first << L" " << pair.second << endl;
//	}
//	out.close();
//}

bool checkWorldName(const wstring& world, const vector<pair<wstring, wstring>>& worldList) {
	for (const pair<wstring, wstring>& worldLi : worldList) {
		if (world == worldLi.first)
			return false;
	}
	return true;
}

wstring utf8_to_wstring(const string& str);
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