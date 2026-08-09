#include "ProfileKnotLinkCommands.h"

#include "FolderRewindFormat.h"
#include "MineBackupVersion.h"
#include "ProfileRuntime.h"
#include "RuntimeFileLock.h"
#include "RuntimeIntegration.h"
#include "text_to_text.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

using namespace std;
using namespace minebackup::knotlink;

namespace {

struct ResolvedTarget {
	Config config;
	wstring world;
	filesystem::path fullPath;
};

string LowerAscii(string value) {
	transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
		return static_cast<char>(tolower(character));
	});
	return value;
}

optional<bool> ParseBoolean(string_view value) {
	const string normalized = LowerAscii(string(value));
	if (normalized == "true") return true;
	if (normalized == "false") return false;
	return nullopt;
}

bool TryParseIndex(string_view value, size_t& result) {
	if (value.empty()) return false;
	const char* begin = value.data();
	const char* end = begin + value.size();
	const auto parsed = from_chars(begin, end, result);
	return parsed.ec == errc{} && parsed.ptr == end;
}

string JoinDelimited(const vector<string>& values) {
	string result;
	for (size_t index = 0; index < values.size(); ++index) {
		if (index != 0) result.push_back(';');
		result += values[index];
	}
	return result;
}

string BackupModeName(int mode) {
	if (mode == 2) return "Incremental";
	if (mode == 3) return "Overwrite";
	return "Full";
}

const Config* ResolveConfig(
	const ProfileConfigCatalog& catalog,
	string_view identifier) {
	if (identifier.empty()) return nullptr;
	const wstring wide = utf8_to_wstring(string(identifier));
	if (const Config* config = catalog.FindConfig(wide)) return config;
	for (const auto& [index, config] : catalog.configs) {
		(void)index;
		if (config.name == identifier) return &config;
	}
	size_t index = 0;
	if (TryParseIndex(identifier, index)) {
		const auto found = catalog.configs.find(static_cast<int>(index));
		if (found != catalog.configs.end()) return &found->second;
	}
	return nullptr;
}

optional<ResolvedTarget> ResolveTarget(
	const ProfileRuntime& runtime,
	const KnotLinkCommandRequest& request,
	string& error) {
	const auto currentSave = ParseBoolean(request.Get("current_save", "false"));
	if (!currentSave.has_value()) {
		error = "current_save must be true or false.";
		return nullopt;
	}
	const auto catalog = runtime.CatalogSnapshot();
	if (*currentSave) {
		vector<ResolvedTarget> occupied;
		for (const auto& [index, config] : catalog.configs) {
			(void)index;
			for (const auto& [world, description] : config.worlds) {
				(void)description;
				const filesystem::path path = filesystem::path(config.saveRoot) / world;
				if (IsRuntimeWorldOccupied(path)) {
					occupied.push_back({config, world, path});
				}
			}
		}
		if (occupied.empty()) {
			error = "No active world was found.";
			return nullopt;
		}
		if (occupied.size() != 1) {
			error = "Multiple active worlds were found; specify config_id and folder.";
			return nullopt;
		}
		return occupied.front();
	}
	const Config* config = ResolveConfig(catalog, request.Get("config_id"));
	if (!config) {
		error = "Unknown or missing config_id.";
		return nullopt;
	}
	const string selector = request.Get("folder");
	if (selector.empty()) {
		error = "Missing folder.";
		return nullopt;
	}
	size_t index = 0;
	if (TryParseIndex(selector, index) && index < config->worlds.size()) {
		const auto& world = config->worlds[index].first;
		return ResolvedTarget{*config, world,
			filesystem::path(config->saveRoot) / world};
	}
	const wstring wide = utf8_to_wstring(selector);
	for (const auto& [world, description] : config->worlds) {
		(void)description;
		const filesystem::path full = filesystem::path(config->saveRoot) / world;
		if (world == wide
			|| full.lexically_normal() == filesystem::path(wide).lexically_normal()) {
			return ResolvedTarget{*config, world, full};
		}
	}
	error = "Unknown folder.";
	return nullopt;
}

optional<BackupRequest> ResolveBackupRequest(
	const ProfileRuntime& runtime,
	const ResolvedTarget& target,
	const KnotLinkCommandRequest& command,
	string& error) {
	auto request = runtime.ResolveBackup(
		target.config.configId, target.world,
		utf8_to_wstring(command.Get("comment")));
	if (!request) {
		error = "The selected world is not configured.";
		return nullopt;
	}
	if (command.Has("backup_mode") && !command.Get("backup_mode").empty()) {
		const string mode = LowerAscii(command.Get("backup_mode"));
		if (mode == "full") request->config.backupMode = 1;
		else if (mode == "incremental" || mode == "smart") request->config.backupMode = 2;
		else {
			error = "backup_mode must be full or incremental.";
			return nullopt;
		}
	}
	if (command.Has("compression_method")
		&& !command.Get("compression_method").empty()) {
		const string method = LowerAscii(command.Get("compression_method"));
		if (method == "lzma2") request->config.zipMethod = L"LZMA2";
		else if (method == "deflate") request->config.zipMethod = L"Deflate";
		else if (method == "bzip2") request->config.zipMethod = L"BZip2";
		else if (method == "zstd") request->config.zipMethod = L"zstd";
		else {
			error = "Unsupported compression_method.";
			return nullopt;
		}
	}
	if (command.Has("compression_level")
		&& !command.Get("compression_level").empty()) {
		int level = -1;
		const string value = command.Get("compression_level");
		const auto parsed = from_chars(value.data(), value.data() + value.size(), level);
		const string method = LowerAscii(wstring_to_utf8(request->config.zipMethod));
		const int minimum = method == "zstd" || method == "bzip2" ? 1 : 0;
		const int maximum = method == "zstd" ? 22 : 9;
		if (parsed.ec != errc{} || parsed.ptr != value.data() + value.size()
			|| level < minimum || level > maximum) {
			error = "compression_level is outside the supported range.";
			return nullopt;
		}
		request->config.zipLevel = level;
	}
	if (command.Has("backup_blacklist")) {
		for (const auto& item : KnotLinkKeyValueCodec::DecodeList(
				command.GetEncoded("backup_blacklist"))) {
			const wstring rule = utf8_to_wstring(item);
			if (find(request->config.blacklist.begin(),
					request->config.blacklist.end(), rule)
				== request->config.blacklist.end()) {
				request->config.blacklist.push_back(rule);
			}
		}
	}
	return request;
}

filesystem::path LocalArchive(
	const ResolvedTarget& target,
	const wstring& backupFile) {
	FolderRewindFormat::StoragePaths paths;
	if (!FolderRewindFormat::TryResolveStoragePaths(
			target.config.backupPath, target.world,
			target.fullPath.wstring(), paths)) return {};
	return paths.backupSubDir / backupFile;
}

optional<wstring> LatestLocalBackup(
	const ProfileRuntime& runtime,
	const ResolvedTarget& target) {
	const auto entries = runtime.HistorySnapshot(target.config.configId);
	for (auto current = entries.rbegin(); current != entries.rend(); ++current) {
		if (current->worldName != target.world
			&& filesystem::path(current->worldPath).lexically_normal()
				!= target.fullPath.lexically_normal()) continue;
		error_code error;
		if (filesystem::is_regular_file(
				LocalArchive(target, current->backupFile), error) && !error) {
			return current->backupFile;
		}
	}
	return nullopt;
}

BackupRuntimeEvent Event(
	string eventId,
	const KnotLinkCommandContext& context,
	vector<pair<string, string>> fields = {}) {
	if (!context.metadata.from.empty()) {
		fields.emplace_back("from", context.metadata.from);
	}
	if (!context.metadata.requestId.empty()) {
		fields.emplace_back("request_id", context.metadata.requestId);
	}
	return {std::move(eventId), std::move(fields)};
}

} // namespace

struct ProfileKnotLinkCommands::Implementation {
	struct Operation {
		stop_source cancellation;
		atomic<bool> completed{false};
		jthread worker;
	};

	ProfileRuntime& runtime;
	mutex mutex;
	weak_ptr<HeadlessKnotLinkBridge> bridge;
	map<string, shared_ptr<Operation>> operations;
	bool stopping = false;

	Implementation(ProfileRuntime& runtimeValue, shared_ptr<HeadlessKnotLinkBridge> bridgeValue)
		: runtime(runtimeValue), bridge(std::move(bridgeValue)) {
	}

	~Implementation() {
		vector<shared_ptr<Operation>> pending;
		{
			lock_guard lock(mutex);
			stopping = true;
			for (auto& [id, operation] : operations) {
				(void)id;
				operation->cancellation.request_stop();
				pending.push_back(operation);
			}
			operations.clear();
		}
		for (const auto& operation : pending) {
			if (operation->worker.joinable()) operation->worker.join();
		}
	}

	void RequestStop() {
		lock_guard lock(mutex);
		stopping = true;
		for (auto& [id, operation] : operations) {
			(void)id;
			operation->cancellation.request_stop();
		}
	}

	void Cleanup() {
		vector<shared_ptr<Operation>> completed;
		{
			lock_guard lock(mutex);
			for (auto current = operations.begin(); current != operations.end();) {
				if (!current->second->completed.load(memory_order_acquire)) {
					++current;
					continue;
				}
				completed.push_back(current->second);
				current = operations.erase(current);
			}
		}
		for (const auto& operation : completed) {
			if (operation->worker.joinable()) operation->worker.join();
		}
	}

	void Publish(const BackupRuntimeEvent& event) {
		shared_ptr<HeadlessKnotLinkBridge> current;
		{
			lock_guard lock(mutex);
			current = bridge.lock();
		}
		if (current) current->Publish(event);
	}

	bool Submit(
		const shared_ptr<KnotLinkCommandContext>& context,
		function<pair<bool, string>(stop_token)> work) {
		Cleanup();
		const string id = context->metadata.requestId.empty()
			? wstring_to_utf8(FolderRewindFormat::GenerateGuidString())
			: context->metadata.requestId;
		auto operation = make_shared<Operation>();
		{
			lock_guard lock(mutex);
			if (stopping || operations.contains(id)) return false;
			operations.emplace(id, operation);
		}
		Publish(Event("command_accepted", *context,
			{{"command", context->request.command}}));
		operation->worker = jthread([this, operation, context, work = std::move(work)](stop_token) {
			Publish(Event("command_started", *context,
				{{"command", context->request.command}}));
			pair<bool, string> outcome;
			try {
				outcome = work(operation->cancellation.get_token());
			}
			catch (const exception& error) {
				outcome = {false, error.what()};
			}
			catch (...) {
				outcome = {false, "Unknown task failure."};
			}
			Publish(Event(outcome.first ? "command_completed" : "command_failed",
				*context, {{"command", context->request.command},
					{"message", outcome.second}}));
			operation->completed.store(true, memory_order_release);
		});
		return true;
	}

	string Handle(const shared_ptr<KnotLinkCommandContext>& context) {
		Cleanup();
		const auto& request = context->request;
		auto error = [&](string_view message,
			KnotLinkProtocolFormatter::Fields fields = {}) {
			fields.emplace_back("command", request.command);
			return KnotLinkProtocolFormatter::FormatError(
				context.get(), message, fields);
		};
		auto ok = [&](KnotLinkProtocolFormatter::Fields fields = {}) {
			return KnotLinkProtocolFormatter::FormatOk(*context, fields);
		};

		if (request.command == "PING") {
			return ok({{"message", "pong"}, {"version", MINEBACKUP_VERSION_STRING}});
		}
		if (request.command == "GET_CAPABILITIES") {
			return ok({
				{"content_type", "application/json"},
				{"encoding", "percent"},
				{"manifest_version", string(KnotLinkCapabilities::ManifestVersion)},
				{"func_list", string(KnotLinkCapabilities::ManifestJson())}});
		}
		if (request.command == "GET_STATUS") {
			const string active = to_string(ActiveOperationCount());
			const string data = "enabled=True;initialized=True;active_tasks=" + active;
			Publish(Event("status", *context,
				{{"enabled", "True"}, {"initialized", "True"}, {"active_tasks", active}}));
			return ok({{"data", data}});
		}
		if (request.command == "LIST_CONFIGS") {
			vector<string> configs;
			for (const auto& [index, config] : runtime.CatalogSnapshot().configs) {
				(void)index;
				configs.push_back(wstring_to_utf8(config.configId) + "," + config.name);
			}
			const string data = JoinDelimited(configs);
			Publish(Event("list_configs", *context, {{"data", data}}));
			return ok({{"data", data}});
		}
		if (request.command == "LIST_FOLDERS") {
			const auto catalog = runtime.CatalogSnapshot();
			const Config* config = ResolveConfig(catalog, request.Get("config_id"));
			if (!config) return error("Unknown or missing config_id.");
			vector<string> folders;
			for (const auto& [world, description] : config->worlds) {
				(void)description;
				folders.push_back(wstring_to_utf8(world));
			}
			const string data = JoinDelimited(folders);
			Publish(Event("list_folders", *context,
				{{"config", wstring_to_utf8(config->configId)}, {"data", data}}));
			return ok({{"data", data}});
		}
		if (request.command == "GET_CONFIG") {
			const auto catalog = runtime.CatalogSnapshot();
			const Config* config = ResolveConfig(catalog, request.Get("config_id"));
			if (!config) return error("Unknown or missing config_id.");
			const string mode = BackupModeName(config->backupMode);
			const string format = wstring_to_utf8(config->zipFormat);
			const string keep = to_string(config->keepCount);
			const string data = "name=" + config->name + ";backup_mode=" + mode
				+ ";format=" + format + ";keep_count=" + keep;
			Publish(Event("get_config", *context,
				{{"config", wstring_to_utf8(config->configId)}}));
			return ok({{"data", data}});
		}
		if (request.command == "LIST_BACKUPS") {
			string targetError;
			const auto target = ResolveTarget(runtime, request, targetError);
			if (!target) return error(targetError);
			vector<string> backups;
			for (const auto& entry : runtime.HistorySnapshot(target->config.configId)) {
				if (entry.worldName != target->world
					&& filesystem::path(entry.worldPath).lexically_normal()
						!= target->fullPath.lexically_normal()) continue;
				error_code fileError;
				if (filesystem::is_regular_file(
						LocalArchive(*target, entry.backupFile), fileError) && !fileError) {
					backups.push_back(wstring_to_utf8(entry.backupFile));
				}
			}
			const string data = JoinDelimited(backups);
			Publish(Event("list_backups", *context,
				{{"config", wstring_to_utf8(target->config.configId)},
					{"folder", wstring_to_utf8(target->world)}, {"data", data}}));
			return ok({{"data", data}});
		}

		if (request.command == "BACKUP") {
			string targetError;
			const auto target = ResolveTarget(runtime, request, targetError);
			if (!target) return error(targetError);
			const auto backup = ResolveBackupRequest(runtime, *target, request, targetError);
			if (!backup) return error(targetError);
			if (!Submit(context, [this, backup = *backup](stop_token token) {
				const auto result = runtime.RunBackupRequest(backup, token);
				return pair{IsSuccessful(result.code), ToString(result.code)};
			})) return error("The headless runtime is stopping.");
			return ok({{"message", "Command accepted."}});
		}
		if (request.command == "BACKUP_ALL") {
			const auto catalog = runtime.CatalogSnapshot();
			const Config* config = ResolveConfig(catalog, request.Get("config_id"));
			if (!config) return error("Unknown or missing config_id.");
			vector<BackupRequest> backups;
			for (const auto& [world, description] : config->worlds) {
				(void)description;
				const ResolvedTarget target{*config, world,
					filesystem::path(config->saveRoot) / world};
				string backupError;
				auto backup = ResolveBackupRequest(runtime, target, request, backupError);
				if (!backup) return error(backupError);
				backups.push_back(std::move(*backup));
			}
			if (!Submit(context, [this, backups = std::move(backups)](stop_token token) {
				size_t succeeded = 0;
				for (const auto& backup : backups) {
					if (token.stop_requested()) return pair{false, string("cancelled")};
					if (IsSuccessful(runtime.RunBackupRequest(backup, token).code)) ++succeeded;
				}
				return pair{succeeded == backups.size(),
					"succeeded=" + to_string(succeeded) + ", failed="
						+ to_string(backups.size() - succeeded)};
			})) return error("The headless runtime is stopping.");
			return ok({{"message", "Command accepted."}});
		}
		if (request.command == "RESTORE") {
			string targetError;
			const auto target = ResolveTarget(runtime, request, targetError);
			if (!target) return error(targetError);
			const bool currentSave = ParseBoolean(
				request.Get("current_save", "false")).value_or(false);
			wstring archive = utf8_to_wstring(request.Get("file"));
			if (archive.empty()) {
				const auto latest = LatestLocalBackup(runtime, *target);
				if (!latest) return error("No local history backup is available.");
				archive = *latest;
			}
			const string mode = LowerAscii(request.Get("mode", "clean"));
			if (mode != "clean" && mode != "overwrite") {
				return error("mode must be overwrite or clean.");
			}
			RestoreRequest restore;
			restore.config = target->config;
			restore.world = {target->config.configId, target->world};
			restore.archive = archive;
			restore.mode = mode == "clean" ? RestoreMode::Clean : RestoreMode::Overwrite;
			restore.restorePreserve = runtime.RestorePreserveSnapshot();
			if (request.Has("restore_whitelist")) {
				for (const auto& item : KnotLinkKeyValueCodec::DecodeList(
						request.GetEncoded("restore_whitelist"))) {
					const wstring rule = utf8_to_wstring(item);
					if (find(restore.restorePreserve.begin(),
							restore.restorePreserve.end(), rule)
						== restore.restorePreserve.end()) {
						restore.restorePreserve.push_back(rule);
					}
				}
			}
			if (currentSave) {
				HotRestoreRequest hotRequest;
				hotRequest.configId = target->config.configId;
				hotRequest.worldPath = target->world;
				hotRequest.fullWorldPath = target->fullPath;
				hotRequest.requestId = context->metadata.requestId;
				if (!Submit(context, [this,
						hotRequest = std::move(hotRequest),
						restore = std::move(restore)](stop_token token) {
					const auto result = runtime.RunHotRestore(
						hotRequest, restore, token);
					return pair{IsSuccessful(result.code), ToString(result.code)};
				})) return error("The headless runtime is stopping.");
				return ok({{"message", "Command accepted."}});
			}
			if (!Submit(context, [this, restore = std::move(restore)](stop_token token) {
				const auto result = runtime.Restore(restore, false, token);
				return pair{IsSuccessful(result.code), ToString(result.code)};
			})) return error("The headless runtime is stopping.");
			return ok({{"message", "Command accepted."}});
		}
		if (request.command == "MARK_IMPORTANT") {
			string targetError;
			const auto target = ResolveTarget(runtime, request, targetError);
			if (!target) return error(targetError);
			const wstring file = utf8_to_wstring(request.Get("file"));
			const auto important = ParseBoolean(request.Get("important"));
			if (file.empty() || !important.has_value()) {
				return error("file and important=true|false are required.");
			}
			if (!runtime.SetBackupImportant(
					target->config.configId, target->world, file, *important)) {
				return error("Backup history entry was not found.");
			}
			Publish(Event("mark_important", *context,
				{{"config", wstring_to_utf8(target->config.configId)},
					{"folder", wstring_to_utf8(target->world)},
					{"file", wstring_to_utf8(file)},
					{"important", *important ? "true" : "false"}}));
			return ok({{"message", "Importance flag updated."}});
		}
		return error("Unknown command.", {{"code", "unknown_command"}});
	}

	size_t ActiveOperationCount() {
		Cleanup();
		lock_guard lock(mutex);
		return operations.size();
	}
};

ProfileKnotLinkCommands::ProfileKnotLinkCommands(
	ProfileRuntime& runtime,
	shared_ptr<HeadlessKnotLinkBridge> bridge)
	: implementation_(make_unique<Implementation>(runtime, std::move(bridge))) {
}

ProfileKnotLinkCommands::~ProfileKnotLinkCommands() = default;

void ProfileKnotLinkCommands::SetBridge(shared_ptr<HeadlessKnotLinkBridge> bridge) {
	lock_guard lock(implementation_->mutex);
	implementation_->bridge = std::move(bridge);
}

void ProfileKnotLinkCommands::Stop() {
	implementation_->RequestStop();
}

string ProfileKnotLinkCommands::Handle(
	const shared_ptr<KnotLinkCommandContext>& context) {
	return implementation_->Handle(context);
}

size_t ProfileKnotLinkCommands::ActiveOperationCount() {
	return implementation_->ActiveOperationCount();
}
