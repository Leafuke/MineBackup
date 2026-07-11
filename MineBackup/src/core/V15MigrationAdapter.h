#pragma once

namespace V15MigrationAdapter {

// Installs the MineBackup 1.15 compatibility callbacks into MigrationCoordinator.
// This symbol and its implementation are absent when v1.15 migration is disabled.
void Install();

} // namespace V15MigrationAdapter
