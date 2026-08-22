#pragma once
#ifndef _CONFIG_MANAGER_H
#define _CONFIG_MANAGER_H
#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include "LegacyIniConfigCodec.h"

struct NormalConfigIndexAllocatorState {
	int nextIndex = 2;
};

// 配置持久化的三态提交结果：
// - NotCommitted        config.ini 从未被替换，调用方可安全回滚内存状态；
// - CommittedNotDurable config.ini 已被替换，但目录同步未确认（持久化告警），
//                       逻辑上配置已提交，禁止内存回滚；
// - CommittedDurably    配置已提交且持久化步骤全部完成。
enum class ConfigSaveState {
	NotCommitted,
	CommittedNotDurable,
	CommittedDurably
};

struct ConfigSaveResult {
	ConfigSaveState state = ConfigSaveState::NotCommitted;
	std::wstring detail;

	// 逻辑 commit 是否已发生（CommittedNotDurable 也算已提交）。
	bool Committed() const noexcept { return state != ConfigSaveState::NotCommitted; }
	bool Durable() const noexcept { return state == ConfigSaveState::CommittedDurably; }
};

void LoadConfigs();
void LoadConfigs(const std::filesystem::path& filename);
std::filesystem::path GetEffectiveDefaultBackupRoot();
// 布尔契约：返回“逻辑 commit 是否已发生”，而非“目录 fsync 是否确认”。
// NotCommitted -> false；CommittedNotDurable / CommittedDurably -> true。
bool SaveConfigs();
bool SaveConfigs(const std::filesystem::path& filename);
// 需要区分持久化阶段（是否可回滚内存）的调用方应使用 Detailed 版本。
ConfigSaveResult SaveConfigsDetailed();
ConfigSaveResult SaveConfigsDetailed(const std::filesystem::path& filename);
void AddHistoryEntry(int configIndex, const std::wstring& worldName, const std::wstring& backupFile, const std::wstring& backupType, const std::wstring& comment, const std::wstring& worldPath = L"");
void RemoveHistoryEntry(int configIndex, const std::wstring& backupFileToRemove);
int CreateNewNormalConfig(const std::string& name_hint = "None");
NormalConfigIndexAllocatorState SnapshotNormalConfigIndexAllocator();
void RestoreNormalConfigIndexAllocator(NormalConfigIndexAllocatorState state);
int AllocateNormalConfigIndex();
void AssignFreshNormalConfigId(int configIndex);
void EnsureConfigIds();
std::vector<std::wstring> DefaultBackupBlacklist();
std::vector<std::wstring> DefaultRestoreWhitelist();
std::vector<std::wstring> BuildEffectiveRestoreWhitelist(const std::vector<std::wstring>& userWhitelist);
void EnsureDefaultBackupBlacklist(std::vector<std::wstring>& blacklist);
void EnsureDefaultRestoreWhitelist();
void FinalizeUiScaleMigration(float primaryDpiScale);
void CheckForConfigConflicts();
const std::vector<LegacyIniConfigCodec::Diagnostic>& GetLastConfigLoadDiagnostics();
bool LastConfigLoadHasFatalDiagnostics();
#endif // CONFIG_MANAGER_H
