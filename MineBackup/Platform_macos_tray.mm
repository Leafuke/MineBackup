#include "Platform_macos.h"
#include "AppState.h"
#include "i18n.h"
#include <GLFW/glfw3.h>
#include <cctype>
#include <cstdint>
#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>

extern GLFWwindow* wc;
extern AppState g_appState;

static NSStatusItem* g_statusItem = nil;
static id g_statusHandler = nil;

static EventHotKeyRef g_backupHotKeyRef = nullptr;
static EventHotKeyRef g_restoreHotKeyRef = nullptr;
static bool g_hotkeyHandlerInstalled = false;

@interface MBStatusItemHandler : NSObject
- (void)openMain:(id)sender;
- (void)exitApp:(id)sender;
@end

@implementation MBStatusItemHandler
- (void)openMain:(id)sender {
	(void)sender;
	g_appState.showMainApp = true;
	if (wc) {
		glfwShowWindow(wc);
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
	switch (std::toupper(key)) {
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
	EventHotKeyID hotKeyID;
	GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof(hotKeyID), nullptr, &hotKeyID);
	if (hotKeyID.id == MINEBACKUP_HOTKEY_ID) {
		TriggerHotkeyBackup();
	} else if (hotKeyID.id == MINERESTORE_HOTKEY_ID) {
		TriggerHotkeyRestore();
	}
	return noErr;
}

static void EnsureHotkeyHandlerInstalled() {
	if (g_hotkeyHandlerInstalled) return;
	EventTypeSpec eventType;
	eventType.eventClass = kEventClassKeyboard;
	eventType.eventKind = kEventHotKeyPressed;
	InstallEventHandler(GetEventDispatcherTarget(), HotKeyHandler, 1, &eventType, nullptr, nullptr);
	g_hotkeyHandlerInstalled = true;
}

void CreateTrayIcon() {
	if (g_statusItem) return;
	@autoreleasepool {
		NSStatusBar* bar = [NSStatusBar systemStatusBar];
		g_statusItem = [bar statusItemWithLength:NSVariableStatusItemLength];
		g_statusItem.button.title = @"MineBackup";

		g_statusHandler = [[MBStatusItemHandler alloc] init];
		NSMenu* menu = [[NSMenu alloc] init];

		NSString* openTitle = [NSString stringWithUTF8String:"OPEN"];
		NSString* exitTitle = [NSString stringWithUTF8String:"EXIT"];

		NSMenuItem* openItem = [[NSMenuItem alloc] initWithTitle:openTitle action:@selector(openMain:) keyEquivalent:@""];
		[openItem setTarget:g_statusHandler];
		[menu addItem:openItem];

		NSMenuItem* exitItem = [[NSMenuItem alloc] initWithTitle:exitTitle action:@selector(exitApp:) keyEquivalent:@""];
		[exitItem setTarget:g_statusHandler];
		[menu addItem:exitItem];

		g_statusItem.menu = menu;
	}
}

void RemoveTrayIcon() {
	@autoreleasepool {
		if (!g_statusItem) return;
		[[NSStatusBar systemStatusBar] removeStatusItem:g_statusItem];
		g_statusItem = nil;
		g_statusHandler = nil;
	}
}

void RegisterHotkeys(int hotkeyId, int key) {
	EnsureHotkeyHandlerInstalled();
	UInt32 keyCode = MacKeyCodeFromAscii(key);
	if (keyCode == UINT32_MAX) return;
	EventHotKeyRef* ref = (hotkeyId == MINEBACKUP_HOTKEY_ID) ? &g_backupHotKeyRef : &g_restoreHotKeyRef;
	if (*ref) {
		UnregisterEventHotKey(*ref);
		*ref = nullptr;
	}
	EventHotKeyID hotKeyID;
	hotKeyID.signature = 'MBHK';
	hotKeyID.id = hotkeyId;
	RegisterEventHotKey(keyCode, controlKey | optionKey, hotKeyID, GetEventDispatcherTarget(), 0, ref);
}

void UnregisterHotkeys(int hotkeyId) {
	EventHotKeyRef* ref = (hotkeyId == MINEBACKUP_HOTKEY_ID) ? &g_backupHotKeyRef : &g_restoreHotKeyRef;
	if (*ref) {
		UnregisterEventHotKey(*ref);
		*ref = nullptr;
	}
}
