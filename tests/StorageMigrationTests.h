#pragma once

#include "TestSupport.h"

#include <filesystem>

void RunStorageMigrationTests(TestContext& test, const std::filesystem::path& root);
