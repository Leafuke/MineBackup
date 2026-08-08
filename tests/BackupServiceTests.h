#pragma once

#include "TestSupport.h"

#include <filesystem>

void RunBackupServiceTests(
	TestContext& test,
	const std::filesystem::path& temporaryRoot);

