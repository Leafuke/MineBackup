#pragma once

#include "TestSupport.h"

#include <filesystem>

void RunProfileConfigCatalogTests(
	TestContext& test,
	const std::filesystem::path& temporaryRoot);

