#include <cstdio>
#include <string>
#include <map>
#include <vector>
#include <fstream>
#include <sstream>
#include <Windows.h>
using namespace std;

string wstring_to_utf8(const wstring& wstr);
wstring utf8_to_wstring(const string& str);
string gbk_to_utf8(const string& gbk);
string utf8_to_gbk(const string& utf8);

extern struct HistoryEntry {
	wstring timestamp_str;
	wstring worldName;
	wstring backupFile;
	wstring backupType;
	wstring comment;
};
extern map<int, vector<HistoryEntry>> g_history;

// ����������ʷ��¼д���ļ�
void SaveHistory() {
	const wstring filename = L"history.dat"; // ʹ�� .dat ����Ϊ���أ������û������
	SetFileAttributes(filename.c_str(), FILE_ATTRIBUTE_NORMAL);
	wofstream out(filename, ios::binary);
	out.clear();
	if (!out.is_open()) return;
	out.imbue(locale(out.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));

	for (const auto& config_pair : g_history) {
		out << L"[Config" << config_pair.first << L"]\n";
		for (const auto& entry : config_pair.second) {
			// ʹ�� | ��Ϊ�ָ���
			out << L"Entry=" << entry.timestamp_str << L"|"
				<< entry.worldName << L"|"
				<< entry.backupFile << L"|"
				<< entry.backupType << L"|"
				<< entry.comment << L"\n";
		}
	}
	out.close();
	// �����ļ�Ϊ��������
	SetFileAttributesW(filename.c_str(), FILE_ATTRIBUTE_HIDDEN);
}

// ���������ļ�������ʷ��¼
void LoadHistory() {
	const wstring filename = L"history.dat";
	g_history.clear();
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
					if (segments.size() == 5) {
						HistoryEntry entry;
						entry.timestamp_str = segments[0];
						entry.worldName = segments[1];
						entry.backupFile = segments[2];
						entry.backupType = segments[3];
						entry.comment = segments[4];
						g_history[current_config_id].push_back(entry);
					}
					else if (segments.size() == 4) { // �û�û������ע��
						HistoryEntry entry;
						entry.timestamp_str = segments[0];
						entry.worldName = segments[1];
						entry.backupFile = segments[2];
						entry.backupType = segments[3];
						entry.comment = L"";
						g_history[current_config_id].push_back(entry);
					}
				}
			}
		}
	}
}


// ���������һ����ʷ��¼��Ŀ
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

	g_history[configIndex].push_back(entry);
	SaveHistory();
}