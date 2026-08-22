#pragma once

#include "TestSupport.h"

#include <filesystem>

void RunPathIdentityTests(
	TestContext& test,
	const std::filesystem::path& temporaryRoot);
