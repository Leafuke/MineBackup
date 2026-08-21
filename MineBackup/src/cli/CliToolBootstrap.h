#pragma once

#include "AppPaths.h"

#include <stop_token>
#include <string>

bool EnsureCliSevenZip(
	const AppPaths& paths,
	std::stop_token stopToken,
	std::wstring& error);
