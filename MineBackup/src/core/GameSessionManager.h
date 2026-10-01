#pragma once

#include <filesystem>
#include <stop_token>
#include <string>
#include <functional>
#include <map>
#include <optional>
#include <vector>
#include "DataModels.h"

using SessionWorldKey = std::pair<std::wstring, std::wstring>;
SessionWorldKey GameSessionWorldKey(const MyFolder& world);
std::vector<MyFolder> EnumerateOccupiedWorlds(const std::map<int, Config>& configs,
    const std::function<bool(const std::filesystem::path&)>& occupied);
std::optional<MyFolder> ResolveSessionWorld(const std::map<int, Config>& configs, const SessionWorldKey& key);
struct GameSessionChanges { std::vector<MyFolder> started, ended; };
class GameSessionTracker {
public:
    GameSessionChanges Poll(const std::vector<MyFolder>& current);
private:
    std::map<SessionWorldKey, MyFolder> active_;
};

MyFolder GetOccupiedWorld();
bool IsWorldOccupied(const std::filesystem::path& worldPath);
bool SubmitUserRestore(
	const MyFolder& world,
	const std::wstring& backupFile,
	int restoreMethod,
	std::string customRestoreList,
	bool backupBeforeRestore);
void GameSessionWatcherThread(std::stop_token stopToken);
void TriggerHotkeyBackup(std::string comment);
void TriggerHotkeyRestore(const std::string& backupFile);
