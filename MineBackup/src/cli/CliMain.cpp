#include "CliApplication.h"

#include "text_to_text.h"

#include <string>
#include <vector>

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
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
