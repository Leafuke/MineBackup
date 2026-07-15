#pragma once

#include "DataModels.h"

#include <map>
#include <optional>

struct SpecialConfigPolicyResult {
    std::optional<int> autoExecuteIndex;
    std::optional<int> runOnStartupIndex;
    int disabledDuplicateAutoExecute = 0;
    int disabledDuplicateRunOnStartup = 0;
};

[[nodiscard]] SpecialConfigPolicyResult NormalizeSpecialConfigExecutionPolicy(
    std::map<int, SpecialConfig>& specialConfigs);
void SetExclusiveSpecialAutoExecute(
    std::map<int, SpecialConfig>& specialConfigs, int selectedIndex, bool enabled);
void SetExclusiveSpecialRunOnStartup(
    std::map<int, SpecialConfig>& specialConfigs, int selectedIndex, bool enabled);
[[nodiscard]] std::optional<int> FindSpecialRunOnStartup(
    const std::map<int, SpecialConfig>& specialConfigs);
