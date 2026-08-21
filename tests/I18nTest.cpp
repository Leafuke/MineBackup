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
        "LOG_EXPORT_DIAGNOSTICS_WARNING",
        "SETTINGS_MINECRAFT_TITLE",
        "SETTINGS_MINECRAFT_DESC",
        "SETTINGS_MINECRAFT_RESCAN",
        "SETTINGS_MINECRAFT_SCANNING",
        "SETTINGS_MINECRAFT_CHECKING",
        "SETTINGS_MINECRAFT_EMPTY",
        "SETTINGS_MINECRAFT_NEW",
        "SETTINGS_MINECRAFT_ADDED",
        "SETTINGS_MINECRAFT_ALREADY_ADDED",
        "SETTINGS_MINECRAFT_ADDED_SUCCESS",
        "SETTINGS_MINECRAFT_TASK_FAILED",
        "SETTINGS_MINECRAFT_SELECT_REQUIRED",
        "SETTINGS_MINECRAFT_COMMIT_FAILED",
        "SETTINGS_MINECRAFT_ADD_SELECTED",
        "WIZARD_DISCOVER_TITLE",
        "WIZARD_DISCOVER_DESC",
        "WIZARD_RESCAN",
        "WIZARD_MANUAL_ADD",
        "WIZARD_SCANNING",
        "WIZARD_DISCOVERY_EMPTY",
        "WIZARD_PCL2_HINT",
        "WIZARD_ADVANCED_CUSTOM_DESC",
        "WIZARD_ADVANCED_CUSTOM",
        "WIZARD_ADVANCED_CUSTOM_INVALID",
        "WIZARD_SOURCE",
        "WIZARD_SOURCE_KNOWN",
        "WIZARD_SOURCE_MANUAL",
        "WIZARD_SOURCE_PCL2",
        "WIZARD_WORLD_COUNT",
        "WIZARD_ALREADY_ADDED",
        "WIZARD_SELECT_AT_LEAST_ONE",
        "WIZARD_BACKUP_TITLE",
        "WIZARD_BACKUP_DESC",
        "WIZARD_BACKUP_ABSOLUTE_REQUIRED",
        "WIZARD_BACKUP_PREVIEW",
        "WIZARD_READY_TITLE",
        "WIZARD_READY_DESC",
        "WIZARD_READY_OK",
        "WIZARD_READINESS_CHECKING",
        "WIZARD_FINAL_CHECKING",
        "WIZARD_CORE_VALIDATION_TITLE",
        "WIZARD_CORE_VALIDATION_RUNNING",
        "WIZARD_CORE_VALIDATION_START_FAILED",
        "WIZARD_CORE_VALIDATION_PASSED",
        "WIZARD_CORE_VALIDATION_FAILED",
        "WIZARD_CORE_VALIDATION_FAILED_DESC",
        "WIZARD_OPEN_LOGS",
        "WIZARD_OPEN_SETTINGS",
        "WIZARD_ENTER_MAIN",
        "WIZARD_TASK_SUBMIT_FAILED",
        "WIZARD_COMMIT_FAILED",
        "WIZARD_READINESS_READINESS_CANCELLED",
        "WIZARD_READINESS_READINESS_EMPTY_BATCH",
        "WIZARD_READINESS_READINESS_UNEXPECTED_FAILURE",
        "WIZARD_READINESS_SEVEN_ZIP_UNAVAILABLE",
        "WIZARD_READINESS_SOURCE_MISSING",
        "WIZARD_READINESS_SOURCE_NOT_DIRECTORY",
        "WIZARD_READINESS_SOURCE_ALREADY_CONFIGURED",
        "WIZARD_READINESS_SOURCE_DUPLICATE_IN_BATCH",
        "WIZARD_READINESS_SOURCE_NO_WORLDS",
        "WIZARD_READINESS_WORLD_RELATIVE_PATH_UNSAFE",
        "WIZARD_READINESS_WORLD_MISSING",
        "WIZARD_READINESS_JAVA_WORLD_INVALID",
        "WIZARD_READINESS_BEDROCK_WORLD_INVALID",
        "WIZARD_READINESS_BACKUP_PATH_NOT_ABSOLUTE",
        "WIZARD_READINESS_BACKUP_PATH_EXISTING_COLLISION",
        "WIZARD_READINESS_BACKUP_PATH_BATCH_COLLISION",
        "WIZARD_READINESS_BACKUP_INSIDE_SOURCE",
        "WIZARD_READINESS_BACKUP_INSIDE_WORLD",
        "WIZARD_READINESS_BACKUP_WRITE_PROBE_FAILED",
        "WIZARD_READINESS_STORAGE_IDENTITY_COLLISION"
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
        const auto minecraftCount = MineFormatMessage(
            "SETTINGS_MINECRAFT_ADDED_SUCCESS", static_cast<std::size_t>(2));
        Check(!stringAndInteger.empty()
                && stringAndInteger.find(L"%d") == std::wstring::npos
                && stringAndInteger.find(L"%s") == std::wstring::npos,
            std::string(language)
                + " printf translation should format string/integer arguments");
        Check(!sizeValue.empty()
                && sizeValue.find(L"%zu") == std::wstring::npos,
            std::string(language)
                + " printf translation should format size_t arguments");
        Check(!minecraftCount.empty()
				&& minecraftCount.find(L"%zu") == std::wstring::npos,
			std::string(language)
				+ " Minecraft result translation should format size_t arguments");
    }

    if (failures == 0) {
        std::cout << "[PASS] MineBackup i18n tables\n";
        return 0;
    }
    std::cerr << failures << " i18n assertion(s) failed\n";
    return 1;
}
