#include "json.hpp"
#include "text_to_text.h"
#include <string>
#include <map>
#include <filesystem>
#include <fstream>
#include <regex>
using namespace std;

bool IsPureASCII(const wstring& s) {
	for (wchar_t c : s) {
		if (c < 0 || c > 127) {
			return false;
		}
	}
	return true;
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

// 定义检查结果的枚举类型
enum class BackupCheckResult {
	NO_CHANGE,
	CHANGES_DETECTED,
	FORCE_FULL_BACKUP_METADATA_INVALID,
	FORCE_FULL_BACKUP_BASE_MISSING
};

// 全局变量 currentState 不再需要，作用域移至函数内部

// 位于 basic_func.cpp
vector<filesystem::path> GetChangedFiles(
	const filesystem::path& worldPath,
	const filesystem::path& metadataPath,
	const filesystem::path& backupPath, // 需要传入备份路径以供验证
	BackupCheckResult& out_result,
	map<wstring, size_t>& out_currentState // 将当前状态传出，供后续保存
) {
	out_result = BackupCheckResult::NO_CHANGE;
	out_currentState.clear();
	vector<filesystem::path> changedFiles;
	map<wstring, size_t> lastState;
	filesystem::path metadataFile = metadataPath / L"metadata.json";

	// 1. 读取并验证元数据
	if (!filesystem::exists(metadataFile)) {
		out_result = BackupCheckResult::FORCE_FULL_BACKUP_METADATA_INVALID;
		// 元数据不存在，扫描所有文件并返回，以便进行首次完整备份
		for (const auto& entry : filesystem::recursive_directory_iterator(worldPath)) {
			if (entry.is_regular_file()) {
				out_currentState[filesystem::relative(entry.path(), worldPath).wstring()] = CalculateFileHash(entry.path());
			}
		}
		return {}; // 返回空列表，因为所有文件状态都记录在 out_currentState 中了
	}

	nlohmann::json metadata;
	wstring basedOnBackupFile;
	try {
		ifstream f(metadataFile.c_str());
		metadata = nlohmann::json::parse(f);
		basedOnBackupFile = utf8_to_wstring(metadata.at("basedOnBackupFile"));
		for (auto& [key, val] : metadata.at("fileStates").items()) {
			lastState[utf8_to_wstring(key)] = val.get<size_t>();
		}
	}
	catch (const nlohmann::json::exception& e) {
		// 元数据文件损坏或格式错误
		out_result = BackupCheckResult::FORCE_FULL_BACKUP_METADATA_INVALID;
		// 同样需要扫描所有文件
		for (const auto& entry : filesystem::recursive_directory_iterator(worldPath)) {
			if (entry.is_regular_file()) {
				out_currentState[filesystem::relative(entry.path(), worldPath).wstring()] = CalculateFileHash(entry.path());
			}
		}
		return {};
	}

	// 2. 核心验证：检查元数据依赖的基准备份文件是否存在
	if (!filesystem::exists(backupPath / basedOnBackupFile)) {
		out_result = BackupCheckResult::FORCE_FULL_BACKUP_BASE_MISSING;
		// 基准文件被用户删除，元数据失效，扫描所有文件以进行新的完整备份
		for (const auto& entry : filesystem::recursive_directory_iterator(worldPath)) {
			if (entry.is_regular_file()) {
				out_currentState[filesystem::relative(entry.path(), worldPath).wstring()] = CalculateFileHash(entry.path());
			}
		}
		return {};
	}

	// 3. 计算当前状态并与上次状态比较
	if (filesystem::exists(worldPath)) {
		for (const auto& entry : filesystem::recursive_directory_iterator(worldPath)) {
			if (entry.is_regular_file()) {
				filesystem::path relativePath = filesystem::relative(entry.path(), worldPath);
				//filesystem::path fileName = entry.path().filename();
				size_t currentHash = CalculateFileHash(entry.path());
				out_currentState[relativePath.wstring()] = currentHash;

				//// 将filename和hash都输出一下，用来检查问题，输出到debug.txt文件
				/*ofstream debugFile("debug.txt", ios::app);
				debugFile << wstring_to_utf8(fileName.wstring()) << " " << currentHash << endl;*/

				// 如果文件是新的，或者哈希值不同，则判定为已更改
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




// 获取已更改的文件列表，并更新状态文件
//wstring utf8_to_wstring(const string& str);
//vector<filesystem::path> GetChangedFiles(const filesystem::path& worldPath, const filesystem::path& metadataPath) {
//	vector<filesystem::path> changedFiles;
//	map<wstring, size_t> lastState;
//	filesystem::path stateFilePath = metadataPath / L"backup_state.txt";
//	// 1. 读取上一次的状态
//	ifstream stateFileIn(stateFilePath);
//	if (stateFileIn.is_open()) {
//		string path; // txt里千万不能有空格！
//		size_t hash;
//		while (stateFileIn >> path >> hash) {
//			lastState[utf8_to_wstring(path)] = hash;
//		}
//		stateFileIn.close();
//	}
//
//	// 2. 计算当前状态并与上次状态比较
//	if (filesystem::exists(worldPath)) {
//		for (const auto& entry : filesystem::recursive_directory_iterator(worldPath)) {
//			if (entry.is_regular_file()) {
//				filesystem::path relativePath = filesystem::relative(entry.path(), worldPath);
//				size_t currentHash = CalculateFileHash(entry.path());
//				currentState[relativePath.wstring()] = currentHash;
//
//				// 如果文件是新的，或者哈希值不同，则判定为已更改
//				if (lastState.find(relativePath.wstring()) == lastState.end() || lastState[relativePath.wstring()] != currentHash) {
//					changedFiles.push_back(entry.path());
//				}
//			}
//		}
//	}
//	return changedFiles;
//}
bool checkWorldName(const wstring& world, const vector<pair<wstring, wstring>>& worldList) {
	for (const pair<wstring, wstring>& worldLi : worldList) {
		if (world == worldLi.first)
			return false;
	}
	return true;
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

/**
 * @brief 检查文件路径是否符合黑名单规则（支持普通字符串和正则表达式）
 * @param file_to_check 待检查的文件的完整路径
 * @param backup_source_root 当前备份操作的根目录（可能是原始存档路径，也可能是热备份快照路径）
 * @param original_world_root 原始的、在config中配置的存档根目录
 * @param blacklist 黑名单规则列表
 * @param console 用于日志输出的控制台对象
 * @return 如果文件被命中黑名单，则返回 true
 */
bool is_blacklisted(
	const filesystem::path& file_to_check,
	const filesystem::path& backup_source_root,
	const filesystem::path& original_world_root,
	const vector<wstring>& blacklist)
{
	// 将路径转换为小写以进行不区分大小写的字符串比较
	wstring file_path_lower = file_to_check.wstring();
	transform(file_path_lower.begin(), file_path_lower.end(), file_path_lower.begin(), ::towlower);

	// 获取相对于当前备份源的相对路径
	error_code ec;
	filesystem::path relative_path_obj = filesystem::relative(file_to_check, backup_source_root, ec);
	wstring relative_path_lower = ec ? L"" : relative_path_obj.wstring();
	transform(relative_path_lower.begin(), relative_path_lower.end(), relative_path_lower.begin(), ::towlower);


	for (const auto& rule_orig : blacklist) {
		wstring rule = rule_orig;
		transform(rule.begin(), rule.end(), rule.begin(), ::towlower);

		if (rule.rfind(L"regex:", 0) == 0) { // 正则表达式规则
			try {
				wstring pattern_str = rule_orig.substr(6); // 使用原始大小写的规则进行正则匹配
				wregex pattern(pattern_str, regex_constants::icase | regex_constants::ECMAScript);

				// 正则表达式同时匹配绝对路径和相对路径
				if (regex_search(file_to_check.wstring(), pattern) ||
					(!relative_path_obj.empty() && regex_search(relative_path_obj.wstring(), pattern))) {
					return true;
				}
			}
			catch (const regex_error& e) {
				//console.AddLog("[Error] Invalid regex in blacklist: %s. Error: %s", wstring_to_utf8(rule_orig).c_str(), e.what());
			}
		}
		else { // 普通字符串规则
			// 规则可能是绝对路径或相对路径片段
			// 1. 检查是否直接匹配完整路径
			if (file_path_lower.find(rule) != wstring::npos) {
				return true;
			}

			// 2. 为了处理热备份，如果规则是绝对路径，我们需要将其动态映射到快照路径
			filesystem::path rule_path(rule_orig);
			if (rule_path.is_absolute()) {
				// 检查规则路径是否在原始世界路径之下
				auto res = mismatch(original_world_root.begin(), original_world_root.end(), rule_path.begin());
				if (res.first == original_world_root.end()) {
					// 是的，规则是原始世界的一个子路径。现在我们构建它在快照中的对应路径。
					error_code rel_ec;
					filesystem::path rule_relative_to_world = filesystem::relative(rule_path, original_world_root, rel_ec);
					if (!rel_ec) {
						filesystem::path remapped_rule_path = backup_source_root / rule_relative_to_world;
						wstring remapped_rule_lower = remapped_rule_path.wstring();
						transform(remapped_rule_lower.begin(), remapped_rule_lower.end(), remapped_rule_lower.begin(), ::towlower);

						// 检查文件是否位于重映射后的黑名单路径下
						if (file_path_lower.rfind(remapped_rule_lower, 0) == 0) {
							return true;
						}
					}
				}
			}
		}
	}

	return false;
}