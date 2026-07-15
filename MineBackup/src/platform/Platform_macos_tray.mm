#include "Platform_macos.h"
#include "AppState.h"
#include "Globals.h"
#include "i18n.h"
#include <GLFW/glfw3.h>
#include <cctype>
#include <cstdint>
#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>

static NSStatusItem* g_statusItem = nil;
static id g_statusHandler = nil;

static EventHotKeyRef g_backupHotKeyRef = nullptr;
static EventHotKeyRef g_restoreHotKeyRef = nullptr;
static bool g_hotkeyHandlerInstalled = false;
static EventHandlerRef g_hotkeyHandlerRef = nullptr;

@interface MBStatusItemHandler : NSObject
- (void)openMain:(id)sender;
- (void)exitApp:(id)sender;
@end

@implementation MBStatusItemHandler
- (void)openMain:(id)sender {
	(void)sender;
	g_appState.showMainApp = true;
	if (wc) {
		[NSApp activateIgnoringOtherApps:YES];
		glfwShowWindow(wc);
		glfwRestoreWindow(wc);
		glfwFocusWindow(wc);
		glfwPostEmptyEvent();
	}
}
- (void)exitApp:(id)sender {
	(void)sender;
	g_appState.done = true;
	glfwPostEmptyEvent();
}
@end

static UInt32 MacKeyCodeFromAscii(int key) {
	switch (std::toupper(static_cast<unsigned char>(key))) {
		case 'A': return kVK_ANSI_A;
		case 'B': return kVK_ANSI_B;
		case 'C': return kVK_ANSI_C;
		case 'D': return kVK_ANSI_D;
		case 'E': return kVK_ANSI_E;
		case 'F': return kVK_ANSI_F;
		case 'G': return kVK_ANSI_G;
		case 'H': return kVK_ANSI_H;
		case 'I': return kVK_ANSI_I;
		case 'J': return kVK_ANSI_J;
		case 'K': return kVK_ANSI_K;
		case 'L': return kVK_ANSI_L;
		case 'M': return kVK_ANSI_M;
		case 'N': return kVK_ANSI_N;
		case 'O': return kVK_ANSI_O;
		case 'P': return kVK_ANSI_P;
		case 'Q': return kVK_ANSI_Q;
		case 'R': return kVK_ANSI_R;
		case 'S': return kVK_ANSI_S;
		case 'T': return kVK_ANSI_T;
		case 'U': return kVK_ANSI_U;
		case 'V': return kVK_ANSI_V;
		case 'W': return kVK_ANSI_W;
		case 'X': return kVK_ANSI_X;
		case 'Y': return kVK_ANSI_Y;
		case 'Z': return kVK_ANSI_Z;
		case '0': return kVK_ANSI_0;
		case '1': return kVK_ANSI_1;
		case '2': return kVK_ANSI_2;
		case '3': return kVK_ANSI_3;
		case '4': return kVK_ANSI_4;
		case '5': return kVK_ANSI_5;
		case '6': return kVK_ANSI_6;
		case '7': return kVK_ANSI_7;
		case '8': return kVK_ANSI_8;
		case '9': return kVK_ANSI_9;
		default: return UINT32_MAX;
	}
}

static OSStatus HotKeyHandler(EventHandlerCallRef, EventRef event, void*) {
	EventHotKeyID hotKeyID{};
	const OSStatus parameterStatus = GetEventParameter(event, kEventParamDirectObject,
		typeEventHotKeyID, nullptr, sizeof(hotKeyID), nullptr, &hotKeyID);
	if (parameterStatus != noErr || hotKeyID.signature != 'MBHK') return eventNotHandledErr;
	if (hotKeyID.id == MINEBACKUP_HOTKEY_ID) {
		TriggerHotkeyBackup();
	} else if (hotKeyID.id == MINERESTORE_HOTKEY_ID) {
		TriggerHotkeyRestore();
	}
	return noErr;
}

static bool EnsureHotkeyHandlerInstalled() {
    if (g_hotkeyHandlerInstalled) return true;
	EventTypeSpec eventType;
	eventType.eventClass = kEventClassKeyboard;
	eventType.eventKind = kEventHotKeyPressed;
	const OSStatus status = InstallEventHandler(GetEventDispatcherTarget(), HotKeyHandler,
		1, &eventType, nullptr, &g_hotkeyHandlerRef);
	g_hotkeyHandlerInstalled = status == noErr && g_hotkeyHandlerRef != nullptr;
	return g_hotkeyHandlerInstalled;
}

static void RemoveHotkeyHandlerIfUnused() {
	if (g_backupHotKeyRef || g_restoreHotKeyRef || !g_hotkeyHandlerRef) return;
	RemoveEventHandler(g_hotkeyHandlerRef);
	g_hotkeyHandlerRef = nullptr;
	g_hotkeyHandlerInstalled = false;
}

bool CreateTrayIcon() {
	if (g_statusItem) return true;
	__block bool created = false;

	// macOS 要求所有 AppKit 操作在主线程执行
	void (^createBlock)(void) = ^{
		@autoreleasepool {
			if (g_statusItem) {
				created = true;
				return;
			}
			[NSApplication sharedApplication];
			NSStatusBar* bar = [NSStatusBar systemStatusBar];
			g_statusItem = [bar statusItemWithLength:NSVariableStatusItemLength];

			// 使用系统图标~
			NSImage* icon = [NSImage imageWithSystemSymbolName:@"archivebox.fill"
									 accessibilityDescription:@"MineBackup"];
			if (icon) {
				// 调整图标大小适配状态栏
				[icon setSize:NSMakeSize(18, 18)];
				icon.template = YES;
				g_statusItem.button.image = icon;
			} else {
				// 回退到文字标题
				g_statusItem.button.title = @"MB";
			}
			g_statusItem.button.toolTip = @"MineBackup";

			g_statusHandler = [[MBStatusItemHandler alloc] init];
			NSMenu* menu = [[NSMenu alloc] init];

			NSString* openTitle = [NSString stringWithUTF8String:L("OPEN")];
			NSString* exitTitle = [NSString stringWithUTF8String:L("EXIT")];

			NSMenuItem* openItem = [[NSMenuItem alloc] initWithTitle:openTitle action:@selector(openMain:) keyEquivalent:@""];
			[openItem setTarget:g_statusHandler];
			[menu addItem:openItem];

			NSMenuItem* exitItem = [[NSMenuItem alloc] initWithTitle:exitTitle action:@selector(exitApp:) keyEquivalent:@""];
			[exitItem setTarget:g_statusHandler];
			[menu addItem:exitItem];

			g_statusItem.menu = menu;
			created = g_statusItem != nil;
		}
	};

	if ([NSThread isMainThread]) {
		createBlock();
	} else {
		dispatch_sync(dispatch_get_main_queue(), createBlock);
	}
	return created;
}

void RemoveTrayIcon() {
	void (^removeBlock)(void) = ^{
		@autoreleasepool {
			if (!g_statusItem) return;
			[[NSStatusBar systemStatusBar] removeStatusItem:g_statusItem];
			g_statusItem = nil;
			g_statusHandler = nil;
		}
	};

	if ([NSThread isMainThread]) {
		removeBlock();
	} else {
		dispatch_sync(dispatch_get_main_queue(), removeBlock);
	}
}

static bool RegisterHotkeyOnMainThread(int hotkeyId, int key) {
	UInt32 keyCode = MacKeyCodeFromAscii(key);
	if (keyCode == UINT32_MAX) return false;
	EventHotKeyRef* ref = hotkeyId == MINEBACKUP_HOTKEY_ID ? &g_backupHotKeyRef
		: hotkeyId == MINERESTORE_HOTKEY_ID ? &g_restoreHotKeyRef : nullptr;
	if (!ref) return false;
	if (!EnsureHotkeyHandlerInstalled()) return false;
	if (*ref) {
		UnregisterEventHotKey(*ref);
		*ref = nullptr;
	}
	EventHotKeyID hotKeyID;
	hotKeyID.signature = 'MBHK';
	hotKeyID.id = hotkeyId;
	const OSStatus status = RegisterEventHotKey(keyCode, controlKey | optionKey,
		hotKeyID, GetEventDispatcherTarget(), 0, ref);
	if (status != noErr) {
		*ref = nullptr;
		RemoveHotkeyHandlerIfUnused();
		return false;
	}
	return true;
}

bool RegisterHotkeys(int hotkeyId, int key) {
	if ([NSThread isMainThread]) return RegisterHotkeyOnMainThread(hotkeyId, key);
	__block bool registered = false;
	dispatch_sync(dispatch_get_main_queue(), ^{
		registered = RegisterHotkeyOnMainThread(hotkeyId, key);
	});
	return registered;
}

static void UnregisterHotkeyOnMainThread(int hotkeyId) {
	EventHotKeyRef* ref = hotkeyId == MINEBACKUP_HOTKEY_ID ? &g_backupHotKeyRef
		: hotkeyId == MINERESTORE_HOTKEY_ID ? &g_restoreHotKeyRef : nullptr;
	if (!ref) return;
	if (*ref) {
		UnregisterEventHotKey(*ref);
		*ref = nullptr;
	}
	RemoveHotkeyHandlerIfUnused();
}

void UnregisterHotkeys(int hotkeyId) {
	if ([NSThread isMainThread]) {
		UnregisterHotkeyOnMainThread(hotkeyId);
		return;
	}
	dispatch_sync(dispatch_get_main_queue(), ^{
		UnregisterHotkeyOnMainThread(hotkeyId);
	});
}
