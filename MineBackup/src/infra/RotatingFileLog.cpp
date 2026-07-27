#include "RotatingFileLog.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <system_error>

using namespace std;

namespace RotatingFileLog {
namespace {

mutex g_logMutex;

filesystem::path ArchivedPath(const filesystem::path& path, int index) {
    return path.wstring() + L"." + to_wstring(index);
}

bool Rotate(const filesystem::path& path, int fileCount) {
    error_code error;
    if (fileCount <= 1) {
        ofstream truncated(path, ios::binary | ios::trunc);
        return truncated.good();
    }

    filesystem::remove(ArchivedPath(path, fileCount - 1), error);
    error.clear();
    for (int index = fileCount - 2; index >= 1; --index) {
        const auto source = ArchivedPath(path, index);
        if (!filesystem::exists(source, error)) {
            error.clear();
            continue;
        }
        const auto destination = ArchivedPath(path, index + 1);
        filesystem::remove(destination, error);
        error.clear();
        filesystem::rename(source, destination, error);
        if (error) return false;
    }
    if (filesystem::exists(path, error)) {
        if (error) return false;
        const auto destination = ArchivedPath(path, 1);
        filesystem::remove(destination, error);
        error.clear();
        filesystem::rename(path, destination, error);
        if (error) return false;
    }
    return true;
}

} // namespace

bool Append(const filesystem::path& requestedPath, const string& text, uintmax_t maximumBytes, int fileCount) {
    if (requestedPath.empty() || maximumBytes == 0 || fileCount <= 0) return false;
    lock_guard<mutex> lock(g_logMutex);

    error_code error;
    const auto path = filesystem::absolute(requestedPath, error).lexically_normal();
    if (error) return false;
    filesystem::create_directories(path.parent_path(), error);
    if (error) return false;

    size_t offset = 0;
    while (offset < text.size()) {
        uintmax_t currentSize = 0;
        if (filesystem::exists(path, error)) {
            if (error) return false;
            currentSize = filesystem::file_size(path, error);
            if (error) return false;
        }
        if (currentSize >= maximumBytes) {
            if (!Rotate(path, fileCount)) return false;
            currentSize = 0;
        }

        const uintmax_t available = maximumBytes - currentSize;
        const size_t count = static_cast<size_t>(min<uintmax_t>(
            available, static_cast<uintmax_t>(text.size() - offset)));
        ofstream output(path, ios::binary | ios::app);
        if (!output.is_open()) return false;
        output.write(text.data() + offset, static_cast<streamsize>(count));
        output.flush();
        if (!output.good()) return false;
        offset += count;
    }
    return true;
}

} // namespace RotatingFileLog
