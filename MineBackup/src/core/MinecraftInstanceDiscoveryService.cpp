#include "MinecraftInstanceDiscoveryService.h"

#include "KnownMinecraftLocationProvider.h"
#include "Logging.h"
#include "PathIdentity.h"
#include "Pcl2ProcessDiscoveryProvider.h"
#include "ProcessInspectionService.h"

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>

using namespace std;

namespace {

wstring Lower(wstring value) {
	transform(value.begin(), value.end(), value.begin(), ::towlower);
	return value;
}

int LocationKindRank(DiscoveryLocationKind kind) {
	switch (kind) {
	case DiscoveryLocationKind::BedrockWorldsRoot: return 0;
	case DiscoveryLocationKind::SavesRoot: return 1;
	case DiscoveryLocationKind::VersionRoot: return 2;
	case DiscoveryLocationKind::MinecraftRoot: return 3;
	case DiscoveryLocationKind::Manual: return 4;
	}
	return 5;
}

int EvidenceRank(const DiscoveryEvidence& evidence) {
	switch (evidence.kind) {
	case DiscoveryEvidenceKind::LauncherSettings: return 0;
	case DiscoveryEvidenceKind::LauncherProcess:
	case DiscoveryEvidenceKind::WorkspaceProbe:
	case DiscoveryEvidenceKind::ExistingConfig: return 1;
	case DiscoveryEvidenceKind::KnownLocation: return 2;
	case DiscoveryEvidenceKind::Manual: return 3;
	}
	return 4;
}

filesystem::path InferMinecraftRootFromSaveRoot(const filesystem::path& saveRoot) {
	if (saveRoot.empty() || !saveRoot.is_absolute()) return {};

	const filesystem::path normalizedSaveRoot =
		PathIdentity::NormalizeExistingOrProspectivePath(saveRoot);
	if (Lower(normalizedSaveRoot.filename().wstring()) != L"saves") return {};

	const filesystem::path instanceParent = normalizedSaveRoot.parent_path();
	if (Lower(instanceParent.filename().wstring()) == L".minecraft") {
		return instanceParent;
	}

	const filesystem::path versionsRoot = instanceParent.parent_path();
	const filesystem::path minecraftRoot = versionsRoot.parent_path();
	if (Lower(versionsRoot.filename().wstring()) != L"versions"
		|| Lower(minecraftRoot.filename().wstring()) != L".minecraft") {
		return {};
	}
	return minecraftRoot;
}

vector<DiscoveryLocation> BuildExistingConfigDiscoveryLocations(
	const map<int, Config>& existingConfigs) {
	vector<DiscoveryLocation> locations;
	for (const auto& [index, config] : existingConfigs) {
		(void)index;
		const filesystem::path saveRoot(config.saveRoot);
		const filesystem::path minecraftRoot =
			InferMinecraftRootFromSaveRoot(saveRoot);
		if (minecraftRoot.empty()) continue;

		locations.push_back({
			minecraftRoot,
			DiscoveryLocationKind::MinecraftRoot,
			{{DiscoveryEvidenceKind::ExistingConfig,
				"existing-config", saveRoot}}});
	}
	return locations;
}

tuple<int, string, wstring> EvidenceKey(const DiscoveryEvidence& evidence) {
	return {static_cast<int>(evidence.kind), evidence.providerId,
		evidence.source.empty() ? wstring{} : PathIdentity::BuildPathIdentityKey(evidence.source)};
}

void MergeEvidence(
	vector<DiscoveryEvidence>& destination,
	const vector<DiscoveryEvidence>& source) {
	set<tuple<int, string, wstring>> seen;
	for (const auto& evidence : destination) seen.insert(EvidenceKey(evidence));
	for (const auto& evidence : source) {
		if (seen.insert(EvidenceKey(evidence)).second) destination.push_back(evidence);
	}
	stable_sort(destination.begin(), destination.end(), [](const auto& left, const auto& right) {
		return tuple(EvidenceRank(left), left.providerId, EvidenceKey(left))
			< tuple(EvidenceRank(right), right.providerId, EvidenceKey(right));
	});
}

void MergeWorlds(
	vector<DiscoveredMinecraftWorld>& destination,
	const vector<DiscoveredMinecraftWorld>& source) {
	set<wstring> seen;
	for (const auto& world : destination) {
		seen.insert(PathIdentity::BuildPathIdentityKey(world.absolutePath));
	}
	for (const auto& world : source) {
		if (seen.insert(PathIdentity::BuildPathIdentityKey(world.absolutePath)).second) {
			destination.push_back(world);
		}
	}
	stable_sort(destination.begin(), destination.end(), [](const auto& left, const auto& right) {
		const auto leftName = Lower(left.folderName);
		const auto rightName = Lower(right.folderName);
		return leftName != rightName ? leftName < rightName
			: left.relativePath.wstring() < right.relativePath.wstring();
	});
}

int BestEvidenceRank(const InspectedMinecraftInstance& instance) {
	int rank = 4;
	for (const auto& evidence : instance.evidence) rank = (min)(rank, EvidenceRank(evidence));
	return rank;
}

} // namespace

MinecraftInstanceDiscoveryService::MinecraftInstanceDiscoveryService(
	vector<shared_ptr<IMinecraftDiscoveryProvider>> providers,
	MinecraftInstanceInspector inspector)
	: providers_(std::move(providers)), inspector_(std::move(inspector)) {}

MinecraftDiscoveryResult MinecraftInstanceDiscoveryService::Discover(
	const map<int, Config>& existingConfigs,
	vector<DiscoveryLocation> additionalLocations,
	stop_token stopToken) const {
	MinecraftDiscoveryResult result;
	vector<DiscoveryLocation> locations = std::move(additionalLocations);
	const auto existingConfigLocations =
		BuildExistingConfigDiscoveryLocations(existingConfigs);
	locations.insert(locations.end(), existingConfigLocations.begin(),
		existingConfigLocations.end());
	MB_LOG_INFO(minebackup::logging::LogCategory::Application,
		"minecraft.discovery.started", "providers={}", providers_.size());

	MinecraftDiscoveryContext context{
		[&](DiscoveryDiagnostic diagnostic) {
			result.diagnostics.push_back(std::move(diagnostic));
		}};
	for (const auto& provider : providers_) {
		if (stopToken.stop_requested()) break;
		if (!provider) continue;
		const size_t before = locations.size();
		try {
			auto discovered = provider->DiscoverLocations(context, stopToken);
			locations.insert(locations.end(),
				make_move_iterator(discovered.begin()), make_move_iterator(discovered.end()));
			MB_LOG_INFO(minebackup::logging::LogCategory::Application,
				"minecraft.discovery.provider_completed", "provider_id={} locations={}",
				provider->Id(), locations.size() - before);
		}
		catch (...) {
			result.diagnostics.push_back({
				"provider_failed", provider->Id(), {}, {}});
		}
	}

	map<wstring, DiscoveryLocation> uniqueLocations;
	for (auto& location : locations) {
		if (location.path.empty()) continue;
		const wstring key = PathIdentity::BuildPathIdentityKey(location.path);
		auto [position, inserted] = uniqueLocations.try_emplace(key, std::move(location));
		if (!inserted) {
			if (LocationKindRank(location.kind) < LocationKindRank(position->second.kind)) {
				position->second.kind = location.kind;
			}
			MergeEvidence(position->second.evidence, location.evidence);
		}
	}

	map<wstring, InspectedMinecraftInstance> uniqueInstances;
	for (const auto& [key, location] : uniqueLocations) {
		(void)key;
		if (stopToken.stop_requested()) break;
		auto inspected = inspector_.InspectDetailed(location, stopToken);
		result.diagnostics.insert(result.diagnostics.end(),
			make_move_iterator(inspected.diagnostics.begin()),
			make_move_iterator(inspected.diagnostics.end()));
		for (auto& instance : inspected.instances) {
			const wstring instanceKey = PathIdentity::BuildPathIdentityKey(instance.savesRoot);
			auto [position, inserted] = uniqueInstances.try_emplace(
				instanceKey, std::move(instance));
			if (!inserted) {
				MergeEvidence(position->second.evidence, instance.evidence);
				MergeWorlds(position->second.worlds, instance.worlds);
			}
		}
	}

	set<wstring> configuredSaveRoots;
	for (const auto& [index, config] : existingConfigs) {
		(void)index;
		if (!config.saveRoot.empty()) {
			configuredSaveRoots.insert(PathIdentity::BuildPathIdentityKey(config.saveRoot));
		}
	}
	for (auto& [key, instance] : uniqueInstances) {
		result.instances.push_back({
			std::move(instance), configuredSaveRoots.contains(key)});
	}
	stable_sort(result.instances.begin(), result.instances.end(), [](const auto& left, const auto& right) {
		if (left.alreadyConfigured != right.alreadyConfigured) return !left.alreadyConfigured;
		const int leftEvidence = BestEvidenceRank(left.instance);
		const int rightEvidence = BestEvidenceRank(right.instance);
		if (leftEvidence != rightEvidence) return leftEvidence < rightEvidence;
		const auto leftName = Lower(left.instance.suggestedName);
		const auto rightName = Lower(right.instance.suggestedName);
		if (leftName != rightName) return leftName < rightName;
		return PathIdentity::BuildPathIdentityKey(left.instance.savesRoot)
			< PathIdentity::BuildPathIdentityKey(right.instance.savesRoot);
	});

	const size_t configured = static_cast<size_t>(count_if(
		result.instances.begin(), result.instances.end(),
		[](const auto& instance) { return instance.alreadyConfigured; }));
	MB_LOG_INFO(minebackup::logging::LogCategory::Application,
		"minecraft.discovery.completed", "locations={} instances={} configured={}",
		uniqueLocations.size(), result.instances.size(), configured);
	return result;
}

MinecraftInstanceDiscoveryService CreateDefaultMinecraftDiscoveryService() {
	vector<shared_ptr<IMinecraftDiscoveryProvider>> providers;
	providers.push_back(make_shared<KnownMinecraftLocationProvider>());
	if (GetProcessInspectionAvailability() == ProcessInspectionAvailability::Available) {
		providers.push_back(make_shared<Pcl2ProcessDiscoveryProvider>(
			CreateProcessInspectionService()));
	}
	return MinecraftInstanceDiscoveryService(std::move(providers));
}
