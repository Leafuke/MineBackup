#include "CoreValidation.h"

#include "BackupManager.h"
#include "ConfigManager.h"
#include "Console.h"
#include "Globals.h"
#include "HistoryManager.h"
#include "i18n.h"
#include "PlatformCompat.h"
#include "text_to_text.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

using namespace std;

namespace {
	constexpr int kValidationConfigIndex = -424242;
	constexpr const wchar_t* kSmartWorldName = L"__CoreValidationSmart";
	constexpr const wchar_t* kLimitWorldName = L"__CoreValidationLimit";

	using WorldState = map<wstring, string>;

	static string Msg(const char* key) {
		return string(L(key));
	}

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

		auto it = g_appState.g_history.find(kValidationConfigIndex);
		if (it != g_appState.g_history.end()) {
			auto& history = it->second;
			history.erase(remove_if(history.begin(), history.end(), [&](const HistoryEntry& entry) {
				return entry.worldName == worldName;
			}), history.end());
		}
	}

	static string MakeNumericPayload(const string& label, int lineCount) {
		ostringstream builder;
		builder << label << '\n';
		for (int line = 0; line < lineCount; ++line) {
			builder << line << ':';
			for (int digit = 0; digit < 48; ++digit) {
				builder << ((line + digit) % 10);
			}
			builder << '\n';
		}
		return builder.str();
	}

	static vector<HistoryEntry> GetHistoryEntriesForWorld(int configIndex, const wstring& worldName) {
		vector<HistoryEntry> out;
		auto it = g_appState.g_history.find(configIndex);
		if (it == g_appState.g_history.end()) return out;
		for (const auto& entry : it->second) {
			if (entry.worldName == worldName) {
				out.push_back(entry);
			}
		}
		return out;
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

		error = Msg("VAL_ERR_NO_7Z_CONFIG");
		return false;
	}

	static void SleepForUniqueBackupName() {
		this_thread::sleep_for(chrono::milliseconds(1100));
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
				error = Msg("VAL_ERR_SHARED_OPEN");
				return false;
			}

			LARGE_INTEGER start{};
			if (!SetFilePointerEx(handle_, start, nullptr, FILE_BEGIN) || !SetEndOfFile(handle_)) {
				error = Msg("VAL_ERR_SHARED_TRUNCATE");
				Close();
				return false;
			}

			DWORD written = 0;
			if (!WriteFile(handle_, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) || written != content.size()) {
				error = Msg("VAL_ERR_SHARED_WRITE");
				Close();
				return false;
			}

			FlushFileBuffers(handle_);
			return true;
#else
			stream_.open(filePath, ios::binary | ios::in | ios::out | ios::trunc);
			if (!stream_.is_open()) {
				error = Msg("VAL_ERR_SHARED_OPEN_STREAM");
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
		Console& console;
		bool automatic = false;
		vector<string> failures;

		void Info(const string& message) {
			console.AddLog("[Info] [Validation] %s", message.c_str());
		}

		bool Require(bool condition, const string& successMessage, const string& failureMessage) {
			if (condition) {
				if (!successMessage.empty()) {
					Info(successMessage);
				}
				return true;
			}
			failures.push_back(failureMessage);
			console.AddLog("[Error] [Validation] %s", failureMessage.c_str());
			return false;
		}
	};

	struct ValidationCleanupGuard {
		filesystem::path sandboxRoot;
		bool previousSafeDelete = true;
		bool hadHistorySnapshot = false;
		vector<HistoryEntry> historySnapshot;

		~ValidationCleanupGuard() {
			isSafeDelete = previousSafeDelete;
			if (hadHistorySnapshot) {
				g_appState.g_history[kValidationConfigIndex] = historySnapshot;
			}
			else {
				g_appState.g_history.erase(kValidationConfigIndex);
			}
			SaveHistory();
			error_code ec;
			filesystem::remove_all(sandboxRoot, ec);
		}
	};

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
			worldPath / L"session.lock",
			worldPath / L"LOCK",
			worldPath / L"locks" / L"runtime.lock"
		};
		for (const auto& path : disallowed) {
			if (filesystem::exists(path)) {
				const string message = MsgFmt("VAL_ERR_LOCK_ARTIFACT_LEFT", wstring_to_utf8(path.wstring()));
				ctx.failures.push_back(message);
				ctx.console.AddLog("[Error] [Validation] %s", message.c_str());
				return false;
			}
		}
		ctx.Info(Msg("VAL_INFO_LOCK_FILES_EXCLUDED"));
		return true;
	}

	static bool RunSmartBackupScenario(ValidationContext& ctx, const Config& templateConfig, const filesystem::path& sandboxRoot) {
		ctx.Info(Msg("VAL_INFO_SCENARIO_SMART"));

		const filesystem::path saveRoot = sandboxRoot / L"worlds";
		const filesystem::path backupRoot = sandboxRoot / L"backups";
		const filesystem::path worldPath = saveRoot / kSmartWorldName;
		filesystem::create_directories(worldPath / L"data");
		filesystem::create_directories(worldPath / L"region");
		filesystem::create_directories(worldPath / L"locks");

		Config cfg = BuildValidationConfig(templateConfig, saveRoot, backupRoot, 2, 0, true);
		MyFolder world{ worldPath.wstring(), kSmartWorldName, L"CoreValidation", cfg, kValidationConfigIndex, 0 };
		ClearValidationArtifactsForWorld(cfg, world.name);
		SaveHistory();

		WorldState state1 = {
			{ L"notes.txt", MakeNumericPayload("base-notes", 260) },
			{ L"data/base.txt", "base-file-v1\n" },
			{ L"region/0.0.mca", "region-v1\n" },
			{ L"to_delete.txt", "delete-me-later\n" }
		};
		WorldState state2 = state1;
		state2[L"notes.txt"] = MakeNumericPayload("smart-notes-1", 280);
		state2[L"data/add.txt"] = "added-on-smart-backup\n";
		state2[L"region/0.0.mca"] = "region-v2-open-for-write\n";
		WorldState state3 = state2;
		state3[L"notes.txt"] = MakeNumericPayload("smart-notes-2", 300);
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
		DoBackup(world, ctx.console, L"CoreValidation_Base");
		auto historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(historyEntries.size() == 1, Msg("VAL_OK_INITIAL_FULL_CREATED"), Msg("VAL_ERR_INITIAL_FULL_NOT_CREATED"))) return false;
		if (!ctx.Require(historyEntries[0].backupType == L"Full", Msg("VAL_OK_FIRST_TYPE_FULL"), Msg("VAL_ERR_FIRST_TYPE_NOT_FULL"))) return false;
		const wstring fullBackupFile = historyEntries[0].backupFile;
		if (!ctx.Require(GetBackupFilesForWorld(cfg, world.name).size() == 1, Msg("VAL_OK_ARCHIVE_COUNT_AFTER_FIRST"), Msg("VAL_ERR_ARCHIVE_COUNT_AFTER_FIRST"))) return false;

		world.config.skipIfUnchanged = true;
		SleepForUniqueBackupName();
		DoBackup(world, ctx.console, L"CoreValidation_NoChange");
		historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(historyEntries.size() == 1, Msg("VAL_OK_SKIP_NO_CHANGE"), Msg("VAL_ERR_NO_CHANGE_CREATED"))) return false;

		world.config.skipIfUnchanged = false;
		SleepForUniqueBackupName();
		WriteTextFile(worldPath / L"notes.txt", state2.at(L"notes.txt"));
		WriteTextFile(worldPath / L"data" / L"add.txt", state2.at(L"data/add.txt"));
		SharedWriteHandle sharedWriteHandle;
		string lockError;
		if (!ctx.Require(
			sharedWriteHandle.OpenAndRewrite(worldPath / L"region" / L"0.0.mca", state2.at(L"region/0.0.mca"), lockError),
			Msg("VAL_OK_SHARED_LOCK_CREATED"),
			MsgFmt("VAL_ERR_SHARED_LOCK_CREATE_FAILED", lockError)
		)) return false;
		DoBackup(world, ctx.console, L"CoreValidation_Smart_Locked");
		sharedWriteHandle.Close();
		historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(historyEntries.size() == 2, Msg("VAL_OK_FIRST_SMART_CREATED"), Msg("VAL_ERR_FIRST_SMART_NOT_CREATED"))) return false;
		if (!ctx.Require(historyEntries.back().backupType == L"Smart", Msg("VAL_OK_FIRST_SMART_TYPE"), Msg("VAL_ERR_FIRST_SMART_TYPE"))) return false;
		const wstring firstSmartBackupFile = historyEntries.back().backupFile;

		SleepForUniqueBackupName();
		WriteTextFile(worldPath / L"notes.txt", state3.at(L"notes.txt"));
		WriteTextFile(worldPath / L"data" / L"base.txt", state3.at(L"data/base.txt"));
		WriteTextFile(worldPath / L"data" / L"fresh.txt", state3.at(L"data/fresh.txt"));
		RemoveIfExists(worldPath / L"to_delete.txt");
		DoBackup(world, ctx.console, L"CoreValidation_Smart_Delete");
		historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(historyEntries.size() == 3, Msg("VAL_OK_SECOND_SMART_CREATED"), Msg("VAL_ERR_SECOND_SMART_NOT_CREATED"))) return false;
		const wstring secondSmartBackupFile = historyEntries.back().backupFile;

		SleepForUniqueBackupName();
		RemoveIfExists(worldPath / L"data" / L"fresh.txt");
		DoBackup(world, ctx.console, L"CoreValidation_DeleteOnly");
		historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(historyEntries.size() == 4, Msg("VAL_OK_DELETION_ONLY_CREATED"), Msg("VAL_ERR_DELETION_ONLY_NOT_CREATED"))) return false;
		const wstring latestBackupFile = historyEntries.back().backupFile;
		if (!ctx.Require(GetBackupFilesForWorld(cfg, world.name).size() == 4, Msg("VAL_OK_ARCHIVE_COUNT_BEFORE_RESTORE"), Msg("VAL_ERR_ARCHIVE_COUNT_BEFORE_RESTORE"))) return false;

		WriteTextFile(worldPath / L"notes.txt", "corrupted-before-clean-restore\n");
		WriteTextFile(worldPath / L"manual_only.txt", "should-be-removed\n");
		WriteTextFile(worldPath / L"session.lock", "should-not-survive-restore\n");
		WriteTextFile(worldPath / L"locks" / L"runtime.lock", "should-not-survive-restore\n");
		if (!ctx.Require(DoRestore(cfg, world.name, fullBackupFile, ctx.console, 0, ""), Msg("VAL_OK_RESTORE_FULL_SUCCESS"), Msg("VAL_ERR_RESTORE_FULL_FAILED"))) return false;
		string diff;
		if (!ctx.Require(CompareWorldState(state1, CaptureWorldState(worldPath), diff), Msg("VAL_OK_RESTORE_FULL_MATCH"), MsgFmt("VAL_ERR_RESTORE_FULL_MISMATCH", diff))) return false;
		if (!AssertLockArtifactsAbsent(ctx, worldPath)) return false;

		WriteTextFile(worldPath / L"notes.txt", "custom-restore-target\n");
		if (!ctx.Require(DoRestore(cfg, world.name, secondSmartBackupFile, ctx.console, 3, "notes.txt"), Msg("VAL_OK_CUSTOM_RESTORE_SUCCESS"), Msg("VAL_ERR_CUSTOM_RESTORE_FAILED"))) return false;
		WorldState customExpected = state1;
		customExpected[L"notes.txt"] = state3.at(L"notes.txt");
		if (!ctx.Require(CompareWorldState(customExpected, CaptureWorldState(worldPath), diff), Msg("VAL_OK_CUSTOM_RESTORE_MATCH"), MsgFmt("VAL_ERR_CUSTOM_RESTORE_MISMATCH", diff))) return false;

		const auto safeDeleteTarget = find_if(historyEntries.begin(), historyEntries.end(), [&](const HistoryEntry& entry) {
			return entry.backupFile == firstSmartBackupFile;
		});
		if (!ctx.Require(safeDeleteTarget != historyEntries.end(), Msg("VAL_OK_SAFEDELETE_TARGET_FOUND"), Msg("VAL_ERR_SAFEDELETE_TARGET_NOT_FOUND"))) return false;
		DoSafeDeleteBackup(cfg, *safeDeleteTarget, kValidationConfigIndex, ctx.console);
		historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(historyEntries.size() == 3, Msg("VAL_OK_SAFEDELETE_HISTORY_SIZE"), Msg("VAL_ERR_SAFEDELETE_HISTORY_SIZE"))) return false;
		if (!ctx.Require(!filesystem::exists(filesystem::path(cfg.backupPath) / world.name / firstSmartBackupFile), Msg("VAL_OK_SAFEDELETE_ARCHIVE_REMOVED"), Msg("VAL_ERR_SAFEDELETE_ARCHIVE_PRESENT"))) return false;

		WriteTextFile(worldPath / L"notes.txt", "corrupted-before-final-restore\n");
		WriteTextFile(worldPath / L"manual_only.txt", "should-be-removed-again\n");
		WriteTextFile(worldPath / L"LOCK", "should-not-survive-restore\n");
		if (!ctx.Require(DoRestore(cfg, world.name, latestBackupFile, ctx.console, 0, ""), Msg("VAL_OK_FINAL_RESTORE_SUCCESS"), Msg("VAL_ERR_FINAL_RESTORE_FAILED"))) return false;
		if (!ctx.Require(CompareWorldState(state4, CaptureWorldState(worldPath), diff), Msg("VAL_OK_FINAL_RESTORE_MATCH"), MsgFmt("VAL_ERR_FINAL_RESTORE_MISMATCH", diff))) return false;
		if (!AssertLockArtifactsAbsent(ctx, worldPath)) return false;

		return true;
	}

	static bool RunKeepCountScenario(ValidationContext& ctx, const Config& templateConfig, const filesystem::path& sandboxRoot) {
		ctx.Info(Msg("VAL_INFO_SCENARIO_LIMIT"));

		const filesystem::path saveRoot = sandboxRoot / L"worlds";
		const filesystem::path backupRoot = sandboxRoot / L"backups";
		const filesystem::path worldPath = saveRoot / kLimitWorldName;
		filesystem::create_directories(worldPath);

		Config cfg = BuildValidationConfig(templateConfig, saveRoot, backupRoot, 2, 2, false);
		MyFolder world{ worldPath.wstring(), kLimitWorldName, L"CoreValidation", cfg, kValidationConfigIndex, 1 };
		ClearValidationArtifactsForWorld(cfg, world.name);
		SaveHistory();

		TemporarySafeDeleteMode noSafeDelete(false);

		WriteTextFile(worldPath / L"counter.txt", MakeNumericPayload("limit-case-v1", 240));
		DoBackup(world, ctx.console, L"CoreValidation_Limit_1");
		auto historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(historyEntries.size() == 1, Msg("VAL_OK_LIMIT_FIRST_CREATED"), Msg("VAL_ERR_LIMIT_FIRST_FAILED"))) return false;
		const wstring oldestBackupFile = historyEntries.front().backupFile;

		SleepForUniqueBackupName();
		WriteTextFile(worldPath / L"counter.txt", MakeNumericPayload("limit-case-v2", 260));
		DoBackup(world, ctx.console, L"CoreValidation_Limit_2");
		historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(historyEntries.size() == 2, Msg("VAL_OK_LIMIT_SECOND_CREATED"), Msg("VAL_ERR_LIMIT_SECOND_FAILED"))) return false;
		if (!ctx.Require(historyEntries.back().backupType == L"Smart", Msg("VAL_OK_LIMIT_SECOND_IS_SMART"), Msg("VAL_ERR_LIMIT_SECOND_NOT_SMART"))) return false;

		SleepForUniqueBackupName();
		WriteTextFile(worldPath / L"counter.txt", MakeNumericPayload("limit-case-v3", 280));
		DoBackup(world, ctx.console, L"CoreValidation_Limit_3");
		historyEntries = GetHistoryEntriesForWorld(kValidationConfigIndex, world.name);
		if (!ctx.Require(!historyEntries.empty() && historyEntries.back().backupType == L"Smart", Msg("VAL_OK_LIMIT_THIRD_IS_SMART"), Msg("VAL_ERR_LIMIT_THIRD_NOT_SMART"))) return false;
		auto archives = GetBackupFilesForWorld(cfg, world.name);
		if (!ctx.Require(archives.size() == 2, Msg("VAL_OK_LIMIT_PRUNE_DISK"), Msg("VAL_ERR_LIMIT_PRUNE_DISK"))) return false;
		if (!ctx.Require(historyEntries.size() == 2, Msg("VAL_OK_LIMIT_PRUNE_HISTORY"), Msg("VAL_ERR_LIMIT_PRUNE_HISTORY"))) return false;
		const bool oldestRemoved = none_of(historyEntries.begin(), historyEntries.end(), [&](const HistoryEntry& entry) {
			return entry.backupFile == oldestBackupFile;
		});
		if (!ctx.Require(oldestRemoved, Msg("VAL_OK_LIMIT_OLDEST_REMOVED"), Msg("VAL_ERR_LIMIT_OLDEST_PRESENT"))) return false;

		return true;
	}

	static bool RunCoreValidation(Console& console, bool automatic) {
		ValidationContext ctx{ console, automatic };
		ctx.Info(automatic ? Msg("VAL_INFO_START_AUTO") : Msg("VAL_INFO_START_MANUAL"));

		Config templateConfig;
		string resolveError;
		if (!TryResolveValidationTemplate(templateConfig, resolveError)) {
			ctx.Require(false, "", resolveError);
			return false;
		}

		const filesystem::path sandboxRoot = filesystem::temp_directory_path() / L"MineBackup_CoreValidation" /
			to_wstring(chrono::steady_clock::now().time_since_epoch().count());
		ValidationCleanupGuard cleanup;
		cleanup.sandboxRoot = sandboxRoot;
		cleanup.previousSafeDelete = isSafeDelete;
		auto historyIt = g_appState.g_history.find(kValidationConfigIndex);
		if (historyIt != g_appState.g_history.end()) {
			cleanup.hadHistorySnapshot = true;
			cleanup.historySnapshot = historyIt->second;
		}

		try {
			filesystem::create_directories(sandboxRoot / L"worlds");
			filesystem::create_directories(sandboxRoot / L"backups");
			RunSmartBackupScenario(ctx, templateConfig, sandboxRoot);
			RunKeepCountScenario(ctx, templateConfig, sandboxRoot);
		}
		catch (const exception& ex) {
			ctx.Require(false, "", MsgFmt("VAL_ERR_UNEXPECTED_EXCEPTION", ex.what()));
		}

		const bool passed = ctx.failures.empty();
		if (passed) {
			ctx.Info(Msg("VAL_INFO_COMPLETED"));
		}
		else {
			ctx.console.AddLog("[Error] [Validation] %s", MsgFmt("VAL_ERR_FINISHED_WITH_COUNT", static_cast<int>(ctx.failures.size())).c_str());
			for (size_t index = 0; index < ctx.failures.size(); ++index) {
				ctx.console.AddLog("[Error] [Validation] %d. %s", static_cast<int>(index + 1), ctx.failures[index].c_str());
			}
		}

		return passed;
	}
}

void StartCoreValidationAsync(bool automatic, Console& console) {
	bool expected = false;
	if (!g_CoreValidationRunning.compare_exchange_strong(expected, true)) {
		console.AddLog("[Info] [Validation] %s", L("VAL_INFO_ALREADY_RUNNING"));
		return;
	}

	console.AddLog("[Info] [Validation] %s", automatic ? L("VAL_INFO_QUEUED_AUTO") : L("VAL_INFO_QUEUED_MANUAL"));
	thread([automatic, &console]() {
		bool passed = false;
		try {
			passed = RunCoreValidation(console, automatic);
		}
		catch (const exception& ex) {
			console.AddLog("[Error] [Validation] %s", MsgFmt("VAL_ERR_WORKER_CRASHED", ex.what()).c_str());
		}

		g_CoreValidationPassed.store(passed);
		g_CoreValidationPending.store(false);
		SaveConfigs();
		console.AddLog("[Info] [Validation] %s", passed ? L("VAL_INFO_PASSED") : L("VAL_INFO_FAILED_RETRY"));
		g_CoreValidationRunning.store(false);
	}).detach();
}
