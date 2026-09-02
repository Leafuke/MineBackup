#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace LauncherDiscoveryUtils {

// 启动器配置文件读取上限（统一限制 1 MiB）
constexpr std::uintmax_t MaximumConfigBytes = 1024u * 1024u;

// 有界文本文件读取状态
enum class BoundedTextFileStatus {
	Loaded,
	Missing,
	NotRegular,
	TooLarge,
	ReadFailed
};

// 有界文本文件读取结果
struct BoundedTextFileResult {
	BoundedTextFileStatus status = BoundedTextFileStatus::Missing;
	std::string contents;
	std::error_code filesystemError;
};

// 读取环境变量并转换为路径（Windows 下使用宽字符 API，避免 ANSI 代码页字符损坏）
std::optional<std::filesystem::path>
ReadEnvironmentPath(std::string_view name);

// 解析当前用户主目录（HOME / USERPROFILE）
std::optional<std::filesystem::path>
ResolveHomeDirectory();

// 解析各操作系统的标准用户数据根目录
// Windows: %APPDATA%
// macOS: ~/Library/Application Support
// Linux: $XDG_DATA_HOME (仅绝对路径) 或 ~/.local/share
std::optional<std::filesystem::path>
ResolveUserDataRoot();

// 安全读取指定路径的有界文本文件（最大不超过 MaximumConfigBytes）
BoundedTextFileResult
ReadBoundedTextFile(const std::filesystem::path& path);

// 判断路径是否为目录，填充 error_code
bool IsDirectory(
	const std::filesystem::path& path,
	std::error_code& error);

// 判断路径是否为常规文件，填充 error_code
bool IsRegularFile(
	const std::filesystem::path& path,
	std::error_code& error);

// 非递归列出指定根目录下的直接子目录（一层深度），并进行稳定排序
std::vector<std::filesystem::path>
ListChildDirectories(
	const std::filesystem::path& root,
	std::error_code& error);

#ifdef _WIN32
// 展开 Windows 环境变量字符串（如 %USERPROFILE%\foo -> C:\Users\user\foo）
std::optional<std::wstring> ExpandEnvironmentString(std::wstring_view input);
#endif

// 判断 filesystem 错误是否属于文件/路径不存在
bool IsMissingFilesystemError(std::error_code error);

} // namespace LauncherDiscoveryUtils
