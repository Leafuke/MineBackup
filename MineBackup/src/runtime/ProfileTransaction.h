#pragma once
#include "AppPaths.h"
#include "AtomicFileWriter.h"
#include "OperationResult.h"
#include <functional>
#include <optional>

enum class ConfigSaveState {
    NotCommitted,
    CommittedNotDurable,
    CommittedDurably,
    RecoveryRequired
};

struct ConfigSaveResult {
    ConfigSaveState state = ConfigSaveState::NotCommitted;
    std::wstring detail;
    bool Committed() const noexcept {
        return state == ConfigSaveState::CommittedNotDurable || state == ConfigSaveState::CommittedDurably;
    }
    bool Durable() const noexcept { return state == ConfigSaveState::CommittedDurably; }
    bool CanRollbackMemory() const noexcept { return state == ConfigSaveState::NotCommitted; }
};

namespace ProfileTransaction {
struct Documents {
    std::string config;
    std::string jobs;
    std::optional<std::string> history;
};
// A scoped writer is also the deterministic failure-injection seam for existing tests.
using Writer = std::function<AtomicFileWriter::WriteResult(const std::filesystem::path&, const std::string&)>;
ConfigSaveResult Commit(const AppPaths& paths, const Documents& documents, Writer writer = {}, const std::filesystem::path& configFile = {});
bool Recover(const AppPaths& paths, std::vector<Diagnostic>& diagnostics, Writer writer = {});
// Read-only: a prepared/invalid journal prevents loading a mixed configuration.
bool Inspect(const std::filesystem::path& configFile, std::vector<Diagnostic>& diagnostics);
}
