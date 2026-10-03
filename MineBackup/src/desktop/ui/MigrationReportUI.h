#pragma once

#include "DataModels.h"

#include <string>

namespace MigrationReportUI {
const char* StatusLabel(MigrationStatus status);
std::string UnitLabel(const std::wstring& unitId);
void DrawSettings();
}
