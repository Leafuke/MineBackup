#include "i18n.h"

#include <array>
#include <iostream>
#include <string>
#include <unordered_set>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[FAIL] " << message << '\n';
}

} // namespace

int main() {
    const auto zh = g_LangTable.find("zh_CN");
    const auto en = g_LangTable.find("en_US");
    Check(zh != g_LangTable.end(), "zh_CN translation table is missing");
    Check(en != g_LangTable.end(), "en_US translation table is missing");
    if (zh == g_LangTable.end() || en == g_LangTable.end()) return 1;

    Check(zh->second.size() == en->second.size(),
        "Chinese and English translation tables have different sizes");
    for (const auto& [key, value] : zh->second) {
        Check(!value.value.empty(), "zh_CN translation is empty: " + key);
        Check(en->second.contains(key), "en_US translation is missing: " + key);
    }
    for (const auto& [key, value] : en->second) {
        Check(!value.value.empty(), "en_US translation is empty: " + key);
        Check(zh->second.contains(key), "zh_CN translation is missing: " + key);
    }

    constexpr std::array criticalKeys = {
        "BUTTON_CONFIRM",
        "DIALOG_SELECT_FOLDER_TITLE",
        "DIALOG_SELECT_FILE_TITLE",
        "DIALOG_SAVE_FILE_TITLE",
        "CAPABILITIES_HEADER",
        "SETTINGS_CATEGORY_APPLICATION",
        "DEFAULT_BACKUP_ROOT_TITLE",
        "DEFAULT_BACKUP_ROOT_DESCRIPTION",
        "BUTTON_RESTORE_RECOMMENDED",
        "MIGRATION_STARTUP_TITLE",
        "PORTABLE_BINDING_NOTICE",
        "TASK_COMMAND_WARNING",
        "RCLONE_INSTALL_BUTTON",
        "CLOUD_HISTORY_IMPORT_SUCCEEDED",
        "CLOUD_STATUS_DOWNLOADING_HISTORY",
        "TAB_LOG_PANEL",
        "TAB_COMMAND_CONSOLE",
        "LOG_EXPORT_DIAGNOSTICS",
        "LOG_EXPORT_DIAGNOSTICS_WARNING"
    };
    for (const char* language : lang_codes) {
        SetLanguage(language);
        for (const char* key : criticalKeys) {
            Check(std::string(L(key)) != key,
                std::string(language) + " translation is missing or empty: " + key);
        }
        const auto stringAndInteger = MineFormatMessage(
            "CONFIRM_DELETE_MSG", 7, "Profile");
        const auto sizeValue = MineFormatMessage(
            "LOG_BACKUP_SMART_INFO", static_cast<std::size_t>(42));
        Check(!stringAndInteger.empty()
                && stringAndInteger.find(L"%d") == std::wstring::npos
                && stringAndInteger.find(L"%s") == std::wstring::npos,
            std::string(language)
                + " printf translation should format string/integer arguments");
        Check(!sizeValue.empty()
                && sizeValue.find(L"%zu") == std::wstring::npos,
            std::string(language)
                + " printf translation should format size_t arguments");
    }

    if (failures == 0) {
        std::cout << "[PASS] MineBackup i18n tables\n";
        return 0;
    }
    std::cerr << failures << " i18n assertion(s) failed\n";
    return 1;
}
