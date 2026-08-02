#include "ConfigSelection.h"

#include <cwctype>

using namespace std;

namespace {
	bool EqualStableId(const wstring& left, const wstring& right) {
#ifdef _WIN32
		return _wcsicmp(left.c_str(), right.c_str()) == 0;
#else
		if (left.size() != right.size()) return false;
		for (size_t index = 0; index < left.size(); ++index) {
			if (towlower(left[index]) != towlower(right[index])) return false;
		}
		return true;
#endif
	}
}

int FindConfigByStableId(
	const map<int, Config>& configs,
	const wstring& stableId) {
	for (const auto& [index, config] : configs) {
		if (EqualStableId(config.configId, stableId)) return index;
	}
	return -1;
}

int FindSpecialConfigByStableId(
	const map<int, SpecialConfig>& configs,
	const wstring& stableId) {
	for (const auto& [index, config] : configs) {
		if (EqualStableId(config.specialConfigId, stableId)) return index;
	}
	return -1;
}
