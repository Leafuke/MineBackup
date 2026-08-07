#pragma once

#include "TestSupport.h"

#include <filesystem>

void RunSpecialTaskDocumentTests(
	TestContext& test,
	const std::filesystem::path& temporaryRoot);
