#pragma once

#include "PortableConfigDocument.h"

#include <filesystem>
#include <string>

namespace V15MigrationAdapter {

// Installs the MineBackup 1.15 compatibility callbacks into MigrationCoordinator.
// This symbol and its implementation are absent when v1.15 migration is disabled.
void Install();

// Explicit, read-only bridge for a user-selected legacy remote config.ini.
// It snapshots the source and returns only the portable whitelist model.
bool ImportLegacyRemoteIni(
	const std::filesystem::path& source,
	PortableConfigDocument& document,
	std::wstring& error);

} // namespace V15MigrationAdapter
