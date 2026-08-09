#pragma once

#include "TestSupport.h"

#include <filesystem>

void RunProfileManifestTests(
	TestContext& test,
	const std::filesystem::path& temporaryRoot);
