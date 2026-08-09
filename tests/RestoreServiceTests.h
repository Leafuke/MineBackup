#pragma once

#include "TestSupport.h"

#include <filesystem>

void RunRestoreServiceTests(
	TestContext& test,
	const std::filesystem::path& temporaryRoot);
