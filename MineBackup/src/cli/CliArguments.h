#pragma once

#include "CliTypes.h"

CliParseResult ParseCliArguments(const std::vector<std::wstring>& arguments);
std::string CliCommandName(CliCommand command);
void PrintCliHelp();
