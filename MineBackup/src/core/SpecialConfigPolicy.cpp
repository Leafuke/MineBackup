#include "SpecialConfigPolicy.h"

using namespace std;

SpecialConfigPolicyResult NormalizeSpecialConfigExecutionPolicy(
    map<int, SpecialConfig>& specialConfigs) {
    SpecialConfigPolicyResult result;
    for (auto& [index, config] : specialConfigs) {
        if (config.autoExecute) {
            if (!result.autoExecuteIndex) result.autoExecuteIndex = index;
            else {
                config.autoExecute = false;
                ++result.disabledDuplicateAutoExecute;
            }
        }
        if (config.runOnStartup) {
            if (!result.runOnStartupIndex) result.runOnStartupIndex = index;
            else {
                config.runOnStartup = false;
                ++result.disabledDuplicateRunOnStartup;
            }
        }
    }
    return result;
}

void SetExclusiveSpecialAutoExecute(
    map<int, SpecialConfig>& specialConfigs, int selectedIndex, bool enabled) {
    for (auto& [index, config] : specialConfigs) {
        config.autoExecute = enabled && index == selectedIndex;
    }
}

void SetExclusiveSpecialRunOnStartup(
    map<int, SpecialConfig>& specialConfigs, int selectedIndex, bool enabled) {
    for (auto& [index, config] : specialConfigs) {
        config.runOnStartup = enabled && index == selectedIndex;
    }
}

optional<int> FindSpecialRunOnStartup(const map<int, SpecialConfig>& specialConfigs) {
    for (const auto& [index, config] : specialConfigs) {
        if (config.runOnStartup) return index;
    }
    return nullopt;
}
