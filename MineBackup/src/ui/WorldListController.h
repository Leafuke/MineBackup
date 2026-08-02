#pragma once

#include "WorldListModel.h"

#include <chrono>
#include <map>
#include <string>

struct WorldIconCache {
	std::map<std::wstring, unsigned int> textures;
};

struct WorldListController {
	int selectedWorldIndex = -1;
	char backupComment[1024]{};
	std::vector<DisplayWorld> displayWorlds;
	int cachedConfigIndex = -999;
	bool cachedSpecialSetting = false;
	std::size_t cachedWorldCount = 0;
	std::chrono::steady_clock::time_point lastDisplayWorldsRefresh{};
	std::map<std::wstring, std::wstring> cachedOpenTimes;
	std::map<std::wstring, std::wstring> cachedBackupTimes;
	std::map<std::wstring, bool> cachedNeedsBackup;
	std::chrono::steady_clock::time_point lastTimeCacheRefresh{};
	std::map<std::pair<int, int>, bool> cachedTaskRunning;
	WorldIconCache iconCache;

	bool showAddConfigPopup = false;
	bool showDeleteConfigPopup = false;
	int configType = 0;
	char newConfigName[128] = "New Config";
	char modsComment[256]{};
	char pathBuffer[1024]{};
	char othersComment[1024]{};
	Config temporaryExportConfig;
	char outputPath[260]{};
	char description[2048]{};
	char blacklistItem[1024]{};
	int selectedBlacklistItem = -1;
	int selectedFormat = 0;

	void ResetSelection();
	void Invalidate();
};
