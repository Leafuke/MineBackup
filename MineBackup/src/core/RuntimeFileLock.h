#pragma once

#include <filesystem>

bool IsRuntimeFileLocked(const std::filesystem::path& path);
bool IsRuntimeWorldOccupied(const std::filesystem::path& worldPath);
