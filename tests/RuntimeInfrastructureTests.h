#pragma once

#include "TestSupport.h"

#include <filesystem>

void RunRuntimeInfrastructureTests(
	TestContext& test,
	const std::filesystem::path& root);
