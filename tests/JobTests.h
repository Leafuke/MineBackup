#pragma once

#include "TestSupport.h"

#include <filesystem>

void RunJobTests(TestContext& test, const std::filesystem::path& temporaryRoot);
