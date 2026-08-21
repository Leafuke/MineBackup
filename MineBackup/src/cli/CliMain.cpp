#include "CliApplication.h"

#include "text_to_text.h"

#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>

namespace {

bool IsConsoleHandle(HANDLE handle) {
	if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return false;

	DWORD mode = 0;
	return GetConsoleMode(handle, &mode) != FALSE;
}

void ConfigureCliConsoleEncoding() {
	// CliApplication emits UTF-8 JSON through std::cout/std::cerr. Change the
	// Windows console code page only for attached consoles; redirected output
	// must remain raw UTF-8 for scripts and pipes.
	if (IsConsoleHandle(GetStdHandle(STD_OUTPUT_HANDLE)) ||
		IsConsoleHandle(GetStdHandle(STD_ERROR_HANDLE))) {
		(void)SetConsoleOutputCP(CP_UTF8);
	}
}

} // namespace

int wmain(int argc, wchar_t** argv) {
	ConfigureCliConsoleEncoding();

	std::vector<std::wstring> arguments;
	for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
	return RunMineBackupCli(arguments);
}
#else
int main(int argc, char** argv) {
	std::vector<std::wstring> arguments;
	for (int index = 1; index < argc; ++index) {
		arguments.push_back(utf8_to_wstring(argv[index]));
	}
	return RunMineBackupCli(arguments);
}
#endif
