#include "MineBackupVersion.h"

#include <iostream>
#include <string_view>

namespace {

void PrintHelp() {
	std::cout
		<< "MineBackup headless command line interface\n\n"
		<< "Usage:\n"
		<< "  minebackup-cli [global options] <command>\n\n"
		<< "Commands:\n"
		<< "  doctor\n"
		<< "  config list\n"
		<< "  world list --config <ConfigId>\n"
		<< "  backup --config <ConfigId> --world <relative-path>\n"
		<< "  history list --config <ConfigId> --world <relative-path>\n"
		<< "  run-special <SpecialConfigId>\n\n"
		<< "Global options:\n"
		<< "  --data-dir <path>  --json  --log-level <off|info|debug>\n"
		<< "  --no-network  --non-interactive  --help  --version\n";
}

} // namespace

int main(int argc, char** argv) {
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument(argv[index]);
		if (argument == "--version") {
			std::cout << "minebackup-cli " MINEBACKUP_VERSION_STRING "\n";
			return 0;
		}
		if (argument == "--help" || argument == "-h") {
			PrintHelp();
			return 0;
		}
	}
	if (argc == 1) {
		PrintHelp();
		return 0;
	}
	std::cerr << "minebackup-cli: business commands are not available in this build stage\n";
	return 2;
}
