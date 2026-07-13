#include "Sha256.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

using namespace std;

namespace {

constexpr array<uint32_t, 64> kRoundConstants{{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u}};

uint32_t RotateRight(uint32_t value, unsigned int count) {
    return (value >> count) | (value << (32u - count));
}

} // namespace

void Sha256::Transform(const uint8_t* block) {
    uint32_t words[64]{};
    for (size_t index = 0; index < 16; ++index) {
        words[index] = (static_cast<uint32_t>(block[index * 4]) << 24)
            | (static_cast<uint32_t>(block[index * 4 + 1]) << 16)
            | (static_cast<uint32_t>(block[index * 4 + 2]) << 8)
            | static_cast<uint32_t>(block[index * 4 + 3]);
    }
    for (size_t index = 16; index < 64; ++index) {
        const uint32_t s0 = RotateRight(words[index - 15], 7) ^ RotateRight(words[index - 15], 18) ^ (words[index - 15] >> 3);
        const uint32_t s1 = RotateRight(words[index - 2], 17) ^ RotateRight(words[index - 2], 19) ^ (words[index - 2] >> 10);
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];
    for (size_t index = 0; index < 64; ++index) {
        const uint32_t sum1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
        const uint32_t choice = (e & f) ^ ((~e) & g);
        const uint32_t temporary1 = h + sum1 + choice + kRoundConstants[index] + words[index];
        const uint32_t sum0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::Update(const void* data, size_t size) {
    if (finalized_ || !data || size == 0) return;
    const auto* bytes = static_cast<const uint8_t*>(data);
    totalBytes_ += size;
    while (size > 0) {
        const size_t copied = (min)(size, buffer_.size() - bufferedBytes_);
        copy_n(bytes, copied, buffer_.begin() + bufferedBytes_);
        bytes += copied;
        size -= copied;
        bufferedBytes_ += copied;
        if (bufferedBytes_ == buffer_.size()) {
            Transform(buffer_.data());
            bufferedBytes_ = 0;
        }
    }
}

string Sha256::FinalHex() {
    if (!finalized_) {
        const uint64_t totalBits = totalBytes_ * 8u;
        buffer_[bufferedBytes_++] = 0x80u;
        if (bufferedBytes_ > 56) {
            fill(buffer_.begin() + bufferedBytes_, buffer_.end(), 0);
            Transform(buffer_.data());
            bufferedBytes_ = 0;
        }
        fill(buffer_.begin() + bufferedBytes_, buffer_.begin() + 56, 0);
        for (size_t index = 0; index < 8; ++index) {
            buffer_[63 - index] = static_cast<uint8_t>(totalBits >> (index * 8));
        }
        Transform(buffer_.data());
        finalized_ = true;
    }
    ostringstream output;
    output << hex << setfill('0');
    for (const auto value : state_) output << setw(8) << value;
    return output.str();
}

bool Sha256::FileHex(const filesystem::path& path, string& hex, wstring& error) {
    ifstream input(path, ios::binary);
    if (!input.is_open()) {
        error = L"Could not open the file for SHA-256 verification.";
        return false;
    }
    Sha256 hash;
    array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<streamsize>(buffer.size()));
        const auto read = input.gcount();
        if (read > 0) hash.Update(buffer.data(), static_cast<size_t>(read));
    }
    if (!input.eof()) {
        error = L"Could not read the complete file for SHA-256 verification.";
        return false;
    }
    hex = hash.FinalHex();
    error.clear();
    return true;
}
