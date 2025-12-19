#include <map>
#include <vector>
#include <fstream>
#include <sstream>
#include "AppState.h"
#include "text_to_text.h"
#include <algorithm>
#include <filesystem>
using namespace std;

void SetFileAttributesWin(const wstring& path, bool isHidden);
// 将历史记录保存到 history.dat 文件
// 文件结构：
// [Config<id>]
// Entry=<timestamp>|<worldName>|<backupFile>|<backupType>|<comment>
void SaveHistory() {
	const filesystem::path filename = L"history.dat"; // 使用 .dat 保存历史记录
#ifdef _WIN32
	SetFileAttributesWin(filename.wstring(), 0);
#endif
	ofstream out(filename, ios::binary);
	if (!out.is_open()) return;

	auto write_utf8 = [&out](const wstring& line) {
		string utf8 = wstring_to_utf8(line);
		out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
	};

	for (const auto& config_pair : g_appState.g_history) {
		write_utf8(L"[Config" + to_wstring(config_pair.first) + L"]\n");
		for (const auto& entry : config_pair.second) {
			// 使用 | 作为分隔符
			wstring line = L"Entry=" + entry.timestamp_str + L"|" + entry.worldName + L"|" + entry.backupFile + L"|" + entry.backupType + L"|" + entry.comment;
			if (entry.isImportant) {
				line += L"|important";
			}
			line += L"\n";
			write_utf8(line);
		}
	}
	out.close();
#ifdef _WIN32
	// 设置文件为隐藏属性
	SetFileAttributesWin(filename.wstring(), 1);
#endif
}

// 暂时不使用自动标记的方案
//void UpdateAutoPinnedFullBackup(int configIndex, const wstring& worldName, const wstring& latestFullBackupFile) {
//	if (!g_appState.g_history.count(configIndex)) return;
//	bool changed = false;
//	auto& history_vec = g_appState.g_history[configIndex];
//	for (auto& entry : history_vec) {
//		if (entry.worldName != worldName) continue;
//		if (entry.backupFile == latestFullBackupFile) {
//			if (!entry.isImportant || !entry.isAutoImportant) {
//				entry.isImportant = true;
//				entry.isAutoImportant = true;
//				changed = true;
//			}
//		}
//		else if (entry.isAutoImportant) {
//			entry.isAutoImportant = false;
//			entry.isImportant = false;
//			changed = true;
//		}
//	}
//	if (changed) {
//		SaveHistory();
//	}
//}

// 从文件加载历史记录
void LoadHistory() {
	const filesystem::path filename = L"history.dat";
	g_appState.g_history.clear();
	ifstream in(filename, ios::binary);
	if (!in.is_open()) return;

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
			if (pos != wstring::npos) {
				wstring key = line.substr(0, pos);
				wstring val = line.substr(pos + 1);
				if (key == L"Entry") {
					wstringstream ss(val);
					wstring segment;
					vector<wstring> segments;
					while (getline(ss, segment, L'|')) {
						segments.push_back(segment);
					}
					if (segments.size() >= 5) {
						HistoryEntry entry;
						entry.timestamp_str = segments[0];
						entry.worldName = segments[1];
						entry.backupFile = segments[2];
						entry.backupType = segments[3];
						entry.comment = segments[4];
						entry.isImportant = (segments.size() >= 6 && segments[5] == L"important");
						g_appState.g_history[current_config_id].push_back(entry);
					}
					else if (segments.size() == 4) { // 没有设置注释
						HistoryEntry entry;
						entry.timestamp_str = segments[0];
						entry.worldName = segments[1];
						entry.backupFile = segments[2];
						entry.backupType = segments[3];
						entry.comment = L"";
						g_appState.g_history[current_config_id].push_back(entry);
					}
				}
			}
		}
	}
}


// 添加一条历史记录条目
void AddHistoryEntry(int configIndex, const wstring& worldName, const wstring& backupFile, const wstring& backupType, const wstring& comment) {
	time_t now = time(0);
	tm ltm;
	localtime_s(&ltm, &now);
	wchar_t timeBuf[80];
	wcsftime(timeBuf, sizeof(timeBuf), L"%Y-%m-%d_%H-%M-%S", &ltm);

	HistoryEntry entry;
	entry.timestamp_str = timeBuf;
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