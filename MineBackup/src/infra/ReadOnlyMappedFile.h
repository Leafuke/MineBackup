#pragma once

#include <cstddef>
#include <filesystem>
#include <system_error>

namespace minebackup::infra {

class ReadOnlyMappedFile {
public:
    ReadOnlyMappedFile() = default;
    ~ReadOnlyMappedFile();

    ReadOnlyMappedFile(const ReadOnlyMappedFile&) = delete;
    ReadOnlyMappedFile& operator=(const ReadOnlyMappedFile&) = delete;

    ReadOnlyMappedFile(ReadOnlyMappedFile&& other) noexcept;
    ReadOnlyMappedFile& operator=(ReadOnlyMappedFile&& other) noexcept;

    bool Open(const std::filesystem::path& path, std::error_code& error) noexcept;
    void Close() noexcept;

    [[nodiscard]] bool IsOpen() const noexcept { return data_ != nullptr; }
    [[nodiscard]] const void* Data() const noexcept { return data_; }
    [[nodiscard]] std::size_t Size() const noexcept { return size_; }

private:
    void* data_ = nullptr;
    std::size_t size_ = 0;
#ifdef _WIN32
    void* mappingHandle_ = nullptr;
#endif
};

} // namespace minebackup::infra
