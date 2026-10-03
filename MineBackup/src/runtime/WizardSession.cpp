#include "WizardSession.h"

#include "ConfigFactory.h"
#include "PathIdentity.h"
#include "text_to_text.h"

#include <algorithm>
#include <utility>

using namespace std;

wstring BuildWizardInstanceKey(const InspectedMinecraftInstance& instance) {
	return PathIdentity::BuildPathIdentityKey(instance.savesRoot);
}

void InvalidateWizardReadiness(WizardSession& session) {
	session.readiness = {};
}

uint64_t BeginWizardDiscovery(WizardSession& session) {
	++session.scanGeneration;
	if (session.scanGeneration == 0) ++session.scanGeneration;
	session.drafts.clear();
	InvalidateWizardReadiness(session);
	return session.scanGeneration;
}

bool ApplyWizardDiscoveryResult(
	WizardSession& session,
	uint64_t generation,
	MinecraftDiscoveryResult result) {
	if (generation != session.scanGeneration) return false;

	set<wstring> selectableKeys;
	for (const auto& candidate : result.instances) {
		if (!candidate.alreadyConfigured) {
			selectableKeys.insert(BuildWizardInstanceKey(candidate.instance));
		}
	}
	for (auto selected = session.selectedInstanceKeys.begin();
		selected != session.selectedInstanceKeys.end();) {
		if (!selectableKeys.contains(*selected)) {
			selected = session.selectedInstanceKeys.erase(selected);
		}
		else {
			++selected;
		}
	}
	session.discovery = std::move(result);
	session.drafts.clear();
	InvalidateWizardReadiness(session);
	return true;
}

bool SetWizardInstanceSelected(
	WizardSession& session,
	const wstring& instanceKey,
	bool selected) {
	const auto candidate = find_if(
		session.discovery.instances.begin(), session.discovery.instances.end(),
		[&](const auto& value) {
			return BuildWizardInstanceKey(value.instance) == instanceKey;
		});
	if (candidate == session.discovery.instances.end() || candidate->alreadyConfigured) {
		return false;
	}

	bool changed = false;
	if (selected) changed = session.selectedInstanceKeys.insert(instanceKey).second;
	else changed = session.selectedInstanceKeys.erase(instanceKey) != 0;
	if (changed) {
		session.drafts.clear();
		InvalidateWizardReadiness(session);
	}
	return changed;
}

void SetWizardDefaultBackupRoot(
	WizardSession& session,
	filesystem::path defaultBackupRoot) {
	defaultBackupRoot = defaultBackupRoot.lexically_normal();
	if (session.defaultBackupRoot == defaultBackupRoot) return;
	session.defaultBackupRoot = std::move(defaultBackupRoot);
	session.drafts.clear();
	InvalidateWizardReadiness(session);
}

const vector<ConfigDraft>& RebuildWizardDrafts(
	WizardSession& session,
	const map<int, Config>& existingConfigs) {
	vector<ConfigDraft> selectedDrafts;
	for (const auto& candidate : session.discovery.instances) {
		const auto& instance = candidate.instance;
		const wstring key = BuildWizardInstanceKey(instance);
		if (candidate.alreadyConfigured
			|| !session.selectedInstanceKeys.contains(key)) {
			continue;
		}

		ConfigDraft draft;
		draft.name = wstring_to_utf8(instance.suggestedName);
		if (draft.name.empty()) draft.name = "Minecraft";
		draft.edition = instance.edition;
		draft.saveRoot = instance.savesRoot;
		for (const auto& world : instance.worlds) {
			wstring description = world.displayName.empty()
				? world.folderName : world.displayName;
			draft.worlds.emplace_back(world.relativePath.wstring(), std::move(description));
		}
		selectedDrafts.push_back(std::move(draft));
	}

	session.drafts = ResolveUniqueConfigDrafts(
		selectedDrafts, session.defaultBackupRoot, existingConfigs);
	InvalidateWizardReadiness(session);
	return session.drafts;
}
