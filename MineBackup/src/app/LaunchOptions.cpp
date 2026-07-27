#include "LaunchOptions.h"

#include <stdexcept>

using namespace std;

namespace {

bool ReadValue(const vector<wstring>& arguments, size_t& index, const wchar_t* option, wstring& value, wstring& error) {
    if (index + 1 >= arguments.size() || arguments[index + 1].empty()) {
        error = wstring(option) + L" requires a value.";
        return false;
    }
    value = arguments[++index];
    return true;
}

} // namespace

bool ParseLaunchOptions(const vector<wstring>& arguments, LaunchOptions& options, wstring& error) {
    options = {};
    error.clear();
    for (size_t index = 1; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        wstring value;
        if (argument == L"--data-dir") {
            if (!ReadValue(arguments, index, L"--data-dir", value, error)) return false;
            options.dataDirectory = filesystem::path(value);
        }
        else if (argument == L"--autostart") options.autostart = true;
        else if (argument == L"--silent-startup" || argument == L"-silentstartup") options.silentStartup = true;
        else if (argument == L"--select-config") {
            if (!ReadValue(arguments, index, L"--select-config", options.selectConfigId, error)) return false;
        }
        else if (argument == L"--run-special") {
            if (!ReadValue(arguments, index, L"--run-special", options.runSpecialId, error)) return false;
        }
        else if (argument == L"--cleanup-legacy-service") {
            if (!ReadValue(arguments, index, L"--cleanup-legacy-service", options.legacyServiceCleanup, error)) return false;
        }
        else if (argument == L"-specialcfg") {
            if (!ReadValue(arguments, index, L"-specialcfg", value, error)) return false;
            try {
                size_t consumed = 0;
                const int parsed = stoi(value, &consumed);
                if (consumed != value.size() || parsed < 0) throw invalid_argument("invalid index");
                options.legacySpecialConfigIndex = parsed;
            }
            catch (...) {
                error = L"-specialcfg requires a non-negative numeric configuration index.";
                return false;
            }
        }
        else if (argument == L"--service") options.legacyServiceMode = true;
        else {
            error = L"Unknown launch option: " + argument;
            return false;
        }
    }
    const bool hasProfileLaunchOptions = options.dataDirectory.has_value()
        || options.autostart || options.silentStartup
        || !options.selectConfigId.empty() || !options.runSpecialId.empty()
        || options.legacySpecialConfigIndex.has_value();
    if (!options.legacyServiceCleanup.empty()
        && (hasProfileLaunchOptions || options.legacyServiceMode)) {
        error = L"--cleanup-legacy-service must be used by itself.";
        return false;
    }
    if (options.legacyServiceMode && hasProfileLaunchOptions) {
        error = L"--service must be used by itself.";
        return false;
    }
    return true;
}
