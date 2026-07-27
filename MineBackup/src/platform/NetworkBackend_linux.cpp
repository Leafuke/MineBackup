#include "NetworkBackendFactory.h"

#include "NetworkService.h"

#include <curl/curl.h>
#include <cstring>
#include <memory>

using namespace std;

namespace {

struct CurlContext {
    const NetworkChunkSink* sink = nullptr;
    stop_token stopToken;
    bool sinkRejected = false;
};

size_t WriteCallback(char* data, size_t size, size_t count, void* userData) {
    auto& context = *static_cast<CurlContext*>(userData);
    const size_t bytes = size * count;
    if (context.stopToken.stop_requested()) return 0;
    if (!(*context.sink)(data, bytes)) {
        context.sinkRejected = true;
        return 0;
    }
    return bytes;
}

int ProgressCallback(void* userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto& context = *static_cast<CurlContext*>(userData);
    return context.stopToken.stop_requested() ? 1 : 0;
}

NetworkStatus CurlStatus(CURLcode code, const CurlContext& context) {
    if (context.stopToken.stop_requested() || code == CURLE_ABORTED_BY_CALLBACK) return NetworkStatus::Cancelled;
    if (context.sinkRejected && code == CURLE_WRITE_ERROR) return NetworkStatus::SinkRejected;
    if (code == CURLE_PARTIAL_FILE) return NetworkStatus::Truncated;
    if (code == CURLE_OPERATION_TIMEDOUT) return NetworkStatus::TimedOut;
    if (code == CURLE_TOO_MANY_REDIRECTS) return NetworkStatus::RedirectLimit;
    if (code == CURLE_SSL_CONNECT_ERROR || code == CURLE_PEER_FAILED_VERIFICATION
        || code == CURLE_SSL_CERTPROBLEM || code == CURLE_SSL_CACERT_BADFILE
        || code == CURLE_SSL_CRL_BADFILE || code == CURLE_SSL_ISSUER_ERROR) {
        return NetworkStatus::TlsError;
    }
    return NetworkStatus::IoError;
}

bool IsHttps(const char* url) {
    return url && string(url).starts_with("https://");
}

class CurlNetworkBackend final : public NetworkBackend {
public:
    CurlNetworkBackend() {
        static const CURLcode initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
        available_ = initialized == CURLE_OK;
    }

    NetworkResult Get(const NetworkRequest& request, const NetworkChunkSink& sink, stop_token stopToken) override {
        NetworkResult result;
        if (!available_) return result;
        unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
        if (!handle) return result;

        CurlContext context{&sink, stopToken, false};
        curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, request.userAgent.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_MAXREDIRS, static_cast<long>(request.maximumRedirects));
        curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
        curl_easy_setopt(handle.get(), CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
        curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(request.connectTimeout.count()));
        curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(request.totalTimeout.count()));
        curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &context);
        curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, ProgressCallback);
        curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &context);

        const CURLcode code = curl_easy_perform(handle.get());
        curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &result.httpStatus);
        char* effectiveUrl = nullptr;
        curl_easy_getinfo(handle.get(), CURLINFO_EFFECTIVE_URL, &effectiveUrl);
        if (effectiveUrl) result.finalUrl = effectiveUrl;
        curl_off_t transferred = 0;
        curl_easy_getinfo(handle.get(), CURLINFO_SIZE_DOWNLOAD_T, &transferred);
        if (transferred > 0) result.transferredBytes = static_cast<uint64_t>(transferred);

        if (code != CURLE_OK) {
            result.status = CurlStatus(code, context);
            result.error.assign(curl_easy_strerror(code), curl_easy_strerror(code) + strlen(curl_easy_strerror(code)));
            return result;
        }
        if (!IsHttps(effectiveUrl)) {
            result.status = NetworkStatus::InsecureRedirect;
            result.error = L"The final URL was not HTTPS.";
            return result;
        }
        if (result.httpStatus < 200 || result.httpStatus >= 300) {
            result.status = NetworkStatus::HttpError;
            result.error = L"The server returned HTTP " + to_wstring(result.httpStatus) + L".";
            return result;
        }
        result.status = NetworkStatus::Succeeded;
        return result;
    }

private:
    bool available_ = false;
};

} // namespace

shared_ptr<NetworkBackend> CreatePlatformNetworkBackend() {
    return make_shared<CurlNetworkBackend>();
}
