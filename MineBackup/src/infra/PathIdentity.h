#pragma once

#include <filesystem>
#include <string>

namespace PathIdentity {

// 统一处理已存在路径和即将创建路径，供发现、配置和安全校验共用。
std::filesystem::path NormalizeExistingOrProspectivePath(
	const std::filesystem::path& path);

std::wstring BuildPathIdentityKey(const std::filesystem::path& path);

bool PathsEqual(
	const std::filesystem::path& left,
	const std::filesystem::path& right);

// 按路径组件比较，避免把 foo 误判成 foo-bar 或 foobar 的父目录。
bool IsEqualOrDescendant(
	const std::filesystem::path& candidate,
	const std::filesystem::path& ancestor);

} // namespace PathIdentity
