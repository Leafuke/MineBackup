#pragma once
#ifndef GLOBALS_H
#define GLOBALS_H

// 全局变量集中声明
// 定义在 MineBackup.cpp 中，其他文件通过 #include "Globals.h" 访问

#include "AppState.h"
#include <string>
#include <vector>
#include <atomic>

// 前向声明
struct GLFWwindow;
struct ImVec4;

// GLFW 窗口
extern GLFWwindow* wc;

// 版本信息
extern std::string CURRENT_VERSION;

// 更新/通知检查状态
extern std::atomic<bool> g_UpdateCheckDone;
extern std::atomic<bool> g_NewVersionAvailable;
extern std::atomic<bool> g_NoticeCheckDone;
extern std::atomic<bool> g_NewNoticeAvailable;
extern std::string g_LatestVersionStr;
extern std::string g_ReleaseNotes;
extern std::string g_NoticeContent;
extern std::string g_NoticeUpdatedAt;
extern std::string g_NoticeLastSeenVersion;

// 窗口尺寸与 UI 缩放
extern int g_windowWidth, g_windowHeight;
extern float g_uiScale;

// 自动备份间隔
extern int last_interval;

// 设置项变量
extern ImVec4 clear_color;
extern std::wstring Fontss;
extern bool showSettings;
extern bool isSilence, isSafeDelete;
extern bool specialSetting;
extern bool g_CheckForUpdates, g_ReceiveNotices, g_StopAutoBackupOnExit;
extern bool g_RunOnStartup, g_AutoScanForWorlds, g_autoLogEnabled;
extern bool showHistoryWindow;
extern bool g_enableKnotLink;
extern int g_hotKeyBackupId, g_hotKeyRestoreId;

// 关闭行为设置
extern int g_closeAction;
extern bool g_rememberCloseAction;
extern bool g_showCloseConfirmDialog;

// 特殊模式控制
extern std::atomic<bool> specialTasksRunning;
extern std::atomic<bool> specialTasksComplete;
extern std::thread g_exitWatcherThread;
extern std::atomic<bool> g_stopExitWatcher;

// 历史窗口
extern std::wstring g_worldToFocusInHistory;

// 还原白名单
extern std::vector<std::wstring> restoreWhitelist;

// i18n
extern const char* lang_codes[2];
extern const char* langs[2];

#endif // GLOBALS_H
