#pragma once

#include <filesystem>

bool IsRuntimeFileLocked(const std::filesystem::path& path);
