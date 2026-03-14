#include <map>
#include <vector>
#include <fstream>
#include <sstream>
#include "AppState.h"
#include "json.hpp"
#include "text_to_text.h"
#include "PlatformCompat.h"
#include <algorithm>
#include <filesystem>
using namespace std;

static bool LoadLegacyHistoryFile(const filesystem::path& filename) {
	g_appState.g_history.clear();
	ifstream in(filename, ios::binary);
	if (!in.is_open()) return false;

	string line_utf8;
	int current_config_id = -1;

	while (getline(in, line_utf8)) {
		wstring line = utf8_to_wstring(line_utf8);
		if (line.empty() || line.front() == L'#') continue;

		if (line.front() == L'[' && line.back() == L']') {
			wstring section = line.substr(1, line.size() - 2);
			if (section.find(L"Config") == 0) {
				current_config_id = stoi(section.substr(6));
			}
		}
		else if (current_config_id != -1) {
			auto pos = line.find(L'=');
			if (pos == wstring::npos) continue;

			wstring key = line.substr(0, pos);
			wstring val = line.substr(pos + 1);
			if (key != L"Entry") continue;

			wstringstream ss(val);
			wstring segment;
			vector<wstring> segments;
			while (getline(ss, segment, L'|')) {
				segments.push_back(segment);
			}

			if (segments.size() >= 4) {
				HistoryEntry entry;
				entry.timestamp_str = segments[0];
				entry.worldName = segments[1];
				entry.backupFile = segments[2];
				entry.backupType = segments[3];
				entry.comment = segments.size() >= 5 ? segments[4] : L"";
				entry.isImportant = (segments.size() >= 6 && segments[5] == L"important");
				g_appState.g_history[current_config_id].push_back(entry);
			}
		}
	}

	return true;
}

// 将历史记录保存到 history.json 文件
void SaveHistory() {
	const filesystem::path filename = L"history.json";
#ifdef _WIN32
	SetFileAttributesWin(filename.wstring(), 0);
#endif
	ofstream out(filename, ios::binary | ios::trunc);
	if (!out.is_open()) return;

	nlohmann::json root = nlohmann::json::array();

	for (const auto& config_pair : g_appState.g_history) {
		for (const auto& entry : config_pair.second) {
			nlohmann::json item;
			item["configIndex"] = config_pair.first;
			item["timestamp"] = wstring_to_utf8(entry.timestamp_str);
			item["worldPath"] = wstring_to_utf8(entry.worldPath);
			item["worldName"] = wstring_to_utf8(entry.worldName);
			item["backupFile"] = wstring_to_utf8(entry.backupFile);
			item["backupType"] = wstring_to_utf8(entry.backupType);
			item["comment"] = wstring_to_utf8(entry.comment);
			item["isImportant"] = entry.isImportant;
			root.push_back(std::move(item));
		}
	}

	out << root.dump(2);
	out.close();
#ifdef _WIN32
	// 设置文件为隐藏属性
	SetFileAttributesWin(filename.wstring(), 1);
#endif
}

// 从文件加载历史记录
void LoadHistory() {
	const filesystem::path jsonFilename = L"history.json";
	const filesystem::path legacyFilename = L"history.dat";
	g_appState.g_history.clear();
	if (filesystem::exists(jsonFilename)) {
		try {
			ifstream in(jsonFilename, ios::binary);
			if (!in.is_open()) return;

			nlohmann::json root = nlohmann::json::parse(in);
			if (!root.is_array()) return;

			for (const auto& item : root) {
				if (!item.is_object()) continue;
				int configIndex = item.value("configIndex", -1);
				if (configIndex < 0) continue;

				HistoryEntry entry;
				entry.timestamp_str = utf8_to_wstring(item.value("timestamp", std::string{}));
				entry.worldPath = utf8_to_wstring(item.value("worldPath", std::string{}));
				entry.worldName = utf8_to_wstring(item.value("worldName", std::string{}));
				entry.backupFile = utf8_to_wstring(item.value("backupFile", std::string{}));
				entry.backupType = utf8_to_wstring(item.value("backupType", std::string{}));
				entry.comment = utf8_to_wstring(item.value("comment", std::string{}));
				entry.isImportant = item.value("isImportant", false);

				if (!entry.worldName.empty() && !entry.backupFile.empty()) {
					g_appState.g_history[configIndex].push_back(entry);
				}
			}
			return;
		}
		catch (...) {
			g_appState.g_history.clear();
		}
	}

	if (filesystem::exists(legacyFilename) && LoadLegacyHistoryFile(legacyFilename)) {
		SaveHistory();
	}
}


// 添加一条历史记录条目
void AddHistoryEntry(int configIndex, const wstring& worldName, const wstring& backupFile, const wstring& backupType, const wstring& comment, const wstring& worldPath) {
	time_t now = time(0);
	tm ltm;
	localtime_s(&ltm, &now);
	wchar_t timeBuf[80];
	wcsftime(timeBuf, std::size(timeBuf), L"%Y-%m-%d_%H-%M-%S", &ltm);

	HistoryEntry entry;
	entry.timestamp_str = timeBuf;
	entry.worldPath = worldPath;
	entry.worldName = worldName;
	entry.backupFile = backupFile;
	entry.backupType = backupType;
	entry.comment = comment;

	g_appState.g_history[configIndex].push_back(entry);
	SaveHistory();
}

// 删除指定的历史记录条目（通过备份文件名匹配）
void RemoveHistoryEntry(int configIndex, const wstring& backupFileToRemove) {
	if (g_appState.g_history.count(configIndex)) {
		auto& history_vec = g_appState.g_history[configIndex];
		history_vec.erase(
			remove_if(history_vec.begin(), history_vec.end(),
				[&](const HistoryEntry& entry) {
					return entry.backupFile == backupFileToRemove;
				}),
			history_vec.end()
		);
	}
}