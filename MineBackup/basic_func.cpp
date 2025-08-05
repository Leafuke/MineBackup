#include <string>
#include <map>
#include <filesystem>
#include <fstream>
#include <Windows.h>
using namespace std;

// 注册表查询
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
		// 转换为 system_clock::time_point
		auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
			ftime - filesystem::file_time_type::clock::now()
			+ chrono::system_clock::now()
		);
		time_t cftime = chrono::system_clock::to_time_t(sctp);
		wchar_t buf[64];
		struct tm timeinfo;
		//wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", localtime(&cftime));//localtime要换成更安全的localtime
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
					// 转换为 system_clock::time_point
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
		//wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", localtime(&cftime));//localtime要换成更安全的localtime
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

// 计算文件的哈希值（这是一个简单的实现，很不严格哒）
size_t CalculateFileHash(const filesystem::path& filepath) {
	ifstream file(filepath, ios::binary);
	if (!file) return 0;

	string content((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
	hash<string> hasher;
	return hasher(content);
}
// 作为全局变量，方便二者修改
map<wstring, size_t> currentState;
// 获取已更改的文件列表，并更新状态文件
wstring utf8_to_wstring(const string& str);
vector<filesystem::path> GetChangedFiles(const filesystem::path& worldPath, const filesystem::path& metadataPath) {
	vector<filesystem::path> changedFiles;
	map<wstring, size_t> lastState;
	filesystem::path stateFilePath = metadataPath / L"backup_state.txt";
	// 1. 读取上一次的状态
	ifstream stateFileIn(stateFilePath);
	if (stateFileIn.is_open()) {
		string path; // txt里千万不能有空格！
		size_t hash;
		while (stateFileIn >> path >> hash) {
			lastState[utf8_to_wstring(path)] = hash;
		}
		stateFileIn.close();
	}

	// 2. 计算当前状态并与上次状态比较
	if (filesystem::exists(worldPath)) {
		for (const auto& entry : filesystem::recursive_directory_iterator(worldPath)) {
			if (entry.is_regular_file()) {
				filesystem::path relativePath = filesystem::relative(entry.path(), worldPath);
				size_t currentHash = CalculateFileHash(entry.path());
				currentState[relativePath.wstring()] = currentHash;

				// 如果文件是新的，或者哈希值不同，则判定为已更改
				if (lastState.find(relativePath.wstring()) == lastState.end() || lastState[relativePath.wstring()] != currentHash) {
					changedFiles.push_back(entry.path());
				}
			}
		}
	}
	return changedFiles;
}
// 新的函数，专门用于保存状态文件 从filesystem版本修改为wofstream试图解决中文问题
void SaveStateFile(const filesystem::path& metadataPath) {
	filesystem::path stateFilePath = metadataPath / L"backup_state.txt";
	wofstream stateFileOut(stateFilePath, ios::trunc);
	stateFileOut.imbue(locale(stateFileOut.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
	for (const auto& pair : currentState) {
		stateFileOut << pair.first << L" " << pair.second << endl;
	}
	stateFileOut.close();
}

bool checkWorldName(const wstring& world, const vector<pair<wstring, wstring>>& worldList) {
	for (const pair<wstring, wstring>& worldLi : worldList) {
		if (world == worldLi.first)
			return false;
	}
	return true;
}

wstring utf8_to_wstring(const string& str);
// 开机自启功能终于来啦
void SetAutoStart(const string& appName, const wstring& appPath, int configId, bool enable) {
	HKEY hKey;
	const wstring keyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

	// LSTATUS是Windows API中标准返回类型
	LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, KEY_WRITE, &hKey);

	if (status == ERROR_SUCCESS) {
		if (enable) {
			wstring command = L"\"" + appPath + L"\" -specialcfg " + to_wstring(configId);
			// RegSetValueExW 需要6个参数: HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD
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
			// RegDeleteValueW 需要2个参数: HKEY, LPCWSTR
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
			c = L'_'; // 将非法字符替换为下划线
		}
	}
	return output;
}

// 一个简单的、不依赖库的JSON值查找器
string find_json_value(const string& json, const string& key) {
	string search_key = "\"" + key + "\":\"";
	size_t start_pos = json.find(search_key);
	if (start_pos == string::npos) {
		// 尝试查找非字符串值 (如 "key": true)
		search_key = "\"" + key + "\":";
		start_pos = json.find(search_key);
		if (start_pos == string::npos) return "";

		start_pos += search_key.length();
		while (start_pos < json.length() && isspace(json[start_pos])) start_pos++; // 跳过空格

		size_t end_pos = start_pos;
		while (end_pos < json.length() && json[end_pos] != ',' && json[end_pos] != '}') end_pos++;

		string val = json.substr(start_pos, end_pos - start_pos);
		// 去掉末尾的空格
		val.erase(val.find_last_not_of(" \n\r\t") + 1);
		return val;
	}

	start_pos += search_key.length();
	size_t end_pos = json.find("\"", start_pos);
	if (end_pos == string::npos) return "";

	return json.substr(start_pos, end_pos - start_pos);
}