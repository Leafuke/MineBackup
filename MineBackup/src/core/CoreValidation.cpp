#include "CoreValidation.h"
#include "TaskCoordinator.h"

#include "ArchiveRunner.h"
#include "BackupManager.h"
#include "ConfigManager.h"
#include "Logging.h"
#include "FolderRewindFormat.h"
#include "FolderRewindMetadataStore.h"
#include "Globals.h"
#include "HistoryManager.h"
#include "MigrationCoordinator.h"
#include "AppPaths.h"
#include "i18n.h"
#include "json.hpp"
#include "PlatformCompat.h"
#include "text_to_text.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <stop_token>
#include <string_view>
#include <thread>
#include <tuple>

using namespace std;

#define VALIDATION_INFO(...) MB_LOG_PRINTF_INFO(minebackup::logging::LogCategory::Validation, "validation.progress", __VA_ARGS__)
#define VALIDATION_ERROR(...) MB_LOG_PRINTF_ERROR(minebackup::logging::LogCategory::Validation, "validation.error", __VA_ARGS__)

namespace {
	constexpr int kValidationConfigIndex = -424242;
	constexpr const wchar_t* kValidationConfigId = L"00000000-0000-0000-0000-000000424242";
	constexpr const wchar_t* kSmartWorldName = L"__CoreValidationSmart";
	constexpr const wchar_t* kLimitWorldName = L"__CoreValidationLimit";

	using WorldState = map<wstring, string>;

	static string MsgFmt(const char* key, const string& arg) {
		char buffer[4096] = {};
		std::snprintf(buffer, sizeof(buffer), L(key), arg.c_str());
		return string(buffer);
	}

	static string MsgFmt(const char* key, int value) {
		char buffer[1024] = {};
		std::snprintf(buffer, sizeof(buffer), L(key), value);
		return string(buffer);
	}

	static bool AreHistoryEntriesEqual(
		const HistoryEntry& left,
		const HistoryEntry& right) {
		return tie(
			left.configId,
			left.timestamp_str,
			left.worldPath,
			left.worldName,
			left.backupFile,
			left.backupType,
			left.isPartialBackup,
			left.comment,
			left.isImportant,
			left.isCloudArchived,
			left.cloudArchivedAtUtc,
			left.cloudArchiveRemotePath,
			left.cloudMetadataRecordRemotePath,
			left.cloudMetadataStateRemotePath)
			== tie(
				right.configId,
				right.timestamp_str,
				right.worldPath,
				right.worldName,
				right.backupFile,
				right.backupType,
				right.isPartialBackup,
				right.comment,
				right.isImportant,
				right.isCloudArchived,
				right.cloudArchivedAtUtc,
				right.cloudArchiveRemotePath,
				right.cloudMetadataRecordRemotePath,
				right.cloudMetadataStateRemotePath);
	}

	static vector<wstring> GetNormalizedPathComponents(const wstring& rawPath) {
		wstring normalized = rawPath;
		replace(normalized.begin(), normalized.end(), L'\\', L'/');
		const filesystem::path path = filesystem::path(normalized).lexically_normal();
		vector<wstring> components;
		for (const auto& component : path) {
			const wstring value = component.wstring();
			if (value.empty() || value == L"/" || value == L"\\") continue;
			components.push_back(value);
		}
		return components;
	}

	static bool HasValidationSandboxWorldPath(
		const wstring& worldPath,
		const wstring& expectedWorldName) {
		const vector<wstring> components = GetNormalizedPathComponents(worldPath);
		for (size_t index = 0; index + 3 < components.size(); ++index) {
			if (components[index] != L"MineBackup_CoreValidation"
				|| components[index + 1].empty()
				|| components[index + 2] != L"worlds"
				|| components[index + 3] != expectedWorldName) {
				continue;
			}
			if (index + 4 == components.size()) return true;
		}
		return false;
	}

	static bool IsKnownCoreValidationComment(const wstring& comment) {
		static constexpr array<const wchar_t*, 8> knownComments = {
			L"CoreValidation_Base",
			L"CoreValidation_NoChange",
			L"CoreValidation_Smart_Locked",
			L"CoreValidation_Smart_Delete",
			L"CoreValidation_DeleteOnly",
			L"CoreValidation_Limit_1",
			L"CoreValidation_Limit_2",
			L"CoreValidation_Limit_3"
		};
		return any_of(knownComments.begin(), knownComments.end(), [&](const auto value) {
			return comment == value;
		});
	}

	static bool IsLegacyCoreValidationPollutionInternal(const HistoryEntry& entry) {
		const bool isKnownWorld = entry.worldName == kSmartWorldName
			|| entry.worldName == kLimitWorldName;
		if (!isKnownWorld
			|| !HasValidationSandboxWorldPath(entry.worldPath, entry.worldName)
			|| !IsKnownCoreValidationComment(entry.comment)) {
			return false;
		}

		const bool supportedBackupType = entry.backupType == L"Full"
			|| entry.backupType == L"Smart"
			|| entry.backupType == L"Overwrite";
		return supportedBackupType
			&& entry.backupFile.find(entry.worldName) != wstring::npos
			&& entry.backupFile.find(entry.comment) != wstring::npos;
	}

	static bool AreHistoryVectorsEqual(
		const vector<HistoryEntry>& left,
		const vector<HistoryEntry>& right) {
		return left.size() == right.size()
			&& equal(left.begin(), left.end(), right.begin(), AreHistoryEntriesEqual);
	}

	static bool AreCoreValidationHistorySnapshotsEqualInternal(
		const CoreValidationHistorySnapshot& before,
		const CoreValidationHistorySnapshot& after,
		size_t* changedConfigCount) {
		size_t changed = 0;
		auto beforeIt = before.begin();
		auto afterIt = after.begin();
		while (beforeIt != before.end() || afterIt != after.end()) {
			if (beforeIt == before.end()) {
				++changed;
				++afterIt;
				continue;
			}
			if (afterIt == after.end()) {
				++changed;
				++beforeIt;
				continue;
			}
			if (beforeIt->first != afterIt->first) {
				if (beforeIt->first < afterIt->first) ++beforeIt;
				else ++afterIt;
				++changed;
				continue;
			}
			if (!AreHistoryVectorsEqual(beforeIt->second, afterIt->second)) ++changed;
			++beforeIt;
			++afterIt;
		}
		if (changedConfigCount) *changedConfigCount = changed;
		return changed == 0;
	}

	static CoreValidationHistorySnapshot CaptureRealHistorySnapshot() {
		CoreValidationHistorySnapshot result;
		const auto snapshot = GetHistorySnapshot();
		for (const auto& [configId, entries] : snapshot->byConfigId) {
			if (configId == kValidationConfigId) continue;
			result.emplace(configId, *entries);
		}
		return result;
	}

	static wstring ToGenericRelative(const filesystem::path& path, const filesystem::path& root) {
		error_code ec;
		filesystem::path relative = filesystem::relative(path, root, ec);
		wstring result = ec ? path.filename().wstring() : relative.wstring();
		for (wchar_t& ch : result) {
			if (ch == L'\\') ch = L'/';
		}
		return result;
	}

	static bool ShouldIgnoreValidationFile(const filesystem::path& relativePath) {
		wstring lower = relativePath.filename().wstring();
		transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
		if (lower == L"session.lock" || lower == L"lock") return true;
		return lower.size() >= 5 && lower.substr(lower.size() - 5) == L".lock";
	}

	static void WriteTextFile(const filesystem::path& filePath, const string& content) {
		filesystem::create_directories(filePath.parent_path());
		ofstream out(filePath, ios::binary | ios::trunc);
		out.write(content.data(), static_cast<streamsize>(content.size()));
	}

	static string ReadTextFile(const filesystem::path& filePath) {
		ifstream in(filePath, ios::binary);
		return string((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
	}

	static void RemoveIfExists(const filesystem::path& filePath) {
		error_code ec;
		filesystem::remove(filePath, ec);
	}

	static WorldState CaptureWorldState(const filesystem::path& worldPath) {
		WorldState state;
		if (!filesystem::exists(worldPath)) {
			return state;
		}

		for (const auto& entry : filesystem::recursive_directory_iterator(worldPath)) {
			if (!entry.is_regular_file()) continue;
			filesystem::path relativePath = filesystem::relative(entry.path(), worldPath);
			if (ShouldIgnoreValidationFile(relativePath)) continue;
			state[ToGenericRelative(entry.path(), worldPath)] = ReadTextFile(entry.path());
		}
		return state;
	}

	static bool CompareWorldState(const WorldState& expected, const WorldState& actual, string& diff) {
		for (const auto& pair : expected) {
			auto it = actual.find(pair.first);
			if (it == actual.end()) {
				diff = MsgFmt("VAL_DIFF_MISSING", wstring_to_utf8(pair.first));
				return false;
			}
			if (it->second != pair.second) {
				diff = MsgFmt("VAL_DIFF_CONTENT_MISMATCH", wstring_to_utf8(pair.first));
				return false;
			}
		}

		for (const auto& pair : actual) {
			if (!expected.count(pair.first)) {
				diff = MsgFmt("VAL_DIFF_UNEXPECTED", wstring_to_utf8(pair.first));
				return false;
			}
		}

		return true;
	}

	static void ClearValidationArtifactsForWorld(const Config& cfg, const wstring& worldName) {
		error_code ec;
		filesystem::path backupRoot(cfg.backupPath);
		filesystem::remove_all(backupRoot / worldName, ec);
		ec.clear();
		filesystem::remove_all(backupRoot / L"_metadata" / worldName, ec);

		(void)ClearHistoryEntriesForWorld(kValidationConfigIndex, worldName);
	}

	static string MakeDeterministicPayload(
		string_view label,
		size_t byteCount,
		uint32_t seed) {
		string payload;
		payload.reserve(byteCount);
		for (const char value : label) {
			if (payload.size() == byteCount) break;
			payload.push_back(value);
		}
		if (payload.size() < byteCount) payload.push_back('\n');

		uint32_t state = seed == 0 ? 0x6d2b79f5u : seed;
		while (payload.size() < byteCount) {
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			payload.push_back(static_cast<char>((state >> 24) & 0xffu));
		}
		return payload;
	}

	static vector<HistoryEntry> GetHistoryEntriesForWorld(int configIndex, const wstring& worldName) {
		return ::GetHistoryEntriesForWorld(configIndex, worldName);
	}

	static vector<filesystem::path> GetBackupFilesForWorld(const Config& config, const wstring& worldName) {
		vector<filesystem::path> archives;
		filesystem::path backupDir = filesystem::path(config.backupPath) / worldName;
		if (!filesystem::exists(backupDir)) return archives;
		for (const auto& entry : filesystem::directory_iterator(backupDir)) {
			if (entry.is_regular_file()) {
				archives.push_back(entry.path());
			}
		}
		sort(archives.begin(), archives.end(), [](const filesystem::path& a, const filesystem::path& b) {
			error_code ecA, ecB;
			auto timeA = filesystem::last_write_time(a, ecA);
			auto timeB = filesystem::last_write_time(b, ecB);
			if (!ecA && !ecB && timeA != timeB) return timeA < timeB;
			return a.filename().wstring() < b.filename().wstring();
		});
		return archives;
	}

	static Config BuildValidationConfig(const Config& templateConfig, const filesystem::path& saveRoot, const filesystem::path& backupRoot, int backupMode, int keepCount, bool skipIfUnchanged) {
		Config cfg;
		cfg.name = "CoreValidation";
		cfg.saveRoot = saveRoot.wstring();
		cfg.backupPath = backupRoot.wstring();
		cfg.zipPath = templateConfig.zipPath;
		cfg.zipFormat = templateConfig.zipFormat.empty() ? L"7z" : templateConfig.zipFormat;
		cfg.zipMethod = templateConfig.zipMethod.empty() ? L"LZMA2" : templateConfig.zipMethod;
		cfg.zipLevel = templateConfig.zipLevel > 0 ? templateConfig.zipLevel : 5;
		cfg.cpuThreads = templateConfig.cpuThreads;
		cfg.useLowPriority = false;
		cfg.skipIfUnchanged = skipIfUnchanged;
		cfg.maxSmartBackupsPerFull = max(3, templateConfig.maxSmartBackupsPerFull);
		cfg.backupMode = backupMode;
		cfg.keepCount = keepCount;
		cfg.backupBefore = false;
		cfg.blacklist.clear();
		cfg.worlds = {
			{ kSmartWorldName, L"CoreValidation" },
			{ kLimitWorldName, L"CoreValidation" }
		};
		cfg.configId = kValidationConfigId;
		return cfg;
	}

	static bool TryResolveValidationTemplate(Config& outConfig, string& error) {
		lock_guard<mutex> lock(g_appState.configsMutex);
		auto isUsable = [&](const Config& cfg) {
			return !cfg.zipPath.empty() && filesystem::exists(cfg.zipPath);
		};

		auto currentIt = g_appState.configs.find(g_appState.currentConfigIndex);
		if (currentIt != g_appState.configs.end() && isUsable(currentIt->second)) {
			outConfig = currentIt->second;
			return true;
		}

		for (const auto& pair : g_appState.configs) {
			if (isUsable(pair.second)) {
				outConfig = pair.second;
				return true;
			}
		}

		error = L("VAL_ERR_NO_7Z_CONFIG");
		return false;
	}

	static bool SleepForUniqueBackupName(stop_token stopToken = {}) {
		constexpr auto kTotalDuration = chrono::milliseconds(1100);
		constexpr auto kSliceDuration = chrono::milliseconds(50);
		auto elapsed = chrono::milliseconds(0);
		while (elapsed < kTotalDuration) {
			if (stopToken.stop_requested()) {
				return false;
			}
			const auto step = (std::min)(kSliceDuration, kTotalDuration - elapsed);
			this_thread::sleep_for(step);
			elapsed += step;
		}
		return !stopToken.stop_requested();
	}

	class SharedWriteHandle {
	public:
		~SharedWriteHandle() {
			Close();
		}

		bool OpenAndRewrite(const filesystem::path& filePath, const string& content, string& error) {
			Close();
			filesystem::create_directories(filePath.parent_path());
#ifdef _WIN32
			handle_ = CreateFileW(
				filePath.wstring().c_str(),
				GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr,
				OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);
			if (handle_ == INVALID_HANDLE_VALUE) {
				error = L("VAL_ERR_SHARED_OPEN");
				return false;
			}

			LARGE_INTEGER start{};
			if (!SetFilePointerEx(handle_, start, nullptr, FILE_BEGIN) || !SetEndOfFile(handle_)) {
				error = L("VAL_ERR_SHARED_TRUNCATE");
				Close();
				return false;
			}

			DWORD written = 0;
			if (!WriteFile(handle_, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) || written != content.size()) {
				error = L("VAL_ERR_SHARED_WRITE");
				Close();
				return false;
			}

			FlushFileBuffers(handle_);
			return true;
#else
			stream_.open(filePath, ios::binary | ios::in | ios::out | ios::trunc);
			if (!stream_.is_open()) {
				error = L("VAL_ERR_SHARED_OPEN_STREAM");
				return false;
			}
			stream_.write(content.data(), static_cast<streamsize>(content.size()));
			stream_.flush();
			return true;
#endif
		}

		void Close() {
#ifdef _WIN32
			if (handle_ != INVALID_HANDLE_VALUE) {
				CloseHandle(handle_);
				handle_ = INVALID_HANDLE_VALUE;
			}
#else
			if (stream_.is_open()) {
				stream_.close();
			}
#endif
		}

	private:
#ifdef _WIN32
		HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
		fstream stream_;
#endif
	};

	struct ValidationContext {
		bool automatic = false;
		stop_token stopToken;
		vector<string> failures;

		bool StopRequested() const noexcept {
			return stopToken.stop_requested();
		}

		void Info(const string& message) {
			VALIDATION_INFO("%s", message.c_str());
		}

		bool Require(bool condition, const string& successMessage, const string& failureMessage) {
			if (condition) {
				if (!successMessage.empty()) {
					Info(successMessage);
				}
				return true;
			}
			failures.push_back(failureMessage);
			VALIDATION_ERROR("%s", failureMessage.c_str());
			return false;
		}
	};

	struct ValidationCleanupGuard {
		filesystem::path sandboxRoot;
		bool previousSafeDelete = true;
		bool hadHistorySnapshot = false;
		vector<HistoryEntry> historySnapshot;
		bool hadConfigSnapshot = false;
		Config configSnapshot;
		bool finalized = false;

		bool Finalize() noexcept {
			bool success = true;
			isSafeDelete = previousSafeDelete;
			try {
				lock_guard<mutex> lock(g_appState.configsMutex);
				if (hadConfigSnapshot) {
					g_appState.configs[kValidationConfigIndex] = configSnapshot;
				}
			}
			catch (const exception& ex) {
				VALIDATION_ERROR("Validation cleanup could not restore the synthetic config: %s", ex.what());
				success = false;
			}
			catch (...) {
				VALIDATION_ERROR("Validation cleanup could not restore the synthetic config.");
				success = false;
			}

			bool syntheticConfigExists = false;
			try {
				lock_guard<mutex> lock(g_appState.configsMutex);
				syntheticConfigExists = g_appState.configs.contains(kValidationConfigIndex);
			}
			catch (const exception& ex) {
				VALIDATION_ERROR("Validation cleanup could not inspect the synthetic config: %s", ex.what());
				success = false;
			}
			catch (...) {
				VALIDATION_ERROR("Validation cleanup could not inspect the synthetic config.");
				success = false;
			}

			try {
				if (hadConfigSnapshot || hadHistorySnapshot || syntheticConfigExists) {
					if (!ReplaceHistoryEntriesForConfig(
						kValidationConfigIndex,
						hadHistorySnapshot ? historySnapshot : vector<HistoryEntry>{})) {
						VALIDATION_ERROR("Validation cleanup could not restore synthetic history.");
						success = false;
					}
				}
			}
			catch (const exception& ex) {
				VALIDATION_ERROR("Validation cleanup could not restore synthetic history: %s", ex.what());
				success = false;
			}
			catch (...) {
				VALIDATION_ERROR("Validation cleanup could not restore synthetic history.");
				success = false;
			}
			if (!hadConfigSnapshot) {
				try {
					lock_guard<mutex> lock(g_appState.configsMutex);
					g_appState.configs.erase(kValidationConfigIndex);
				}
				catch (const exception& ex) {
					VALIDATION_ERROR("Validation cleanup could not remove the synthetic config: %s", ex.what());
					success = false;
				}
				catch (...) {
					VALIDATION_ERROR("Validation cleanup could not remove the synthetic config.");
					success = false;
				}
			}
			try {
				error_code ec;
				filesystem::remove_all(sandboxRoot, ec);
				if (ec) {
					VALIDATION_ERROR("Validation sandbox cleanup failed: %s", ec.message().c_str());
					success = false;
				}
			}
			catch (const exception& ex) {
				VALIDATION_ERROR("Validation sandbox cleanup failed: %s", ex.what());
				success = false;
			}
			catch (...) {
				VALIDATION_ERROR("Validation sandbox cleanup failed.");
				success = false;
			}
			if (success) finalized = true;
			return success;
		}

		~ValidationCleanupGuard() noexcept {
			if (!finalized) (void)Finalize();
		}
	};

	struct LegacyValidationCleanupResult {
		bool success = true;
		size_t removedEntries = 0;
		size_t affectedConfigs = 0;
	};

	static LegacyValidationCleanupResult CleanupLegacyCoreValidationHistoryPollution() {
		LegacyValidationCleanupResult result;
		vector<int> configIndices;
		{
			lock_guard<mutex> lock(g_appState.configsMutex);
			for (const auto& [configIndex, config] : g_appState.configs) {
				if (!config.configId.empty()) configIndices.push_back(configIndex);
			}
		}

		for (const int configIndex : configIndices) {
			size_t removed = 0;
			if (!RemoveHistoryEntriesIf(
				configIndex,
				IsLegacyCoreValidationPollution,
				&removed)) {
				result.success = false;
				continue;
			}
			if (removed != 0) {
				result.removedEntries += removed;
				++result.affectedConfigs;
			}
		}
		return result;
	}

	struct TemporarySafeDeleteMode {
		bool previousValue = true;

		explicit TemporarySafeDeleteMode(bool enabled) {
			previousValue = isSafeDelete;
			isSafeDelete = enabled;
		}

		~TemporarySafeDeleteMode() {
			isSafeDelete = previousValue;
		}
	};

	static bool AssertLockArtifactsAbsent(ValidationContext& ctx, const filesystem::path& worldPath) {
		const vector<filesystem::path> disallowed = {
			// worldPath / L"session.lock", // session.lock 在默认还原白名单中。
			worldPath / L"LOCK",
			worldPath / L"locks" / L"runtime.lock"
		};
		for (const auto& path : disallowed) {
			if (filesystem::exists(path)) {
				const string message = MsgFmt("VAL_ERR_LOCK_ARTIFACT_LEFT", wstring_to_utf8(path.wstring()));
				ctx.failures.push_back(message);
				VALIDATION_ERROR("%s", message.c_str());
				return false;
			}
		}
		ctx.Info(L("VAL_INFO_LOCK_FILES_EXCLUDED"));
		return true;
	}

	static bool AssertFolderRewindMetadata(ValidationContext& ctx, const Config& cfg, const wstring& worldName, const wstring& backupFile, const wstring& expectedBackupType) {
		FolderRewindFormat::StoragePaths paths;
		if (!ctx.Require(FolderRewindFormat::TryResolveStoragePaths(cfg.backupPath, worldName, L"", paths), "[Validation] FolderRewind storage path resolved.", "[Validation] Failed to resolve FolderRewind storage paths.")) return false;
		if (!ctx.Require(filesystem::exists(paths.statePath), "[Validation] state.json exists.", "[Validation] state.json missing.")) return false;
		if (!ctx.Require(filesystem::exists(paths.recordsDir), "[Validation] records directory exists.", "[Validation] records directory missing.")) return false;

		auto recordPath = FolderRewindMetadataStore::TryGetRecordPath(paths.metadataDir, backupFile);
		if (!ctx.Require(recordPath.has_value(), "[Validation] record json path resolved.", "[Validation] record json path could not be resolved.")) return false;
		if (!ctx.Require(filesystem::exists(*recordPath), "[Validation] record json exists.", "[Validation] record json missing.")) return false;
		if (!ctx.Require(!filesystem::exists(paths.metadataDir / L"metadata.json"), "[Validation] legacy metadata.json absent.", "[Validation] legacy metadata.json was written.")) return false;

		ifstream stateIn(paths.statePath, ios::binary);
		nlohmann::json state = nlohmann::json::parse(stateIn, nullptr, false);
		if (!ctx.Require(!state.is_discarded() && state.is_object(), "[Validation] state.json parses.", "[Validation] state.json parse failed.")) return false;
		if (!ctx.Require(state.contains("Version") && state.contains("LastBackupTime") && state.contains("LastBackupFileName") && state.contains("BasedOnFullBackup") && state.contains("FileStates"), "[Validation] state.json has PascalCase fields.", "[Validation] state.json PascalCase fields missing.")) return false;
		if (!ctx.Require(state.value("Version", string{}) == "3.0", "[Validation] state Version is 3.0.", "[Validation] state Version is not 3.0.")) return false;
		if (!ctx.Require(state["FileStates"].is_object(), "[Validation] state FileStates is object.", "[Validation] state FileStates is not an object.")) return false;
		if (!ctx.Require(utf8_to_wstring(state.value("LastBackupFileName", string{})) == backupFile, "[Validation] state LastBackupFileName matches.", "[Validation] state LastBackupFileName mismatch.")) return false;

		ifstream recordIn(*recordPath, ios::binary);
		nlohmann::json record = nlohmann::json::parse(recordIn, nullptr, false);
		if (!ctx.Require(!record.is_discarded() && record.is_object(), "[Validation] record json parses.", "[Validation] record json parse failed.")) return false;
		if (!ctx.Require(record.contains("ArchiveFileName") && record.contains("BackupType") && record.contains("BasedOnFullBackup") && record.contains("PreviousBackupFileName") && record.contains("CreatedAtUtc") && record.contains("AddedFiles") && record.contains("ModifiedFiles") && record.contains("DeletedFiles") && record.contains("FullFileList"), "[Validation] record json has PascalCase fields.", "[Validation] record PascalCase fields missing.")) return false;
		if (!ctx.Require(utf8_to_wstring(record.value("ArchiveFileName", string{})) == backupFile, "[Validation] record ArchiveFileName matches.", "[Validation] record ArchiveFileName mismatch.")) return false;
		if (!ctx.Require(utf8_to_wstring(record.value("BackupType", string{})) == expectedBackupType, "[Validation] record BackupType matches.", "[Validation] record BackupType mismatch.")) return false;
		if (!ctx.Require(record["AddedFiles"].is_array() && record["ModifiedFiles"].is_array() && record["DeletedFiles"].is_array() && record["FullFileList"].is_array(), "[Validation] record change lists are arrays.", "[Validation] record change lists are not arrays.")) return false;
		return true;
	}

	static bool AssertFolderRewindHistoryItem(ValidationContext& ctx, const Config& cfg, const MyFolder& world, const wstring& backupFile, const wstring& expectedBackupType) {
		ifstream historyIn(GetAppPaths().HistoryFile(), ios::binary);
		nlohmann::json historyRoot = nlohmann::json::parse(historyIn, nullptr, false);
		if (!ctx.Require(!historyRoot.is_discarded() && historyRoot.is_array(), "[Validation] history.json parses as array.", "[Validation] history.json parse failed.")) return false;

		const bool hasFolderRewindHistoryItem = any_of(historyRoot.begin(), historyRoot.end(), [&](const nlohmann::json& item) {
			return item.is_object()
				&& item.value("ConfigId", string{}) == wstring_to_utf8(cfg.configId)
				&& item.value("FolderName", string{}) == wstring_to_utf8(world.name)
				&& item.value("FileName", string{}) == wstring_to_utf8(backupFile)
				&& item.value("BackupType", string{}) == wstring_to_utf8(expectedBackupType)
				&& item.contains("Timestamp")
				&& item.contains("IsCloudArchived");
		});
		if (!ctx.Require(hasFolderRewindHistoryItem, "[Validation] history.json contains FolderRewind HistoryItem.", "[Validation] FolderRewind HistoryItem missing.")) return false;
		return true;
	}

	static string DescribeBackupResult(const BackupResult& result) {
		string description = "outcome=";
		description += ToString(result.outcome);
		description += ", code=";
		description += ToString(result.code);
		for (const auto& diagnostic : result.diagnostics) {
			if (diagnostic.severity != DiagnosticSeverity::Error) continue;
			description += ", diagnostic=";
			description += diagnostic.eventId;
			if (!diagnostic.detail.empty()) {
				description += ": ";
				description += diagnostic.detail;
			}
			break;
		}
		return description;
	}

	static bool RequireExpectedBackup(
		ValidationContext& ctx,
		const BackupResult& result,
		BackupOutcome expected) {
		const char* successKey = expected == BackupOutcome::NoChanges
			? "VAL_OK_BACKUP_NO_CHANGE_OPERATION"
			: "VAL_OK_BACKUP_CREATED_OPERATION";
		return ctx.Require(
			result.outcome == expected,
			L(successKey),
			MsgFmt("VAL_ERR_BACKUP_OPERATION", DescribeBackupResult(result)));
	}

	static bool AssertArchiveIntegrity(
		ValidationContext& ctx,
		const Config& cfg,
		const filesystem::path& archivePath) {
		error_code ec;
		if (!ctx.Require(
			filesystem::is_regular_file(archivePath, ec) && !ec,
			L("VAL_OK_ARCHIVE_PRESENT"),
			L("VAL_ERR_ARCHIVE_MISSING"))) {
			return false;
		}
		const uintmax_t size = filesystem::file_size(archivePath, ec);
		if (!ctx.Require(
			!ec && size > 0,
			L("VAL_OK_ARCHIVE_NONEMPTY"),
			L("VAL_ERR_ARCHIVE_EMPTY"))) {
			return false;
		}

		const ArchiveRunner runner = ArchiveRunner::Resolve(
			cfg.zipPath,
			GetAppPaths(),
			TaskCoordinator::CurrentStopToken());
		if (!ctx.Require(
			runner.IsAvailable(),
			L("VAL_OK_ARCHIVE_TOOL_AVAILABLE"),
			MsgFmt("VAL_ERR_ARCHIVE_TOOL", wstring_to_utf8(runner.Resolution().diagnostic)))) {
			return false;
		}

		const ProcessResult result = runner.Execute(
			{L"t", archivePath.wstring(), L"-y"},
			archivePath.parent_path(),
			cfg.useLowPriority);
		string detail = "exit_code=" + to_string(result.exitCode);
		if (!result.error.empty()) {
			detail += ", error=" + wstring_to_utf8(result.error);
		}
		return ctx.Require(
			result.status == ProcessStatus::Succeeded,
			L("VAL_OK_ARCHIVE_INTEGRITY"),
			MsgFmt("VAL_ERR_ARCHIVE_INTEGRITY", detail));
	}

	static bool RunSmartBackupScenario(ValidationContext& ctx, const Config& templateConfig, const filesystem::path& sandboxRoot) {
		if (ctx.StopRequested()) return false;
		ctx.Info(L("VAL_INFO_SCENARIO_SMART"));

		const filesystem::path saveRoot = sandboxRoot / L"worlds";
		const filesystem::path backupRoot = sandboxRoot / L"backups";
		const filesystem::path worldPath = saveRoot / kSmartWorldName;
		filesystem::create_directories(worldPath / L"data");
		filesystem::create_directories(worldPath / L"region");
		filesystem::create_directories(worldPath / L"locks");

		Config cfg = BuildValidationConfig(templateConfig, saveRoot, backupRoot, 2, 0, true);
		{
			lock_guard<mutex> lock(g_appState.configsMutex);
			g_appState.configs[kValidationConfigIndex] = cfg;
		}
		MyFolder world{ worldPath.wstring(), kSmartWorldName, L"CoreValidation", cfg, kValidationConfigIndex, 0 };
		ClearValidationArtifactsForWorld(cfg, world.name);

		WorldState state1 = {
			{ L"notes.txt", MakeDeterministicPayload("base-notes", 128 * 1024, 0x12345678u) },
			{ L"data/base.txt", "base-file-v1\n" },
			{ L"region/0.0.mca", "region-v1\n" },
			{ L"to_delete.txt", "delete-me-later\n" }
		};
		WorldState state2 = state1;
		state2[L"notes.txt"] = MakeDeterministicPayload("smart-notes-1", 160 * 1024, 0x23456789u);
		state2[L"data/add.txt"] = "added-on-smart-backup\n";
		state2[L"region/0.0.mca"] = "region-v2-open-for-write\n";
		WorldState state3 = state2;
		state3[L"notes.txt"] = MakeDeterministicPayload("smart-notes-2", 192 * 1024, 0x3456789au);
		state3[L"data/base.txt"] = "base-file-v2\n";
		state3.erase(L"to_delete.txt");
		state3[L"data/fresh.txt"] = "fresh-file-before-delete-only\n";
		WorldState state4 = state3;
		state4.erase(L"data/fresh.txt");

		for (const auto& pair : state1) {
			WriteTextFile(worldPath / filesystem::path(pair.first), pair.second);
		}
		WriteTextFile(worldPath / L"session.lock", "ignored-session-lock\n");
		WriteTextFile(worldPath / L"LOCK", "ignored-upper-lock\n");
		WriteTextFile(worldPath / L"locks" / L"runtime.lock", "ignored-sub-lock\n");

		world.config.skipIfUnchanged = false;
		const BackupResult initialFull = RunDesktopBackup(
			world,
			L"CoreValidation_Base",
			TaskCoordinator::CurrentStopToken());
		if (!RequireExpectedBackup(ctx, initialFull, BackupOutcome::Created) || ctx.StopRequested()) return false;
		auto historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(historyEntries.size() == 1, L("VAL_OK_INITIAL_FULL_CREATED"), L("VAL_ERR_INITIAL_FULL_NOT_CREATED"))) return false;
		if (!ctx.Require(historyEntries[0].backupType == L"Full", L("VAL_OK_FIRST_TYPE_FULL"), L("VAL_ERR_FIRST_TYPE_NOT_FULL"))) return false;
		const wstring fullBackupFile = historyEntries[0].backupFile;
		if (!ctx.Require(GetBackupFilesForWorld(cfg, world.name).size() == 1, L("VAL_OK_ARCHIVE_COUNT_AFTER_FIRST"), L("VAL_ERR_ARCHIVE_COUNT_AFTER_FIRST"))) return false;
		if (!AssertArchiveIntegrity(ctx, cfg, filesystem::path(cfg.backupPath) / world.name / fullBackupFile)) return false;
		if (!AssertFolderRewindMetadata(ctx, cfg, world.name, fullBackupFile, L"Full")) return false;
		if (!AssertFolderRewindHistoryItem(ctx, cfg, world, fullBackupFile, L"Full")) return false;

		world.config.skipIfUnchanged = true;
		if (!SleepForUniqueBackupName(ctx.stopToken) || ctx.StopRequested()) return false;
		const BackupResult noChange = RunDesktopBackup(
			world,
			L"CoreValidation_NoChange",
			TaskCoordinator::CurrentStopToken());
		if (!RequireExpectedBackup(ctx, noChange, BackupOutcome::NoChanges) || ctx.StopRequested()) return false;
		historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(historyEntries.size() == 1, L("VAL_OK_SKIP_NO_CHANGE"), L("VAL_ERR_NO_CHANGE_CREATED"))) return false;

		world.config.skipIfUnchanged = false;
		if (!SleepForUniqueBackupName(ctx.stopToken) || ctx.StopRequested()) return false;
		WriteTextFile(worldPath / L"notes.txt", state2.at(L"notes.txt"));
		WriteTextFile(worldPath / L"data" / L"add.txt", state2.at(L"data/add.txt"));
		SharedWriteHandle sharedWriteHandle;
		string lockError;
		if (!ctx.Require(
			sharedWriteHandle.OpenAndRewrite(worldPath / L"region" / L"0.0.mca", state2.at(L"region/0.0.mca"), lockError),
			L("VAL_OK_SHARED_LOCK_CREATED"),
			MsgFmt("VAL_ERR_SHARED_LOCK_CREATE_FAILED", lockError)
		)) return false;
		const BackupResult firstSmart = RunDesktopBackup(
			world,
			L"CoreValidation_Smart_Locked",
			TaskCoordinator::CurrentStopToken());
		sharedWriteHandle.Close();
		if (!RequireExpectedBackup(ctx, firstSmart, BackupOutcome::Created) || ctx.StopRequested()) return false;
		historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(historyEntries.size() == 2, L("VAL_OK_FIRST_SMART_CREATED"), L("VAL_ERR_FIRST_SMART_NOT_CREATED"))) return false;
		if (!ctx.Require(historyEntries.back().backupType == L"Smart", L("VAL_OK_FIRST_SMART_TYPE"), L("VAL_ERR_FIRST_SMART_TYPE"))) return false;
		const wstring firstSmartBackupFile = historyEntries.back().backupFile;
		if (!AssertArchiveIntegrity(ctx, cfg, filesystem::path(cfg.backupPath) / world.name / firstSmartBackupFile)) return false;
		if (!AssertFolderRewindMetadata(ctx, cfg, world.name, firstSmartBackupFile, L"Smart")) return false;

		if (!SleepForUniqueBackupName(ctx.stopToken) || ctx.StopRequested()) return false;
		WriteTextFile(worldPath / L"notes.txt", state3.at(L"notes.txt"));
		WriteTextFile(worldPath / L"data" / L"base.txt", state3.at(L"data/base.txt"));
		WriteTextFile(worldPath / L"data" / L"fresh.txt", state3.at(L"data/fresh.txt"));
		RemoveIfExists(worldPath / L"to_delete.txt");
		const BackupResult secondSmart = RunDesktopBackup(
			world,
			L"CoreValidation_Smart_Delete",
			TaskCoordinator::CurrentStopToken());
		if (!RequireExpectedBackup(ctx, secondSmart, BackupOutcome::Created) || ctx.StopRequested()) return false;
		historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(historyEntries.size() == 3, L("VAL_OK_SECOND_SMART_CREATED"), L("VAL_ERR_SECOND_SMART_NOT_CREATED"))) return false;
		const wstring secondSmartBackupFile = historyEntries.back().backupFile;
		if (!AssertFolderRewindMetadata(ctx, cfg, world.name, secondSmartBackupFile, L"Smart")) return false;

		if (!SleepForUniqueBackupName(ctx.stopToken) || ctx.StopRequested()) return false;
		RemoveIfExists(worldPath / L"data" / L"fresh.txt");
		const BackupResult deletionOnly = RunDesktopBackup(
			world,
			L"CoreValidation_DeleteOnly",
			TaskCoordinator::CurrentStopToken());
		if (!RequireExpectedBackup(ctx, deletionOnly, BackupOutcome::Created) || ctx.StopRequested()) return false;
		historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(historyEntries.size() == 4, L("VAL_OK_DELETION_ONLY_CREATED"), L("VAL_ERR_DELETION_ONLY_NOT_CREATED"))) return false;
		const wstring latestBackupFile = historyEntries.back().backupFile;
		if (!AssertFolderRewindMetadata(ctx, cfg, world.name, latestBackupFile, L"Smart")) return false;
		if (!ctx.Require(GetBackupFilesForWorld(cfg, world.name).size() == 4, L("VAL_OK_ARCHIVE_COUNT_BEFORE_RESTORE"), L("VAL_ERR_ARCHIVE_COUNT_BEFORE_RESTORE"))) return false;

		WriteTextFile(worldPath / L"notes.txt", "corrupted-before-clean-restore\n");
		WriteTextFile(worldPath / L"manual_only.txt", "should-be-removed\n");
		WriteTextFile(worldPath / L"session.lock", "should-not-survive-restore\n");
		WriteTextFile(worldPath / L"locks" / L"runtime.lock", "should-not-survive-restore\n");
		if (!ctx.Require(DoRestore(cfg, world.name, fullBackupFile, 0, ""), L("VAL_OK_RESTORE_FULL_SUCCESS"), L("VAL_ERR_RESTORE_FULL_FAILED")) || ctx.StopRequested()) return false;
		string diff;
		if (!ctx.Require(CompareWorldState(state1, CaptureWorldState(worldPath), diff), L("VAL_OK_RESTORE_FULL_MATCH"), MsgFmt("VAL_ERR_RESTORE_FULL_MISMATCH", diff))) return false;
		if (!AssertLockArtifactsAbsent(ctx, worldPath)) return false;

		WriteTextFile(worldPath / L"notes.txt", "custom-restore-target\n");
		if (!ctx.Require(DoRestore(cfg, world.name, secondSmartBackupFile, 3, "notes.txt"), L("VAL_OK_CUSTOM_RESTORE_SUCCESS"), L("VAL_ERR_CUSTOM_RESTORE_FAILED")) || ctx.StopRequested()) return false;
		WorldState customExpected = state1;
		customExpected[L"notes.txt"] = state3.at(L"notes.txt");
		if (!ctx.Require(CompareWorldState(customExpected, CaptureWorldState(worldPath), diff), L("VAL_OK_CUSTOM_RESTORE_MATCH"), MsgFmt("VAL_ERR_CUSTOM_RESTORE_MISMATCH", diff))) return false;

		const auto safeDeleteTarget = find_if(historyEntries.begin(), historyEntries.end(), [&](const HistoryEntry& entry) {
			return entry.backupFile == firstSmartBackupFile;
		});
		if (!ctx.Require(safeDeleteTarget != historyEntries.end(), L("VAL_OK_SAFEDELETE_TARGET_FOUND"), L("VAL_ERR_SAFEDELETE_TARGET_NOT_FOUND"))) return false;
		DoSafeDeleteBackup(cfg, *safeDeleteTarget, kValidationConfigIndex);
		if (ctx.StopRequested()) return false;
		historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(historyEntries.size() == 3, L("VAL_OK_SAFEDELETE_HISTORY_SIZE"), L("VAL_ERR_SAFEDELETE_HISTORY_SIZE"))) return false;
		if (!ctx.Require(!filesystem::exists(filesystem::path(cfg.backupPath) / world.name / firstSmartBackupFile), L("VAL_OK_SAFEDELETE_ARCHIVE_REMOVED"), L("VAL_ERR_SAFEDELETE_ARCHIVE_PRESENT"))) return false;

		WriteTextFile(worldPath / L"notes.txt", "corrupted-before-final-restore\n");
		WriteTextFile(worldPath / L"manual_only.txt", "should-be-removed-again\n");
		WriteTextFile(worldPath / L"LOCK", "should-not-survive-restore\n");
		if (!ctx.Require(DoRestore(cfg, world.name, latestBackupFile, 0, ""), L("VAL_OK_FINAL_RESTORE_SUCCESS"), L("VAL_ERR_FINAL_RESTORE_FAILED")) || ctx.StopRequested()) return false;
		if (!ctx.Require(CompareWorldState(state4, CaptureWorldState(worldPath), diff), L("VAL_OK_FINAL_RESTORE_MATCH"), MsgFmt("VAL_ERR_FINAL_RESTORE_MISMATCH", diff))) return false;
		if (!AssertLockArtifactsAbsent(ctx, worldPath)) return false;

		return true;
	}

	static bool RunKeepCountScenario(ValidationContext& ctx, const Config& templateConfig, const filesystem::path& sandboxRoot) {
		if (ctx.StopRequested()) return false;
		ctx.Info(L("VAL_INFO_SCENARIO_LIMIT"));

		const filesystem::path saveRoot = sandboxRoot / L"worlds";
		const filesystem::path backupRoot = sandboxRoot / L"backups";
		const filesystem::path worldPath = saveRoot / kLimitWorldName;
		filesystem::create_directories(worldPath);

		Config cfg = BuildValidationConfig(templateConfig, saveRoot, backupRoot, 2, 2, false);
		{
			lock_guard<mutex> lock(g_appState.configsMutex);
			g_appState.configs[kValidationConfigIndex] = cfg;
		}
		MyFolder world{ worldPath.wstring(), kLimitWorldName, L"CoreValidation", cfg, kValidationConfigIndex, 1 };
		ClearValidationArtifactsForWorld(cfg, world.name);

		TemporarySafeDeleteMode noSafeDelete(false);

		WriteTextFile(worldPath / L"counter.txt", MakeDeterministicPayload("limit-case-v1", 128 * 1024, 0x456789abu));
		const BackupResult limitFirst = RunDesktopBackup(
			world,
			L"CoreValidation_Limit_1",
			TaskCoordinator::CurrentStopToken());
		if (!RequireExpectedBackup(ctx, limitFirst, BackupOutcome::Created) || ctx.StopRequested()) return false;
		auto historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(historyEntries.size() == 1, L("VAL_OK_LIMIT_FIRST_CREATED"), L("VAL_ERR_LIMIT_FIRST_FAILED"))) return false;
		const wstring oldestBackupFile = historyEntries.front().backupFile;

		if (!SleepForUniqueBackupName(ctx.stopToken) || ctx.StopRequested()) return false;
		WriteTextFile(worldPath / L"counter.txt", MakeDeterministicPayload("limit-case-v2", 160 * 1024, 0x56789abcu));
		const BackupResult limitSecond = RunDesktopBackup(
			world,
			L"CoreValidation_Limit_2",
			TaskCoordinator::CurrentStopToken());
		if (!RequireExpectedBackup(ctx, limitSecond, BackupOutcome::Created) || ctx.StopRequested()) return false;
		historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(historyEntries.size() == 2, L("VAL_OK_LIMIT_SECOND_CREATED"), L("VAL_ERR_LIMIT_SECOND_FAILED"))) return false;
		if (!ctx.Require(historyEntries.back().backupType == L"Smart", L("VAL_OK_LIMIT_SECOND_IS_SMART"), L("VAL_ERR_LIMIT_SECOND_NOT_SMART"))) return false;

		if (!SleepForUniqueBackupName(ctx.stopToken) || ctx.StopRequested()) return false;
		WriteTextFile(worldPath / L"counter.txt", MakeDeterministicPayload("limit-case-v3", 192 * 1024, 0x6789abcdu));
		const BackupResult limitThird = RunDesktopBackup(
			world,
			L"CoreValidation_Limit_3",
			TaskCoordinator::CurrentStopToken());
		if (!RequireExpectedBackup(ctx, limitThird, BackupOutcome::Created) || ctx.StopRequested()) return false;
		historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(!historyEntries.empty() && historyEntries.back().backupType == L"Smart", L("VAL_OK_LIMIT_THIRD_IS_SMART"), L("VAL_ERR_LIMIT_THIRD_NOT_SMART"))) return false;
		auto archives = GetBackupFilesForWorld(cfg, world.name);
		if (!ctx.Require(archives.size() == 2, L("VAL_OK_LIMIT_PRUNE_DISK"), L("VAL_ERR_LIMIT_PRUNE_DISK"))) return false;
		if (!ctx.Require(historyEntries.size() == 2, L("VAL_OK_LIMIT_PRUNE_HISTORY"), L("VAL_ERR_LIMIT_PRUNE_HISTORY"))) return false;
		const bool oldestRemoved = none_of(historyEntries.begin(), historyEntries.end(), [&](const HistoryEntry& entry) {
			return entry.backupFile == oldestBackupFile;
		});
		if (!ctx.Require(oldestRemoved, L("VAL_OK_LIMIT_OLDEST_REMOVED"), L("VAL_ERR_LIMIT_OLDEST_PRESENT"))) return false;

		return true;
	}

	static bool RunLegacyMigrationScenario(ValidationContext& ctx, const Config& templateConfig, const filesystem::path& sandboxRoot) {
		if (ctx.StopRequested()) return false;
		const wstring worldName = L"__CoreValidationMigration";
		Config cfg = BuildValidationConfig(templateConfig, sandboxRoot / L"worlds", sandboxRoot / L"migration-backups", 2, 0, true);
		cfg.name = "LegacyCloudConfig";
		cfg.cloudSyncEnabled = true;
		cfg.rcloneRemotePath = L"test:FolderRewind";
		cfg.configId = MigrationCoordinator::GenerateLegacyConfigId(cfg, kValidationConfigIndex);
		Config secondDevice = cfg;
		if (!ctx.Require(cfg.configId == MigrationCoordinator::GenerateLegacyConfigId(secondDevice, 999),
			"[Validation] Legacy ConfigId is deterministic across devices.", "[Validation] Legacy ConfigId is not deterministic.")) return false;
		g_appState.configs[kValidationConfigIndex] = cfg;

		const filesystem::path archiveDir = filesystem::path(cfg.backupPath) / worldName;
		const filesystem::path metadataDir = filesystem::path(cfg.backupPath) / L"_metadata" / worldName;
		filesystem::create_directories(archiveDir);
		filesystem::create_directories(metadataDir);
		const wstring fullName = L"[Full][2026-01-01_00-00-00]Old Description.7z";
		const wstring smartName = L"[Smart][2026-01-01_00-01-00]Old Description.7z";
		WriteTextFile(archiveDir / fullName, "archive-placeholder");
		WriteTextFile(archiveDir / smartName, "archive-placeholder");
		const long long ticks = static_cast<long long>(filesystem::last_write_time(archiveDir / smartName).time_since_epoch().count());
		nlohmann::json summary;
		summary["version"] = 2;
		summary["lastBackupFileName"] = wstring_to_utf8(smartName);
		summary["basedOnFullBackup"] = wstring_to_utf8(fullName);
		summary["fileStates"] = { { "level.dat", { { "size", 5 }, { "lastWriteTimeTicks", ticks } } } };
		summary["records"] = nlohmann::json::array({
			{ { "archiveFileName", wstring_to_utf8(fullName) }, { "backupType", "Full" }, { "basedOnFullBackup", wstring_to_utf8(fullName) }, { "previousBackupFileName", "" }, { "createdAtUtc", "2026-01-01T00:00:00Z" } },
			{ { "archiveFileName", wstring_to_utf8(smartName) }, { "backupType", "Smart" }, { "basedOnFullBackup", wstring_to_utf8(fullName) }, { "previousBackupFileName", wstring_to_utf8(fullName) }, { "createdAtUtc", "2026-01-01T00:01:00Z" } }
		});
		WriteTextFile(metadataDir / L"metadata.json", summary.dump(2));
		auto writeRecord = [&](const wstring& name, const string& type, const wstring& previous) {
			nlohmann::json record;
			record["archiveFileName"] = wstring_to_utf8(name);
			record["backupType"] = type;
			record["basedOnFullBackup"] = wstring_to_utf8(fullName);
			record["previousBackupFileName"] = wstring_to_utf8(previous);
			record["createdAtUtc"] = type == "Full" ? "2026-01-01T00:00:00Z" : "2026-01-01T00:01:00Z";
			record["addedFiles"] = nlohmann::json::array({ "level.dat" });
			record["modifiedFiles"] = nlohmann::json::array();
			record["deletedFiles"] = nlohmann::json::array();
			record["fullFileList"] = nlohmann::json::array({ "level.dat" });
			WriteTextFile(metadataDir / (name + L".json"), record.dump(2));
		};
		writeRecord(fullName, "Full", L"");
		writeRecord(smartName, "Smart", fullName);

		if (ctx.StopRequested()) return false;
		const MigrationUnitResult result = MigrationCoordinator::EnsureWorldMigrated(cfg, kValidationConfigIndex, worldName);
		if (ctx.StopRequested()) return false;
		if (!ctx.Require(result.status == MigrationStatus::Succeeded, "[Validation] 1.15 metadata migrated.", "[Validation] 1.15 metadata migration failed.")) return false;
		const filesystem::path snapshotRoot(result.snapshotPath);
		if (!ctx.Require(filesystem::exists(snapshotRoot / L"metadata.json")
			&& filesystem::exists(snapshotRoot / (fullName + L".json"))
			&& filesystem::exists(snapshotRoot / (smartName + L".json")),
			"[Validation] Legacy metadata recovery snapshot contains summary and records.",
			"[Validation] Legacy metadata recovery snapshot is incomplete.")) return false;
		if (!ctx.Require(
			MigrationCoordinator::HigherPriorityStatus(MigrationStatus::Succeeded, MigrationStatus::Pending) == MigrationStatus::Pending
			&& MigrationCoordinator::HigherPriorityStatus(MigrationStatus::Pending, MigrationStatus::Degraded) == MigrationStatus::Degraded
			&& MigrationCoordinator::HigherPriorityStatus(MigrationStatus::Degraded, MigrationStatus::Failed) == MigrationStatus::Failed,
			"[Validation] Migration report priority is Failed > Degraded > Pending > Succeeded.",
			"[Validation] Migration report priority is incorrect.")) return false;
		FolderRewindFormat::MetadataState state;
		if (!ctx.Require(FolderRewindMetadataStore::LoadState(metadataDir, state) && state.lastBackupFileName == smartName,
			"[Validation] Migrated state references the original archive name.", "[Validation] Migrated state is invalid.")) return false;
		FolderRewindFormat::ChangeRecord record;
		if (!ctx.Require(FolderRewindMetadataStore::LoadRecord(metadataDir, smartName, record) && record.previousBackupFileName == fullName,
			"[Validation] Migrated Smart chain is intact.", "[Validation] Migrated Smart chain is invalid.")) return false;
		return ctx.Require(filesystem::exists(archiveDir / fullName) && filesystem::exists(archiveDir / smartName),
			"[Validation] Legacy archives were not renamed.", "[Validation] Legacy archive files changed during migration.");
	}

	static bool RunCoreValidation(bool automatic, stop_token stopToken, bool& wasCancelled) {
		wasCancelled = false;
		ValidationContext ctx{ automatic, stopToken };
		if (ctx.StopRequested()) {
			wasCancelled = true;
			return false;
		}
		minebackup::logging::ScopedLogContext validationContext{{
			"operation_id", wstring_to_utf8(FolderRewindFormat::GenerateGuidString())},
			{"task", automatic ? "automatic_validation" : "manual_validation"}};
		ctx.Info(automatic ? L("VAL_INFO_START_AUTO") : L("VAL_INFO_START_MANUAL"));

		const LegacyValidationCleanupResult legacyCleanup =
			CleanupLegacyCoreValidationHistoryPollution();
		if (!legacyCleanup.success) {
			ctx.Require(false, "", L("VAL_ERR_LEGACY_HISTORY_CLEANUP"));
			return false;
		}
		if (legacyCleanup.removedEntries != 0) {
			ctx.Info(MsgFmt(
				"VAL_INFO_LEGACY_HISTORY_CLEANED",
				to_string(legacyCleanup.removedEntries)));
		}
		if (ctx.StopRequested()) {
			wasCancelled = true;
			return false;
		}

		const CoreValidationHistorySnapshot realHistoryBefore =
			CaptureRealHistorySnapshot();

		Config templateConfig;
		string resolveError;
		if (!TryResolveValidationTemplate(templateConfig, resolveError)) {
			ctx.Require(false, "", resolveError);
			return false;
		}
		if (ctx.StopRequested()) {
			wasCancelled = true;
			return false;
		}

		const filesystem::path sandboxRoot = GetAppPaths().runtimeRoot / L"MineBackup_CoreValidation" /
			to_wstring(chrono::steady_clock::now().time_since_epoch().count());
		ValidationCleanupGuard cleanup;
		cleanup.sandboxRoot = sandboxRoot;
		cleanup.previousSafeDelete = isSafeDelete;
		cleanup.historySnapshot = GetHistoryEntriesForConfig(kValidationConfigIndex);
		cleanup.hadHistorySnapshot = !cleanup.historySnapshot.empty();
		{
			lock_guard<mutex> lock(g_appState.configsMutex);
			auto configIt = g_appState.configs.find(kValidationConfigIndex);
			if (configIt != g_appState.configs.end()) {
				cleanup.hadConfigSnapshot = true;
				cleanup.configSnapshot = configIt->second;
			}
		}

		try {
			filesystem::create_directories(sandboxRoot / L"worlds");
			filesystem::create_directories(sandboxRoot / L"backups");
			if (!ctx.StopRequested()) RunLegacyMigrationScenario(ctx, templateConfig, sandboxRoot);
			if (!ctx.StopRequested()) RunSmartBackupScenario(ctx, templateConfig, sandboxRoot);
			if (!ctx.StopRequested()) RunKeepCountScenario(ctx, templateConfig, sandboxRoot);
		}
		catch (const exception& ex) {
			if (!ctx.StopRequested()) {
				ctx.Require(false, "", MsgFmt("VAL_ERR_UNEXPECTED_EXCEPTION", ex.what()));
			}
		}

		if (ctx.StopRequested()) {
			wasCancelled = true;
			return false;
		}

		size_t changedConfigCount = 0;
		const bool historyIsolated = AreCoreValidationHistorySnapshotsEqual(
			realHistoryBefore,
			CaptureRealHistorySnapshot(),
			&changedConfigCount);
		ctx.Require(
			historyIsolated,
			L("VAL_OK_HISTORY_ISOLATION"),
			MsgFmt("VAL_ERR_HISTORY_ISOLATION", to_string(changedConfigCount)));
		if (!cleanup.Finalize()) {
			ctx.Require(false, "", L("VAL_ERR_CLEANUP_FAILED"));
		}

		const bool passed = ctx.failures.empty();
		if (passed) {
			ctx.Info(L("VAL_INFO_COMPLETED"));
		}
		else {
			VALIDATION_ERROR("%s", MsgFmt("VAL_ERR_FINISHED_WITH_COUNT", static_cast<int>(ctx.failures.size())).c_str());
			for (size_t index = 0; index < ctx.failures.size(); ++index) {
				VALIDATION_ERROR("%d. %s", static_cast<int>(index + 1), ctx.failures[index].c_str());
			}
		}

		return passed;
	}
}

bool IsLegacyCoreValidationPollution(const HistoryEntry& entry) {
	return IsLegacyCoreValidationPollutionInternal(entry);
}

size_t RemoveLegacyCoreValidationPollution(vector<HistoryEntry>& entries) {
	const size_t oldSize = entries.size();
	erase_if(entries, [](const HistoryEntry& entry) {
		return IsLegacyCoreValidationPollutionInternal(entry);
	});
	return oldSize - entries.size();
}

bool AreCoreValidationHistorySnapshotsEqual(
	const CoreValidationHistorySnapshot& before,
	const CoreValidationHistorySnapshot& after,
	size_t* changedConfigCount) {
	return AreCoreValidationHistorySnapshotsEqualInternal(
		before, after, changedConfigCount);
}

bool StartCoreValidationAsync(bool automatic) {
	bool expected = false;
	if (!g_CoreValidationRunning.compare_exchange_strong(expected, true)) {
		VALIDATION_INFO("%s", L("VAL_INFO_ALREADY_RUNNING"));
		return true;
	}

	VALIDATION_INFO("%s", automatic ? L("VAL_INFO_QUEUED_AUTO") : L("VAL_INFO_QUEUED_MANUAL"));
	if (!TaskCoordinator::Instance().Submit(L"core-validation", {L"validation"}, [automatic](stop_token token) {
		bool passed = false;
		bool wasCancelled = false;
		try {
			passed = RunCoreValidation(automatic, token, wasCancelled);
		}
		catch (const exception& ex) {
			if (token.stop_requested()) {
				wasCancelled = true;
			}
			else {
				VALIDATION_ERROR("%s", MsgFmt("VAL_ERR_WORKER_CRASHED", ex.what()).c_str());
			}
		}

		if (wasCancelled || token.stop_requested()) {
			VALIDATION_INFO("Core validation cancelled due to application shutdown.");
			g_CoreValidationRunning.store(false);
			return;
		}

		g_CoreValidationPassed.store(passed);
		g_CoreValidationPending.store(false);
		SaveConfigs();
		if (passed) {
			VALIDATION_INFO("%s", L("VAL_INFO_PASSED"));
		}
		else {
			VALIDATION_ERROR("%s", L("VAL_INFO_FAILED_RETRY"));
		}
		g_CoreValidationRunning.store(false);
	})) {
		g_CoreValidationRunning.store(false);
		VALIDATION_ERROR("Task coordinator is shutting down.");
		return false;
	}
	return true;
}
