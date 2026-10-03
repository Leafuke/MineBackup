#include "WorldListController.h"

void WorldListController::ResetSelection() {
	selectedWorldIndex = -1;
	backupComment[0] = '\0';
	showAddConfigPopup = false;
	showDeleteConfigPopup = false;
}

void WorldListController::Invalidate() {
	cachedConfigIndex = -999;
	cachedWorldCount = 0;
	lastDisplayWorldsRefresh = {};
	lastTimeCacheRefresh = {};
}
