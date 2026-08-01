#include "MainUiController.h"

namespace {
	MainUiController controller;
}

void MainUiController::ResetForClose() {
	showAboutWindow = false;
	showImportConfigConfirm = false;
	showImportHistoryConfirm = false;
	pendingImportPath.clear();
	waitingForHotkey = false;
	worldList.ResetSelection();
}

MainUiController& GetMainUiController() {
	return controller;
}
