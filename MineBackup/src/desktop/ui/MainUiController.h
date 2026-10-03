#pragma once

#include "WorldListController.h"

#include <string>

struct MainUiController {
	bool showAboutWindow = false;
	bool showImportConfigConfirm = false;
	bool showImportHistoryConfirm = false;
	std::wstring pendingImportPath;
	bool waitingForHotkey = false;
	int selectedNoticeAction = 0;
	bool openUpdatePopup = false;
	bool noticePopupOpened = false;
	bool noticeSnoozedThisSession = false;
	bool rememberNoticeChoice = false;
	bool firstDockLayout = true;
	WorldListController worldList;

	void ResetForClose();
};

MainUiController& GetMainUiController();
