#include "PathIdentity.h"

#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace PathIdentity {
namespace {

#ifdef _WIN32

// Windows 路径大小写归一化的唯一入口。
// 使用 LOCALE_NAME_INVARIANT + LCMAP_UPPERCASE（不带 LCMAP_LINGUISTIC_CASING），
// 即操作系统文件系统所用的大小写表，与用户 locale 完全无关；
// std::towlower 受进程 C locale 影响（中文等环境下仅覆盖 ASCII），必须弃用。
std::wstring FoldWindowsPathCase(std::wstring_view value) {
	// 两阶段：先查询所需缓冲区大小，再执行映射。
	// 大小写映射可能扩展字符串（如 ß -> SS），不能假设输出长度等于输入长度。
	const int sourceLength = static_cast<int>(value.size());
	const int required = LCMapStringEx(
		LOCALE_NAME_INVARIANT,
		LCMAP_UPPERCASE,
		value.data(),
		sourceLength,
		nullptr,
		0,
		nullptr,
		nullptr,
		0);
	if (required <= 0) {
		// 极端情况下映射失败时返回原值，保证调用方仍能得到稳定（大小写敏感）的行为。
		return std::wstring(value);
	}

	std::wstring folded(static_cast<size_t>(required), L'\0');
	const int mapped = LCMapStringEx(
		LOCALE_NAME_INVARIANT,
		LCMAP_UPPERCASE,
		value.data(),
		sourceLength,
		folded.data(),
		static_cast<int>(folded.size()),
		nullptr,
		nullptr,
		0);
	if (mapped <= 0) {
		return std::wstring(value);
	}

	// 返回值可能计入结尾空字符；路径本身不含 NUL，安全裁剪。
	folded.resize(static_cast<size_t>(mapped));
	while (!folded.empty() && folded.back() == L'\0') {
		folded.pop_back();
	}
	return folded;
}

#endif

void FoldWindowsCase(std::wstring& value) {
#ifdef _WIN32
	value = FoldWindowsPathCase(value);
#else
	(void)value;
#endif
}

bool ComponentsEqual(
	const std::filesystem::path& left,
	const std::filesystem::path& right) {
	std::wstring leftValue = left.wstring();
	std::wstring rightValue = right.wstring();
	// Windows 文件系统身份不区分大小写；其他平台保留原生大小写语义。
	FoldWindowsCase(leftValue);
	FoldWindowsCase(rightValue);
	return leftValue == rightValue;
}

} // namespace

std::filesystem::path NormalizeExistingOrProspectivePath(
	const std::filesystem::path& path) {
	std::error_code error;
	const auto canonical = std::filesystem::weakly_canonical(path, error);
	if (!error) return canonical.lexically_normal();

	// 目标可能尚不存在；绝对路径加词法归一化仍能稳定处理新建 Config 目录。
	error.clear();
	const auto absolute = std::filesystem::absolute(path, error);
	return (error ? path : absolute).lexically_normal();
}

std::wstring BuildPathIdentityKey(const std::filesystem::path& path) {
	std::wstring value = NormalizeExistingOrProspectivePath(path).wstring();
	FoldWindowsCase(value);
	return value;
}

bool PathsEqual(
	const std::filesystem::path& left,
	const std::filesystem::path& right) {
	return BuildPathIdentityKey(left) == BuildPathIdentityKey(right);
}

bool IsEqualOrDescendant(
	const std::filesystem::path& candidate,
	const std::filesystem::path& ancestor) {
	const auto normalizedCandidate =
		NormalizeExistingOrProspectivePath(candidate);
	const auto normalizedAncestor =
		NormalizeExistingOrProspectivePath(ancestor);

	auto candidatePart = normalizedCandidate.begin();
	const auto candidateEnd = normalizedCandidate.end();
	for (auto ancestorPart = normalizedAncestor.begin();
		ancestorPart != normalizedAncestor.end(); ++ancestorPart, ++candidatePart) {
		if (candidatePart == candidateEnd
			|| !ComponentsEqual(*candidatePart, *ancestorPart)) {
			return false;
		}
	}
	return true;
}

} // namespace PathIdentity
