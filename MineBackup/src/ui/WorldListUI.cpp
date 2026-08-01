#include "MainUI.h"

#include "AppState.h"
#include "Globals.h"
#include "WorldListModel.h"

#include <mutex>

std::vector<DisplayWorld> BuildDisplayWorldsForSelection() {
	std::lock_guard<std::mutex> lock(g_appState.configsMutex);
	return BuildDisplayWorlds(
		g_appState.configs,
		g_appState.specialConfigs,
		g_appState.currentConfigIndex,
		specialSetting);
}
