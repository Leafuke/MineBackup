#include "LegacyServicePolicy.h"

#include <cwctype>

using namespace std;
namespace fs = std::filesystem;

namespace {

wstring Trim(wstring value) {
    while (!value.empty() && iswspace(value.front())) value.erase(value.begin());
    while (!value.empty() && iswspace(value.back())) value.pop_back();
    return value;
}

wstring Lower(wstring value) {
    for (auto& character : value) character = towlower(character);
    return value;
}

bool IsStandaloneToken(const wstring& text, size_t offset, size_t length) {
    const bool validPrefix = offset == 0 || iswspace(text[offset - 1]);
    const size_t end = offset + length;
    const bool validSuffix = end == text.size() || iswspace(text[end]);
    return validPrefix && validSuffix;
}

} // namespace

LegacyServiceImagePath ParseLegacyServiceImagePath(const wstring& imagePath) {
    LegacyServiceImagePath result;
    const wstring trimmed = Trim(imagePath);
    if (trimmed.empty() || trimmed.find(L'\0') != wstring::npos) {
        result.diagnostic = L"The service ImagePath is empty or malformed.";
        return result;
    }

    const wstring lowered = Lower(trimmed);
    constexpr wchar_t serviceToken[] = L"--service";
    constexpr size_t serviceTokenLength = 9;
    size_t tokenOffset = wstring::npos;
    for (size_t offset = lowered.find(serviceToken); offset != wstring::npos;
         offset = lowered.find(serviceToken, offset + 1)) {
        if (!IsStandaloneToken(lowered, offset, serviceTokenLength)) continue;
        if (tokenOffset != wstring::npos) {
            result.diagnostic = L"The service ImagePath contains duplicate --service arguments.";
            return result;
        }
        tokenOffset = offset;
    }
    if (tokenOffset == wstring::npos) {
        result.diagnostic = L"The service ImagePath does not contain the required --service argument.";
        return result;
    }

    wstring executableText = Trim(trimmed.substr(0, tokenOffset));
    if (!Trim(trimmed.substr(tokenOffset + serviceTokenLength)).empty()) {
        result.diagnostic = L"The service ImagePath contains arguments other than --service.";
        return result;
    }
    if (executableText.size() >= 2 && executableText.front() == L'"'
        && executableText.back() == L'"') {
        executableText = executableText.substr(1, executableText.size() - 2);
    }
    if (executableText.empty() || executableText.find(L'"') != wstring::npos) {
        result.diagnostic = L"The executable portion of the service ImagePath is malformed.";
        return result;
    }

    result.executable = fs::path(executableText);
    if (!result.executable.is_absolute()
        || Lower(result.executable.filename().wstring()) != L"minebackup.exe") {
        result.executable.clear();
        result.diagnostic = L"The service ImagePath does not point to an absolute MineBackup.exe.";
        return result;
    }
    result.valid = true;
    return result;
}
