#include "PathIdentity.h"

#include <algorithm>
#include <cwctype>
#include <system_error>

namespace PathIdentity {
namespace {

void FoldWindowsCase(std::wstring& value) {
#ifdef _WIN32
	std::transform(value.begin(), value.end(), value.begin(),
		[](wchar_t character) { return std::towlower(character); });
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
