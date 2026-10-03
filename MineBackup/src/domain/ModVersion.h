#pragma once
#include <string>
#include <tuple>
#include <cstdio>
namespace ModVersion {
inline bool IsCompatible(const std::string& current, const std::string& required) {
		auto parseVersion = [](const std::string& value, std::tuple<int, int, int>& parsed) {
			int major = 0;
			int minor = 0;
			int patch = 0;
			char trailing = '\0';
			if (std::sscanf(value.c_str(), "%d.%d.%d%c", &major, &minor, &patch, &trailing) != 3 ||
				major < 0 || minor < 0 || patch < 0) {
				return false;
			}
			parsed = { major, minor, patch };
			return true;
		};
		std::tuple<int, int, int> currentVersion;
		std::tuple<int, int, int> requiredVersion;
		return parseVersion(current, currentVersion) &&
			parseVersion(required, requiredVersion) &&
			currentVersion >= requiredVersion;
	}

}
