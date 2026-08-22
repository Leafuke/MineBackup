#include "PathIdentityTests.h"

#include "PathIdentity.h"

#include <system_error>

namespace {

void TestNormalization(
	TestContext& test,
	const std::filesystem::path& root) {
	const auto existing = root / "path-identity" / "existing" / "world";
	std::error_code error;
	std::filesystem::create_directories(existing, error);
	test.Expect(!error, "path identity existing fixture should be created");

	const auto withDotAndParent = existing.parent_path() / "." / "world"
		/ ".." / "world" / "";
	test.Expect(PathIdentity::PathsEqual(withDotAndParent, existing),
		"path identity should normalize dot, parent and trailing separators");
	const auto normalized =
		PathIdentity::NormalizeExistingOrProspectivePath(withDotAndParent);
	test.Expect(normalized.is_absolute(),
		"normalized existing paths should be absolute");

	const auto prospective = root / "path-identity" / "missing" / ".."
		/ "future" / "world";
	const auto equivalentProspective = root / "path-identity" / "future" / "world";
	test.Expect(!std::filesystem::exists(prospective)
		&& PathIdentity::PathsEqual(prospective, equivalentProspective),
		"path identity should normalize prospective paths without requiring them to exist");
}

void TestContainment(
	TestContext& test,
	const std::filesystem::path& root) {
	const auto ancestor = root / "foo";
	test.Expect(PathIdentity::IsEqualOrDescendant(ancestor, ancestor),
		"a path should be an equal-or-descendant of itself");
	test.Expect(PathIdentity::IsEqualOrDescendant(ancestor / "world", ancestor),
		"a child path should be an equal-or-descendant");
	test.Expect(!PathIdentity::IsEqualOrDescendant(root / "foobar" / "world", ancestor),
		"component-aware containment should reject foobar as a child of foo");
	test.Expect(!PathIdentity::IsEqualOrDescendant(root / "bar" / "foo", ancestor),
		"component-aware containment should reject a sibling subtree");
}

void TestPlatformCaseIdentity(
	TestContext& test,
	const std::filesystem::path& root) {
	const auto upper = root / "CaseProbe" / "world";
	const auto lower = root / "caseprobe" / "world";
#ifdef _WIN32
	test.Expect(PathIdentity::PathsEqual(upper, lower),
		"Windows path identity should ignore case");
	test.Expect(PathIdentity::BuildPathIdentityKey(upper)
			== PathIdentity::BuildPathIdentityKey(lower),
		"Windows path identity keys should be case-insensitive");
#else
	test.Expect(!PathIdentity::PathsEqual(upper, lower),
		"non-Windows path identity should preserve case");
#endif
}

void TestUnicodeCaseIdentity(
	TestContext& test,
	const std::filesystem::path& root) {
	// 刻意使用尚不存在的 prospective path，避免真实文件系统 canonicalize
	// 在背后“代劳”，从而确保校验的是 PathIdentity 自身的大小写规则。
	const auto upper = root / L"École" / L"world";
	const auto lower = root / L"école" / L"world";
	const auto shout = root / L"ÉCOLE" / L"world";
	test.Expect(!std::filesystem::exists(upper) && !std::filesystem::exists(lower),
		"unicode case fixtures must remain prospective");
#ifdef _WIN32
	// É/é 等 Latin-1 扩展字符在 C locale（如中文 Windows）下无法被
	// std::towlower 正确折叠；必须依赖操作系统 Unicode 大小写表。
	test.Expect(PathIdentity::PathsEqual(upper, lower),
		"Windows Unicode case difference should share path identity");
	test.Expect(PathIdentity::PathsEqual(shout, lower),
		"Windows Unicode identity should hold for full-uppercase variants");
	test.Expect(PathIdentity::BuildPathIdentityKey(upper)
			== PathIdentity::BuildPathIdentityKey(lower),
		"Windows Unicode identity keys should match across case");
	test.Expect(PathIdentity::IsEqualOrDescendant(
			root / L"école" / L"world", root / L"ÉCOLE"),
		"Unicode containment should ignore case on Windows");
#else
	test.Expect(!PathIdentity::PathsEqual(upper, lower),
		"non-Windows Unicode case difference must stay distinct");
#endif
}

void TestLinkedAncestor(
	TestContext& test,
	const std::filesystem::path& root) {
	std::error_code error;
	const auto realRoot = root / "real";
	const auto linkedRoot = root / "linked";
	std::filesystem::create_directories(realRoot / "world", error);
	test.Expect(!error, "path identity linked fixture should be created");
	std::filesystem::create_directory_symlink(realRoot, linkedRoot, error);
	if (error) {
		// Windows 未启用开发者模式时无法创建测试 symlink；其余路径用例仍继续执行。
		return;
	}
	test.Expect(PathIdentity::PathsEqual(linkedRoot / "world", realRoot / "world"),
		"path identity should resolve an existing symbolic-link ancestor");
	test.Expect(PathIdentity::IsEqualOrDescendant(linkedRoot / "world", realRoot),
		"containment should account for an existing symbolic-link ancestor");
}

} // namespace

void RunPathIdentityTests(
	TestContext& test,
	const std::filesystem::path& temporaryRoot) {
	TestNormalization(test, temporaryRoot);
	TestContainment(test, temporaryRoot / "path-identity-containment");
	TestPlatformCaseIdentity(test, temporaryRoot / "path-identity-case");
	TestUnicodeCaseIdentity(test, temporaryRoot / "path-identity-unicode-case");
	TestLinkedAncestor(test, temporaryRoot / "path-identity-link");
}
