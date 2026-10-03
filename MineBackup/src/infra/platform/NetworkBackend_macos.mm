#include "NetworkBackendFactory.h"

#include "NetworkService.h"

#import <Foundation/Foundation.h>

#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>

using namespace std;

@interface MineBackupNetworkDelegate : NSObject <NSURLSessionDataDelegate, NSURLSessionTaskDelegate> {
@public
    const NetworkChunkSink* sink;
    stop_token stopToken;
    int maximumRedirects;
    int redirects;
    NetworkResult result;
    NetworkStatus forcedStatus;
    bool hasForcedStatus;
    long long expectedBytes;
    bool complete;
    mutex completionMutex;
    condition_variable completionCondition;
}
@end

@implementation MineBackupNetworkDelegate

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
    newRequest:(NSURLRequest*)request
    completionHandler:(void (^)(NSURLRequest* _Nullable))completionHandler {
    (void)session;
    (void)task;
    (void)response;
    ++redirects;
    NSString* scheme = request.URL.scheme.lowercaseString;
    if (redirects > maximumRedirects) {
        hasForcedStatus = true;
        forcedStatus = NetworkStatus::RedirectLimit;
        completionHandler(nil);
        return;
    }
    if (![scheme isEqualToString:@"https"]) {
        hasForcedStatus = true;
        forcedStatus = NetworkStatus::InsecureRedirect;
        completionHandler(nil);
        return;
    }
    completionHandler(request);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask
    didReceiveResponse:(NSURLResponse*)response
    completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    (void)session;
    if (![response isKindOfClass:[NSHTTPURLResponse class]]) {
        hasForcedStatus = true;
        forcedStatus = NetworkStatus::HttpError;
        completionHandler(NSURLSessionResponseCancel);
        return;
    }
    NSHTTPURLResponse* http = (NSHTTPURLResponse*)response;
    result.httpStatus = http.statusCode;
    expectedBytes = response.expectedContentLength;
    if (http.statusCode < 200 || http.statusCode >= 300) {
        hasForcedStatus = true;
        forcedStatus = NetworkStatus::HttpError;
        completionHandler(NSURLSessionResponseCancel);
        return;
    }
    completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask didReceiveData:(NSData*)data {
    (void)session;
    if (stopToken.stop_requested()) {
        [dataTask cancel];
        return;
    }
    if (!(*sink)(static_cast<const char*>(data.bytes), data.length)) {
        hasForcedStatus = true;
        forcedStatus = NetworkStatus::SinkRejected;
        [dataTask cancel];
        return;
    }
    result.transferredBytes += data.length;
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    (void)session;
    if (task.currentRequest.URL.absoluteString) {
        result.finalUrl = task.currentRequest.URL.absoluteString.UTF8String;
    }
    if (hasForcedStatus) {
        result.status = forcedStatus;
    }
    else if (!error) {
        result.status = expectedBytes >= 0 && result.transferredBytes != static_cast<uint64_t>(expectedBytes)
            ? NetworkStatus::Truncated : NetworkStatus::Succeeded;
    }
    else if (stopToken.stop_requested() || error.code == NSURLErrorCancelled) {
        result.status = NetworkStatus::Cancelled;
    }
    else if (error.code == NSURLErrorTimedOut) {
        result.status = NetworkStatus::TimedOut;
    }
    else if (error.code == NSURLErrorServerCertificateHasBadDate
        || error.code == NSURLErrorServerCertificateUntrusted
        || error.code == NSURLErrorServerCertificateHasUnknownRoot
        || error.code == NSURLErrorServerCertificateNotYetValid
        || error.code == NSURLErrorSecureConnectionFailed) {
        result.status = NetworkStatus::TlsError;
    }
    else {
        result.status = NetworkStatus::IoError;
    }
    if (error.localizedDescription) {
        const char* text = error.localizedDescription.UTF8String;
        if (text) result.error.assign(text, text + strlen(text));
    }
    {
        lock_guard lock(completionMutex);
        complete = true;
    }
    completionCondition.notify_all();
}

@end

namespace {

class NSURLSessionNetworkBackend final : public NetworkBackend {
public:
    NetworkResult Get(const NetworkRequest& request, const NetworkChunkSink& sink, stop_token stopToken) override {
        @autoreleasepool {
            NSString* urlText = [NSString stringWithUTF8String:request.url.c_str()];
            NSURL* url = urlText ? [NSURL URLWithString:urlText] : nil;
            if (!url || ![url.scheme.lowercaseString isEqualToString:@"https"]) {
                NetworkResult invalid;
                invalid.status = NetworkStatus::InvalidRequest;
                return invalid;
            }

            MineBackupNetworkDelegate* delegate = [MineBackupNetworkDelegate new];
            delegate->sink = &sink;
            delegate->stopToken = stopToken;
            delegate->maximumRedirects = request.maximumRedirects;
            delegate->redirects = 0;
            delegate->hasForcedStatus = false;
            delegate->expectedBytes = -1;
            delegate->complete = false;

            NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
            configuration.timeoutIntervalForRequest = request.connectTimeout.count() / 1000.0;
            configuration.timeoutIntervalForResource = request.totalTimeout.count() / 1000.0;
            NSOperationQueue* queue = [NSOperationQueue new];
            queue.maxConcurrentOperationCount = 1;
            NSURLSession* session = [NSURLSession sessionWithConfiguration:configuration delegate:delegate delegateQueue:queue];
            NSMutableURLRequest* urlRequest = [NSMutableURLRequest requestWithURL:url];
            urlRequest.HTTPMethod = @"GET";
            [urlRequest setValue:[NSString stringWithUTF8String:request.userAgent.c_str()] forHTTPHeaderField:@"User-Agent"];
            NSURLSessionDataTask* task = [session dataTaskWithRequest:urlRequest];
            [task resume];

            unique_lock lock(delegate->completionMutex);
            while (!delegate->complete) {
                delegate->completionCondition.wait_for(lock, chrono::milliseconds(50));
                if (stopToken.stop_requested()) [task cancel];
            }
            lock.unlock();
            [session finishTasksAndInvalidate];
            return delegate->result;
        }
    }
};

} // namespace

shared_ptr<NetworkBackend> CreatePlatformNetworkBackend() {
    return make_shared<NSURLSessionNetworkBackend>();
}
