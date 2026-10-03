#include "Globals.h"

namespace { AppGlobalState state; }
AppWindowState& WindowState() { return state.window; }
AppAppearanceState& AppearanceState() { return state.appearance; }
AppUpdateState& UpdateState() { return state.update; }
KnotLinkRuntimeState& KnotLinkState() { return state.knotlink; }
AppUiState& UiState() { return state.ui; }
AppSettingsState& SettingsState() { return state.settings; }
CoreValidationRuntimeState& CoreValidationState() { return state.coreValidation; }
ExternalToolRuntimeState& ExternalToolState() { return state.externalTools; }
const std::string& ApplicationVersion() { return state.currentVersion; }
