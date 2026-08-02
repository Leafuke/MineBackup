#pragma once

#include "TestSupport.h"

#include <filesystem>

void RunProcessToolTests(
	TestContext& test,
	const std::filesystem::path& executable,
	const std::filesystem::path& root);
