#include "SettingsUIHotkeys.h"

int ImGuiKeyToPlatformHotkey(ImGuiKey key) {
	if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
		return 'A' + (key - ImGuiKey_A);
	}
	if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
		return '0' + (key - ImGuiKey_0);
	}
	return 0;
}
