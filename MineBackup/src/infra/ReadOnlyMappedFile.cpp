#include "ReadOnlyMappedFile.h"

#include <limits>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace minebackup::infra {

ReadOnlyMappedFile::~ReadOnlyMappedFile() {
    Close();
}

ReadOnlyMappedFile::ReadOnlyMappedFile(ReadOnlyMappedFile&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0))
#ifdef _WIN32
      , mappingHandle_(std::exchange(other.mappingHandle_, nullptr))
#endif
{
}

ReadOnlyMappedFile& ReadOnlyMappedFile::operator=(ReadOnlyMappedFile&& other) noexcept {
    if (this == &other) return *this;
    Close();
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
#ifdef _WIN32
    mappingHandle_ = std::exchange(other.mappingHandle_, nullptr);
#endif
    return *this;
}

bool ReadOnlyMappedFile::Open(
    const std::filesystem::path& path,
    std::error_code& error) noexcept {
    Close();
    error.clear();

#ifdef _WIN32
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize)) {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        CloseHandle(file);
        return false;
    }
    if (fileSize.QuadPart <= 0
        || static_cast<unsigned long long>(fileSize.QuadPart)
            > std::numeric_limits<std::size_t>::max()) {
        error = std::make_error_code(fileSize.QuadPart <= 0
            ? std::errc::invalid_argument
            : std::errc::file_too_large);
        CloseHandle(file);
        return false;
    }

    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const DWORD mappingError = mapping == nullptr ? GetLastError() : ERROR_SUCCESS;
    CloseHandle(file);
    if (mapping == nullptr) {
        error = std::error_code(static_cast<int>(mappingError), std::system_category());
        return false;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        CloseHandle(mapping);
        return false;
    }

    data_ = view;
    size_ = static_cast<std::size_t>(fileSize.QuadPart);
    mappingHandle_ = mapping;
#else
    const int descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        error = std::error_code(errno, std::generic_category());
        return false;
    }

    struct stat metadata {};
    if (fstat(descriptor, &metadata) != 0) {
        error = std::error_code(errno, std::generic_category());
        close(descriptor);
        return false;
    }
    if (metadata.st_size <= 0
        || static_cast<unsigned long long>(metadata.st_size)
            > std::numeric_limits<std::size_t>::max()) {
        error = std::make_error_code(metadata.st_size <= 0
            ? std::errc::invalid_argument
            : std::errc::file_too_large);
        close(descriptor);
        return false;
    }

    void* view = mmap(nullptr, static_cast<std::size_t>(metadata.st_size),
        PROT_READ, MAP_PRIVATE, descriptor, 0);
    const int mappingError = errno;
    close(descriptor);
    if (view == MAP_FAILED) {
        error = std::error_code(mappingError, std::generic_category());
        return false;
    }

    data_ = view;
    size_ = static_cast<std::size_t>(metadata.st_size);
#endif
    return true;
}

void ReadOnlyMappedFile::Close() noexcept {
    if (data_ != nullptr) {
#ifdef _WIN32
        UnmapViewOfFile(data_);
#else
        munmap(data_, size_);
#endif
        data_ = nullptr;
        size_ = 0;
    }
#ifdef _WIN32
    if (mappingHandle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mappingHandle_));
        mappingHandle_ = nullptr;
    }
#endif
}

} // namespace minebackup::infra
