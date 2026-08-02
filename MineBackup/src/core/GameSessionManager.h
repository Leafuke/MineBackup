#pragma once

#include <filesystem>
#include <stop_token>
#include <string>

struct MyFolder;

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
