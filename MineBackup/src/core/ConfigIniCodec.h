#pragma once
#include "DataModels.h"
#include "OperationResult.h"
#include <map>
#include <set>
#include <string>
#include <vector>
namespace ConfigIniCodec {
struct DecodeResult {
    std::map<int, Config> configs;
    std::vector<Diagnostic> diagnostics;
    bool valid = true;
};
DecodeResult Parse(const std::string& content);
const std::set<std::wstring>& ManagedConfigKeys();
std::vector<std::wstring> SerializeConfig(int index, const Config& config,
    const std::vector<std::wstring>& unknownLines = {});
}
