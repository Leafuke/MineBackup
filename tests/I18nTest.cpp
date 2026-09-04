#include "i18n.h"

#include <cctype>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
void Check(bool condition, const std::string& message) {
    if (!condition) { ++failures; std::cerr << "[FAIL] " << message << '\n'; }
}

// Compare argument consumption, not wording, field width or precision. Each
// '*' consumes an int before the converted value; '%%' consumes no argument.
std::optional<std::vector<std::string>> Signature(std::string_view format) {
    std::vector<std::string> result;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') continue;
        if (++i == format.size()) return std::nullopt;
        if (format[i] == '%') continue;
        auto isDigit = [&] { return i < format.size() && std::isdigit(static_cast<unsigned char>(format[i])); };
        while (i < format.size() && std::string_view("-+ #0'").find(format[i]) != std::string_view::npos) ++i;
        if (i < format.size() && format[i] == '*') { result.emplace_back("*"); ++i; }
        else while (isDigit()) ++i;
        if (i < format.size() && format[i] == '.') {
            ++i;
            if (i < format.size() && format[i] == '*') { result.emplace_back("*"); ++i; }
            else while (isDigit()) ++i;
        }
        std::string length;
        if (i < format.size() && std::string_view("hljztL").find(format[i]) != std::string_view::npos) {
            length += format[i++];
            if (i < format.size() && (length == "h" || length == "l") && format[i] == length[0]) length += format[i++];
        }
        if (i == format.size() || std::string_view("diouxXfFeEgGaAcspn").find(format[i]) == std::string_view::npos)
            return std::nullopt;
        result.push_back(length + format[i]);
    }
    return result;
}

bool HasSignature(const char* key, std::vector<std::string> expected) {
    const auto& table = g_LangTable.at(g_CurrentLang);
    const auto entry = table.find(key);
    const bool valid = entry != table.end() && Signature(entry->second.value) == expected;
    Check(valid, g_CurrentLang + " unexpected smoke-test signature: " + key);
    return valid; // Never call varargs with an unverified translated signature.
}
} // namespace

int main() {
    Check(Signature("%s %d %zu %llu %c %.2f %.*s %%")
            == std::vector<std::string>{"s", "d", "zu", "llu", "c", "f", "*", "s"},
        "format signatures must account for lengths, precision arguments and escaped percent");
    Check(Signature("%*.*s") == std::vector<std::string>{"*", "*", "s"}
            && !Signature("incomplete %") && !Signature("unsupported %q"),
        "malformed formats must fail instead of silently dropping arguments");

    const auto zh = g_LangTable.find("zh_CN"), en = g_LangTable.find("en_US");
    Check(zh != g_LangTable.end(), "zh_CN table missing");
    Check(en != g_LangTable.end(), "en_US table missing");
    if (zh == g_LangTable.end() || en == g_LangTable.end()) return 1;
    for (const char* language : {"zh_CN", "en_US"}) {
        const auto& table = g_LangTable.at(language);
        const auto& other = g_LangTable.at(std::string(language) == "zh_CN" ? "en_US" : "zh_CN");
        for (const auto& [key, value] : table) {
            Check(!value.value.empty(), std::string(language) + " empty translation: " + key);
            Check(other.contains(key), std::string(language) + " key absent in other language: " + key);
            const auto signature = Signature(value.value);
            Check(signature.has_value(), std::string(language) + " malformed format: " + key);
            if (other.contains(key)) Check(signature == Signature(other.at(key).value), "argument signature mismatch: " + key);
        }
    }

    const auto previousLanguage = g_CurrentLang;
    for (const char* language : {"zh_CN", "en_US"}) {
        SetLanguage(language);
        if (HasSignature("CONFIRM_DELETE_MSG", {"d", "s"})) {
            const auto value = MineFormatMessage("CONFIRM_DELETE_MSG", 7, "Profile");
            Check(value != L"CONFIRM_DELETE_MSG" && value.find(L"7") != std::wstring::npos
                    && value.find(L"Profile") != std::wstring::npos && value.find(L'%') == std::wstring::npos,
                std::string(language) + " must format both integer and string values");
        }
        if (HasSignature("LOG_BACKUP_SMART_INFO", {"zu"})) {
            const auto value = MineFormatMessage("LOG_BACKUP_SMART_INFO", static_cast<std::size_t>(42));
            Check(value != L"LOG_BACKUP_SMART_INFO" && value.find(L"42") != std::wstring::npos
                    && value.find(L'%') == std::wstring::npos,
                std::string(language) + " must format size_t values");
        }
    }
    SetLanguage(previousLanguage);
    if (!failures) std::cout << "[PASS] i18n keys, values, argument signatures and formatting\n";
    return failures ? 1 : 0;
}
