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
	TestLinkedAncestor(test, temporaryRoot / "path-identity-link");
}
