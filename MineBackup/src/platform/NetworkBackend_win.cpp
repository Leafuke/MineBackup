#include "NetworkBackendFactory.h"

#include "NetworkService.h"
#include "text_to_text.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <memory>
#include <limits>
#include <vector>
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

using namespace std;

namespace {

class InternetHandle {
public:
    explicit InternetHandle(HINTERNET handle = nullptr) : handle_(handle) {}
    ~InternetHandle() { if (handle_) WinHttpCloseHandle(handle_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    operator HINTERNET() const { return handle_; }
    bool Valid() const { return handle_ != nullptr; }

private:
    HINTERNET handle_ = nullptr;
};

NetworkStatus StatusForLastError(DWORD error) {
    if (error == ERROR_WINHTTP_TIMEOUT) return NetworkStatus::TimedOut;
    if (error == ERROR_WINHTTP_SECURE_FAILURE
        || error == ERROR_WINHTTP_SECURE_CERT_DATE_INVALID
        || error == ERROR_WINHTTP_SECURE_CERT_CN_INVALID
        || error == ERROR_WINHTTP_SECURE_INVALID_CA
        || error == ERROR_WINHTTP_SECURE_CERT_REV_FAILED) {
        return NetworkStatus::TlsError;
    }
    return NetworkStatus::IoError;
}

wstring WinHttpErrorText(DWORD error) {
    return L"WinHTTP error " + to_wstring(error) + L".";
}

bool CrackHttpsUrl(const string& url, URL_COMPONENTS& components, wstring& host, wstring& path) {
    const wstring wideUrl = utf8_to_wstring(url);
    components = {};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components) || components.nScheme != INTERNET_SCHEME_HTTPS) {
        return false;
    }
    host.assign(components.lpszHostName, components.dwHostNameLength);
    path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength && components.lpszExtraInfo) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/";
    return true;
}

bool QueryHeaderString(HINTERNET request, DWORD query, wstring& value) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t)) return false;
    vector<wchar_t> buffer(bytes / sizeof(wchar_t));
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX,
        buffer.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) return false;
    value.assign(buffer.data());
    return true;
}

string ResolveRedirect(const string& currentUrl, const wstring& host, INTERNET_PORT port,
    const wstring& currentPath, const wstring& location) {
    const string locationUtf8 = wstring_to_utf8(location);
    if (locationUtf8.starts_with("https://") || locationUtf8.starts_with("http://")) return locationUtf8;
    string base = "https://" + wstring_to_utf8(host);
    if (port != INTERNET_DEFAULT_HTTPS_PORT) base += ":" + to_string(port);
    if (!location.empty() && location.front() == L'/') return base + locationUtf8;
    const auto query = currentPath.find(L'?');
    const wstring pathOnly = query == wstring::npos ? currentPath : currentPath.substr(0, query);
    const auto slash = pathOnly.find_last_of(L'/');
    const wstring directory = slash == wstring::npos ? L"/" : pathOnly.substr(0, slash + 1);
    (void)currentUrl;
    return base + wstring_to_utf8(directory + location);
}

bool IsHttpsUrl(const string& url) {
    if (url.size() < 8) return false;
    string prefix = url.substr(0, 8);
    transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char value) {
        return static_cast<char>(tolower(value));
    });
    return prefix == "https://";
}

class WinHttpNetworkBackend final : public NetworkBackend {
public:
    NetworkResult Get(const NetworkRequest& request, const NetworkChunkSink& sink, stop_token stopToken) override {
        NetworkResult result;
        string currentUrl = request.url;
        const auto deadline = chrono::steady_clock::now() + request.totalTimeout;
        for (int redirect = 0; redirect <= request.maximumRedirects; ++redirect) {
            if (stopToken.stop_requested()) {
                result.status = NetworkStatus::Cancelled;
                return result;
            }
            const auto remaining = chrono::duration_cast<chrono::milliseconds>(deadline - chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                result.status = NetworkStatus::TimedOut;
                return result;
            }

            URL_COMPONENTS components{};
            wstring host;
            wstring path;
            if (!CrackHttpsUrl(currentUrl, components, host, path)) {
                result.status = redirect == 0 ? NetworkStatus::InvalidRequest : NetworkStatus::InsecureRedirect;
                result.error = L"WinHTTP only accepts HTTPS URLs.";
                return result;
            }

            const wstring userAgent = utf8_to_wstring(request.userAgent);
            InternetHandle session(WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
            if (!session.Valid()) {
                const DWORD lastError = GetLastError();
                result.status = StatusForLastError(lastError);
                result.error = WinHttpErrorText(lastError);
                return result;
            }
            const int remainingMilliseconds = static_cast<int>((min)(remaining.count(),
                static_cast<decltype(remaining.count())>((numeric_limits<int>::max)())));
            if (!WinHttpSetTimeouts(session,
                static_cast<int>(request.connectTimeout.count()),
                static_cast<int>(request.connectTimeout.count()),
                remainingMilliseconds,
                remainingMilliseconds)) {
                const DWORD lastError = GetLastError();
                result.status = StatusForLastError(lastError);
                result.error = WinHttpErrorText(lastError);
                return result;
            }

            InternetHandle connection(WinHttpConnect(session, host.c_str(), components.nPort, 0));
            InternetHandle httpRequest(connection.Valid()
                ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                    WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                : nullptr);
            if (!connection.Valid() || !httpRequest.Valid()) {
                const DWORD lastError = GetLastError();
                result.status = StatusForLastError(lastError);
                result.error = WinHttpErrorText(lastError);
                return result;
            }
            DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
            if (!WinHttpSetOption(httpRequest, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled))) {
                const DWORD lastError = GetLastError();
                result.status = StatusForLastError(lastError);
                result.error = WinHttpErrorText(lastError);
                return result;
            }
            if (!WinHttpSendRequest(httpRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(httpRequest, nullptr)) {
                const DWORD lastError = GetLastError();
                result.status = StatusForLastError(lastError);
                result.error = WinHttpErrorText(lastError);
                return result;
            }

            DWORD status = 0;
            DWORD statusBytes = sizeof(status);
            if (!WinHttpQueryHeaders(httpRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes, WINHTTP_NO_HEADER_INDEX)) {
                result.status = NetworkStatus::IoError;
                result.error = L"Could not read the HTTP status code.";
                return result;
            }
            result.httpStatus = status;
            if (status >= 300 && status < 400) {
                wstring location;
                if (!QueryHeaderString(httpRequest, WINHTTP_QUERY_LOCATION, location)) {
                    result.status = NetworkStatus::HttpError;
                    result.error = L"The redirect response did not provide a Location header.";
                    return result;
                }
                const string nextUrl = ResolveRedirect(currentUrl, host, components.nPort, path, location);
                if (!IsHttpsUrl(nextUrl)) {
                    result.status = NetworkStatus::InsecureRedirect;
                    result.error = L"An HTTPS request attempted to redirect to a non-HTTPS URL.";
                    return result;
                }
                if (redirect == request.maximumRedirects) {
                    result.status = NetworkStatus::RedirectLimit;
                    result.error = L"The HTTPS redirect limit was exceeded.";
                    return result;
                }
                currentUrl = nextUrl;
                continue;
            }
            if (status < 200 || status >= 300) {
                result.status = NetworkStatus::HttpError;
                result.error = L"The server returned HTTP " + to_wstring(status) + L".";
                return result;
            }

            uint64_t expectedBytes = 0;
            bool hasExpectedBytes = false;
            wstring contentLength;
            if (QueryHeaderString(httpRequest, WINHTTP_QUERY_CONTENT_LENGTH, contentLength)) {
                try {
                    expectedBytes = stoull(contentLength);
                    hasExpectedBytes = true;
                }
                catch (...) {}
            }
            array<char, 64 * 1024> buffer{};
            while (!stopToken.stop_requested()) {
                if (chrono::steady_clock::now() >= deadline) {
                    result.status = NetworkStatus::TimedOut;
                    return result;
                }
                DWORD read = 0;
                if (!WinHttpReadData(httpRequest, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
                    const DWORD lastError = GetLastError();
                    result.status = StatusForLastError(lastError);
                    result.error = WinHttpErrorText(lastError);
                    return result;
                }
                if (read == 0) {
                    result.status = hasExpectedBytes && result.transferredBytes != expectedBytes
                        ? NetworkStatus::Truncated : NetworkStatus::Succeeded;
                    if (result.status == NetworkStatus::Truncated) {
                        result.error = L"The response ended before Content-Length bytes were received.";
                    }
                    result.finalUrl = currentUrl;
                    return result;
                }
                if (!sink(buffer.data(), read)) {
                    result.status = NetworkStatus::SinkRejected;
                    return result;
                }
                result.transferredBytes += read;
            }
            result.status = NetworkStatus::Cancelled;
            return result;
        }
        result.status = NetworkStatus::RedirectLimit;
        return result;
    }
};

} // namespace

shared_ptr<NetworkBackend> CreatePlatformNetworkBackend() {
    return make_shared<WinHttpNetworkBackend>();
}
