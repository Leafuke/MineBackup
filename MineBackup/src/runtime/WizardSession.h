#pragma once

#include "BatchReadinessService.h"
#include "MinecraftDiscovery.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

enum class WizardStage {
	Discover,
	BackupLocation,
	Ready,
	CoreValidation
};

struct WizardSession {
	WizardStage stage = WizardStage::Discover;
	std::uint64_t scanGeneration = 0;
	MinecraftDiscoveryResult discovery;
	std::set<std::wstring> selectedInstanceKeys;
	std::filesystem::path defaultBackupRoot;
	std::vector<ConfigDraft> drafts;
	BatchReadinessResult readiness;
	std::vector<int> committedConfigIndices;
};

std::wstring BuildWizardInstanceKey(const InspectedMinecraftInstance& instance);

std::uint64_t BeginWizardDiscovery(WizardSession& session);

bool ApplyWizardDiscoveryResult(
	WizardSession& session,
	std::uint64_t generation,
	MinecraftDiscoveryResult result);

bool SetWizardInstanceSelected(
	WizardSession& session,
	const std::wstring& instanceKey,
	bool selected);

void SetWizardDefaultBackupRoot(
	WizardSession& session,
	std::filesystem::path defaultBackupRoot);

const std::vector<ConfigDraft>& RebuildWizardDrafts(
	WizardSession& session,
	const std::map<int, Config>& existingConfigs);

void InvalidateWizardReadiness(WizardSession& session);
