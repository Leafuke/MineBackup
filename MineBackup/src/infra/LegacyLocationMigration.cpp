#include "LegacyLocationMigration.h"

#include "AtomicFileWriter.h"
#include "json.hpp"

#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace std;

namespace {

bool IsUntrustedLink(const filesystem::path& path) {
    error_code error;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return filesystem::is_symlink(filesystem::symlink_status(path, error)) || error;
#endif
}

bool ReadBounded(const filesystem::path& path, uintmax_t maximumBytes, string& content, wstring& error) {
    error_code fileError;
    if (!filesystem::is_regular_file(path, fileError) || fileError || IsUntrustedLink(path)) {
        error = L"A legacy source file is missing, inaccessible, or linked.";
        return false;
    }
    const auto size = filesystem::file_size(path, fileError);
    if (fileError || size > maximumBytes) {
        error = L"A legacy source file exceeds its safety limit.";
        return false;
    }
    ifstream input(path, ios::binary);
    content.assign(istreambuf_iterator<char>(input), istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        error = L"A legacy source file could not be read completely.";
        return false;
    }
    return true;
}

} // namespace

LegacyLocationMigrationResult ImportLegacyLocation(
    const LegacyLocationCandidate& source,
    const filesystem::path& targetConfigFile,
    const filesystem::path& targetHistoryFile) {
    LegacyLocationMigrationResult result;
    error_code error;
    const bool configTargetExists = filesystem::exists(targetConfigFile, error);
    if (error) {
        result.error = L"The target configuration location could not be inspected safely.";
        return result;
    }
    const bool historyTargetExists = filesystem::exists(targetHistoryFile, error);
    if (error) {
        result.error = L"The target history location could not be inspected safely.";
        return result;
    }
    if (configTargetExists || historyTargetExists) {
        result.error = L"The target profile is already initialized; startup migration will not merge data.";
        return result;
    }
    if (source.configFile.empty()) {
        result.error = L"The selected legacy location does not contain config.ini.";
        return result;
    }

    string config;
    if (!ReadBounded(source.configFile, 16u * 1024u * 1024u, config, result.error)) return result;
    if (config.find("[General]") == string::npos) {
        result.error = L"The selected legacy config.ini is not a recognized MineBackup profile.";
        return result;
    }

    string history;
    if (!source.historyFile.empty()) {
        if (!ReadBounded(source.historyFile, 256u * 1024u * 1024u, history, result.error)) return result;
        const auto parsed = nlohmann::json::parse(history, nullptr, false);
        if (parsed.is_discarded()) {
            result.error = L"The selected legacy history.json is invalid; no data was imported.";
            return result;
        }
    }

    AtomicFileWriter::WriteOptions options;
    options.keepBackup = false;
    bool historyWritten = false;
    if (!source.historyFile.empty()) {
        const auto write = AtomicFileWriter::WriteText(targetHistoryFile, history, options);
        if (!write.success) {
            result.error = write.error;
            return result;
        }
        historyWritten = true;
    }
    const auto configWrite = AtomicFileWriter::WriteText(targetConfigFile, config, options);
    if (!configWrite.success) {
        if (historyWritten) filesystem::remove(targetHistoryFile, error);
        result.error = configWrite.error;
        return result;
    }
    result.success = true;
    return result;
}
