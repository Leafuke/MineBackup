#include "text_to_text.h"
#include "PlatformCompat.h"

#include <string>
#include <vector>

#if !defined(_WIN32)
#include <errno.h>
#include <iconv.h>
#endif

namespace {

#ifndef _WIN32
constexpr char32_t kReplacementChar = 0xFFFD;

bool EncodeCodePoint(char32_t cp, std::u8string& out) {
    if (cp > 0x10FFFF) {
        cp = kReplacementChar;
    }
    if (cp <= 0x7F) {
        out.push_back(static_cast<char8_t>(cp));
        return true;
    }
    if (cp <= 0x7FF) {
        out.push_back(static_cast<char8_t>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char8_t>(0x80 | (cp & 0x3F)));
        return true;
    }
    if (cp <= 0xFFFF) {
        out.push_back(static_cast<char8_t>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char8_t>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char8_t>(0x80 | (cp & 0x3F)));
        return true;
    }
    out.push_back(static_cast<char8_t>(0xF0 | ((cp >> 18) & 0x07)));
    out.push_back(static_cast<char8_t>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char8_t>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char8_t>(0x80 | (cp & 0x3F)));
    return true;
}

bool DecodeNextCodePoint(const std::u8string& input, size_t& offset, char32_t& out) {
    const size_t size = input.size();
    if (offset >= size) {
        return false;
    }
    unsigned char lead = static_cast<unsigned char>(input[offset]);
    if (lead < 0x80) {
        out = lead;
        ++offset;
        return true;
    }
    auto continuation_ok = [&](size_t idx) {
        return idx < size && (static_cast<unsigned char>(input[idx]) >> 6) == 0x2;
    };
    if ((lead >> 5) == 0x6 && continuation_ok(offset + 1)) {
        char32_t cp = (lead & 0x1F) << 6;
        cp |= static_cast<unsigned char>(input[offset + 1]) & 0x3F;
        offset += 2;
        out = cp;
        return true;
    }
    if ((lead >> 4) == 0xE && continuation_ok(offset + 1) && continuation_ok(offset + 2)) {
        char32_t cp = (lead & 0x0F) << 12;
        cp |= (static_cast<unsigned char>(input[offset + 1]) & 0x3F) << 6;
        cp |= static_cast<unsigned char>(input[offset + 2]) & 0x3F;
        offset += 3;
        out = cp;
        return true;
    }
    if ((lead >> 3) == 0x1E && continuation_ok(offset + 1) && continuation_ok(offset + 2) && continuation_ok(offset + 3)) {
        char32_t cp = (lead & 0x07) << 18;
        cp |= (static_cast<unsigned char>(input[offset + 1]) & 0x3F) << 12;
        cp |= (static_cast<unsigned char>(input[offset + 2]) & 0x3F) << 6;
        cp |= static_cast<unsigned char>(input[offset + 3]) & 0x3F;
        offset += 4;
        out = cp;
        return true;
    }
    // Invalid sequence
    offset += 1;
    out = kReplacementChar;
    return false;
}

std::u8string wstring_to_u8string_posix(const std::wstring& wstr) {
    std::u8string result;
    result.reserve(wstr.size());
    for (wchar_t ch : wstr) {
        EncodeCodePoint(static_cast<char32_t>(ch), result);
    }
    return result;
}

std::wstring u8string_to_wstring_posix(const std::u8string& u8) {
    std::wstring result;
    result.reserve(u8.size());
    size_t offset = 0;
    while (offset < u8.size()) {
        char32_t cp = kReplacementChar;
        DecodeNextCodePoint(u8, offset, cp);
        result.push_back(static_cast<wchar_t>(cp));
    }
    return result;
}

std::string ConvertEncoding(const std::string& input, const char* from, const char* to) {
    if (input.empty()) {
        return {};
    }
    iconv_t cd = iconv_open(to, from);
    if (cd == reinterpret_cast<iconv_t>(-1)) {
        return input;
    }

    std::string inCopy = input;
    std::string output;
    output.resize(inCopy.size() * 4 + 4);

    char* inPtr = inCopy.data();
    size_t inBytesLeft = inCopy.size();
    char* outPtr = output.data();
    size_t outBytesLeft = output.size();

    while (inBytesLeft > 0) {
        size_t res = iconv(cd, &inPtr, &inBytesLeft, &outPtr, &outBytesLeft);
        if (res == static_cast<size_t>(-1)) {
            if (errno == E2BIG) {
                size_t used = output.size() - outBytesLeft;
                output.resize(output.size() * 2);
                outPtr = output.data() + used;
                outBytesLeft = output.size() - used;
                continue;
            }
            iconv_close(cd);
            return input; // fall back to original on error
        }
    }

    iconv_close(cd);
    output.resize(output.size() - outBytesLeft);
    return output;
}
#endif

} // namespace

std::u8string wstring_to_u8string(const std::wstring& wstr) {
#ifdef _WIN32
    if (wstr.empty()) return {};
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return {};
    std::u8string result(static_cast<size_t>(size_needed), u8'\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), reinterpret_cast<LPSTR>(result.data()), size_needed, nullptr, nullptr);
    return result;
#else
    return wstring_to_u8string_posix(wstr);
#endif
}

std::wstring u8string_to_wstring(const std::u8string& str) {
#ifdef _WIN32
    if (str.empty()) return {};
    const char* data = reinterpret_cast<const char*>(str.data());
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(str.size()), nullptr, 0);
    if (size_needed <= 0) return {};
    std::wstring result(static_cast<size_t>(size_needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(str.size()), result.data(), size_needed);
    return result;
#else
    return u8string_to_wstring_posix(str);
#endif
}

std::string wstring_to_utf8(const std::wstring& wstr) {
    std::u8string u8 = wstring_to_u8string(wstr);
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::wstring utf8_to_wstring(const std::string& str) {
    std::u8string u8(reinterpret_cast<const char8_t*>(str.data()), str.size());
    return u8string_to_wstring(u8);
}

Utf8SanitizeResult SanitizeUtf8(const std::string_view input, const std::size_t maximumBytes) {
	Utf8SanitizeResult result;
	if (input.empty()) return result;
	if (maximumBytes == 0) {
		result.truncated = true;
		return result;
	}

	auto appendCodePoint = [](char32_t codePoint, std::string& output) {
		if (codePoint <= 0x7F) {
			output.push_back(static_cast<char>(codePoint));
		}
		else if (codePoint <= 0x7FF) {
			output.push_back(static_cast<char>(0xC0 | ((codePoint >> 6) & 0x1F)));
			output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
		}
		else if (codePoint <= 0xFFFF) {
			output.push_back(static_cast<char>(0xE0 | ((codePoint >> 12) & 0x0F)));
			output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
			output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
		}
		else {
			output.push_back(static_cast<char>(0xF0 | ((codePoint >> 18) & 0x07)));
			output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
			output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
			output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
		}
	};

	auto continuation = [&](const std::size_t index) {
		return index < input.size()
			&& (static_cast<unsigned char>(input[index]) & 0xC0) == 0x80;
	};

	for (std::size_t offset = 0; offset < input.size();) {
		const unsigned char lead = static_cast<unsigned char>(input[offset]);
		std::size_t length = 1;
		char32_t codePoint = 0xFFFD;
		bool valid = true;
		if (lead <= 0x7F) {
			codePoint = lead;
		}
		else if (lead >= 0xC2 && lead <= 0xDF && continuation(offset + 1)) {
			length = 2;
			codePoint = static_cast<char32_t>(lead & 0x1F) << 6;
			codePoint |= static_cast<unsigned char>(input[offset + 1]) & 0x3F;
		}
		else if (lead >= 0xE0 && lead <= 0xEF
			&& continuation(offset + 1) && continuation(offset + 2)
			&& (lead != 0xE0 || static_cast<unsigned char>(input[offset + 1]) >= 0xA0)
			&& (lead != 0xED || static_cast<unsigned char>(input[offset + 1]) <= 0x9F)) {
			length = 3;
			codePoint = static_cast<char32_t>(lead & 0x0F) << 12;
			codePoint |= static_cast<unsigned char>(input[offset + 1]) << 6;
			codePoint |= static_cast<unsigned char>(input[offset + 2]) & 0x3F;
		}
		else if (lead >= 0xF0 && lead <= 0xF4
			&& continuation(offset + 1) && continuation(offset + 2) && continuation(offset + 3)
			&& (lead != 0xF0 || static_cast<unsigned char>(input[offset + 1]) >= 0x90)
			&& (lead != 0xF4 || static_cast<unsigned char>(input[offset + 1]) <= 0x8F)) {
			length = 4;
			codePoint = static_cast<char32_t>(lead & 0x07) << 18;
			codePoint |= static_cast<unsigned char>(input[offset + 1]) << 12;
			codePoint |= static_cast<unsigned char>(input[offset + 2]) << 6;
			codePoint |= static_cast<unsigned char>(input[offset + 3]) & 0x3F;
		}
		else {
			valid = false;
		}

		if (!valid) {
			result.invalidUtf8Replaced = true;
			codePoint = 0xFFFD;
			length = 1;
		}
		std::string encoded;
		appendCodePoint(codePoint, encoded);
		if (encoded.size() > maximumBytes - result.value.size()) {
			result.truncated = true;
			break;
		}
		result.value += encoded;
		offset += length;
	}
	return result;
}

std::string gbk_to_utf8(const std::string& gbk) {
#ifdef _WIN32
    if (gbk.empty()) return {};
    int lenW = MultiByteToWideChar(CP_ACP, 0, gbk.c_str(), static_cast<int>(gbk.size()), nullptr, 0);
    if (lenW <= 0) return gbk;
    std::wstring wstr(static_cast<size_t>(lenW), L'\0');
    MultiByteToWideChar(CP_ACP, 0, gbk.c_str(), static_cast<int>(gbk.size()), wstr.data(), lenW);

    int lenU8 = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (lenU8 <= 0) return gbk;
    std::string u8str(static_cast<size_t>(lenU8), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), u8str.data(), lenU8, nullptr, nullptr);
    return u8str;
#else
    return ConvertEncoding(gbk, "GBK", "UTF-8");
#endif
}

std::string utf8_to_gbk(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return {};
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.length()), nullptr, 0);
    if (wide_len <= 0) return utf8;
    std::wstring wide(static_cast<size_t>(wide_len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.length()), wide.data(), wide_len);

    int gbk_len = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (gbk_len <= 0) return utf8;
    std::string gbk(static_cast<size_t>(gbk_len), '\0');
    WideCharToMultiByte(CP_ACP, 0, wide.c_str(), static_cast<int>(wide.size()), gbk.data(), gbk_len, nullptr, nullptr);
    return gbk;
#else
    return ConvertEncoding(utf8, "UTF-8", "GBK");
#endif
}

const char* as_utf8(const char8_t* s) noexcept {
    return reinterpret_cast<const char*>(s);
}

std::string u8string_to_string(const std::u8string& s) {
    return std::string(as_utf8(s.data()), s.size());
}
