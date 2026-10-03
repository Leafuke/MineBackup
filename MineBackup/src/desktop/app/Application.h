#pragma once

#include <string>
#include <vector>

struct ApplicationEntryContext {
	std::vector<std::wstring> arguments;
	void* nativeInstance = nullptr;
	int showCommand = 0;
};

int RunApplication(const ApplicationEntryContext& context);
