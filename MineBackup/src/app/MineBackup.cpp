#include "Application.h"
#include "text_to_text.h"

#include <string>
#include <vector>

#ifdef _WIN32
#include "PlatformCompat.h"
#include <shellapi.h>

int WINAPI WinMain(
	_In_ HINSTANCE instance,
	_In_opt_ HINSTANCE,
	_In_ LPSTR,
	_In_ int showCommand) {
	int argumentCount = 0;
	wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
	ApplicationEntryContext context;
	if (arguments) {
		context.arguments.assign(arguments, arguments + argumentCount);
		LocalFree(arguments);
	}
	context.nativeInstance = reinterpret_cast<void*>(instance);
	context.showCommand = showCommand;
	return RunApplication(context);
}
#else
int main(int argc, char** argv) {
	ApplicationEntryContext context;
	context.arguments.reserve(static_cast<std::size_t>(argc));
	context.arguments.push_back(L"MineBackup");
	for (int index = 1; index < argc; ++index) {
		if (argv[index]) context.arguments.push_back(utf8_to_wstring(argv[index]));
	}
	return RunApplication(context);
}
#endif
