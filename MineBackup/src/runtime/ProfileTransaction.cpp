#include "ProfileTransaction.h"
#include "json.hpp"
#include "text_to_text.h"
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <array>

using namespace std;
namespace ProfileTransaction {
namespace {
constexpr const char* JournalName = ".profile-apply-transaction.json";
constexpr array<const char*, 3> Names{"config", "jobs", "history"};
recursive_mutex transactionMutex;

filesystem::path Snapshot(const AppPaths& paths, size_t index) {
    return paths.configRoot / (string(".profile-apply-") + Names[index] + ".rollback");
}
array<filesystem::path, 3> Targets(const AppPaths& paths) {
    return {paths.ConfigFile(), paths.JobsFile(), paths.HistoryFile()};
}
string Read(const filesystem::path& path) {
    ifstream input(path, ios::binary);
    if (!input.is_open()) throw runtime_error("Cannot read transaction file: " + path.string());
    string text((istreambuf_iterator<char>(input)), {});
    if (input.bad()) throw runtime_error("Cannot finish reading transaction file: " + path.string());
    return text;
}
Writer EffectiveWriter(Writer writer) {
    return writer ? std::move(writer) : Writer([](const auto& path, const auto& content) {
        return AtomicFileWriter::WriteText(path, content, {false, true});
    });
}
void Cleanup(const AppPaths& paths) {
    // Remove the journal first. A crash must never leave prepared + missing snapshots.
    error_code error;
    filesystem::remove(paths.configRoot / JournalName, error);
    if (error) return;
    for (size_t i = 0; i < Names.size(); ++i) filesystem::remove(Snapshot(paths, i), error);
}
nlohmann::json Journal(const AppPaths& paths) {
    auto value = nlohmann::json::parse(Read(paths.configRoot / JournalName));
    const int version = value.at("schemaVersion").get<int>();
    const auto phase = value.at("phase").get<string>();
    if ((version != 1 && version != 2) || (phase != "prepared" && phase != "committed"))
        throw runtime_error("Unsupported configuration transaction journal");
    for (const char* name : {"configExisted", "jobsExisted"})
        if (!value.contains(name) || !value[name].is_boolean()) throw runtime_error("Invalid transaction existence flag");
    if (version == 2 && (!value.contains("historyIncluded") || !value["historyIncluded"].is_boolean()
        || !value.contains("historyExisted") || !value["historyExisted"].is_boolean()))
        throw runtime_error("Invalid transaction history flags");
    return value;
}
}

bool Inspect(const filesystem::path& configFile, vector<Diagnostic>& diagnostics) {
    lock_guard lock(transactionMutex);
    AppPaths paths;
    paths.configRoot = configFile.parent_path();
    try {
        if (!filesystem::exists(paths.configRoot / JournalName)) return true;
        if (Journal(paths).at("phase") == "committed") return true;
        diagnostics.push_back({"profile.transaction.recovery_required", DiagnosticSeverity::Error,
            "An unfinished configuration transaction requires exclusive recovery."});
    } catch (const exception& error) {
        diagnostics.push_back({"profile.transaction.invalid_journal", DiagnosticSeverity::Error, error.what()});
    }
    return false;
}

bool Recover(const AppPaths& paths, vector<Diagnostic>& diagnostics, Writer writer) {
    lock_guard lock(transactionMutex);
    try {
        if (!filesystem::exists(paths.configRoot / JournalName)) return true;
        const auto journal = Journal(paths);
        if (journal.at("phase") == "committed") { Cleanup(paths); return true; }
        auto write = EffectiveWriter(std::move(writer));
        auto targets = Targets(paths);
        if (journal.contains("configName")) {
            const filesystem::path name = utf8_to_wstring(journal.at("configName").get<string>());
            if (name.empty() || name.has_parent_path() || name == L"." || name == L"..")
                throw runtime_error("Invalid transaction configuration name");
            targets[0] = paths.configRoot / name;
        }
        const size_t count = journal.value("historyIncluded", false) ? 3 : 2;
        // Validate every required snapshot before changing any target.
        array<optional<string>, 3> original;
        for (size_t i = 0; i < count; ++i) {
            if (journal.at(string(Names[i]) + "Existed").get<bool>()) original[i] = Read(Snapshot(paths, i));
        }
        for (size_t i = 0; i < count; ++i) {
            if (original[i]) {
                const auto restored = write(targets[i], *original[i]);
                if (!restored.IsDurable()) throw runtime_error("Transaction rollback could not be persisted");
            } else {
                error_code error;
                filesystem::remove(targets[i], error);
                if (error) throw filesystem::filesystem_error("Transaction rollback removal failed", targets[i], error);
            }
        }
        Cleanup(paths);
        diagnostics.push_back({"profile.transaction.rollback_recovered", DiagnosticSeverity::Warning, {}});
        return true;
    } catch (const exception& error) {
        diagnostics.push_back({"profile.transaction.recovery_required", DiagnosticSeverity::Error, error.what()});
        return false;
    }
}

ConfigSaveResult Commit(const AppPaths& paths, const Documents& documents, Writer writer, const filesystem::path& configFile) {
    lock_guard lock(transactionMutex);
    vector<Diagnostic> diagnostics;
    auto write = EffectiveWriter(std::move(writer));
    if (!Recover(paths, diagnostics, write))
        return {ConfigSaveState::RecoveryRequired, utf8_to_wstring(diagnostics.back().detail)};
    bool prepared = false;
    bool committed = false;
    bool durable = true;
    try {
        auto targets = Targets(paths);
        if (!configFile.empty()) {
            if (filesystem::absolute(configFile.parent_path()).lexically_normal()
                != filesystem::absolute(paths.configRoot).lexically_normal()) throw runtime_error("Configuration must belong to profile root");
            targets[0] = configFile;
        }
        const array<string, 3> content{documents.config, documents.jobs, documents.history.value_or("")};
        const size_t count = documents.history ? 3 : 2;
        nlohmann::json journal{{"schemaVersion", 2}, {"phase", "prepared"},
            {"configName", wstring_to_utf8(targets[0].filename().wstring())},
            {"historyIncluded", documents.history.has_value()}, {"historyExisted", false}};
        for (size_t i = 0; i < count; ++i) {
            const bool existed = filesystem::exists(targets[i]);
            journal[string(Names[i]) + "Existed"] = existed;
            if (existed && !write(Snapshot(paths, i), Read(targets[i])).IsDurable())
                throw runtime_error("Cannot persist transaction snapshot");
        }
        const auto preparation = write(paths.configRoot / JournalName, journal.dump());
        prepared = preparation.WasReplaced();
        if (!preparation.IsDurable()) throw runtime_error("Cannot persist prepared transaction journal");
        for (size_t i = 0; i < count; ++i) {
            const auto replacement = write(targets[i], content[i]);
            if (!replacement.WasReplaced()) throw runtime_error("Cannot replace transaction target: " + targets[i].string());
            durable = durable && replacement.IsDurable();
        }
        journal["phase"] = "committed";
        const auto marker = write(paths.configRoot / JournalName, journal.dump());
        committed = marker.WasReplaced();
        if (!committed) throw runtime_error("Cannot commit transaction journal");
        durable = durable && marker.IsDurable();
        if (durable) Cleanup(paths);
        return {durable ? ConfigSaveState::CommittedDurably : ConfigSaveState::CommittedNotDurable,
            durable ? L"" : L"Configuration committed; persistence could not be fully confirmed."};
    } catch (const exception& error) {
        if (committed) return {ConfigSaveState::CommittedNotDurable, utf8_to_wstring(error.what())};
        if (prepared && !Recover(paths, diagnostics, write))
            return {ConfigSaveState::RecoveryRequired, utf8_to_wstring(error.what())};
        return {ConfigSaveState::NotCommitted, utf8_to_wstring(error.what())};
    }
}
}
