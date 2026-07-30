#pragma once

#include <stop_token>
#include <string>

struct MyFolder;

MyFolder GetOccupiedWorld();
void GameSessionWatcherThread(std::stop_token stopToken);
void TriggerHotkeyBackup(std::string comment);
void TriggerHotkeyRestore(const std::string& backupFile);
