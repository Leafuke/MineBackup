#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

class Sha256 {
public:
    void Update(const void* data, std::size_t size);
    std::string FinalHex();

    static bool FileHex(const std::filesystem::path& path, std::string& hex, std::wstring& error);

private:
    void Transform(const std::uint8_t* block);

    std::array<std::uint32_t, 8> state_{{
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u}};
    std::array<std::uint8_t, 64> buffer_{};
    std::uint64_t totalBytes_ = 0;
    std::size_t bufferedBytes_ = 0;
    bool finalized_ = false;
};
