// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace shelf {
class Sha256 {
    std::array<std::uint32_t, 8> state_{{
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u}};
    std::array<unsigned char, 64> block_{};
    std::uint64_t bytes_ = 0;
    std::size_t used_ = 0;

    static constexpr std::array<std::uint32_t, 64> constants_{{
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u}};
    static constexpr std::uint32_t rotate(std::uint32_t value, unsigned amount) {
        return (value >> amount) | (value << (32 - amount));
    }
    void transform(const unsigned char* input) {
        std::uint32_t words[64]{};
        for (unsigned i = 0; i < 16; ++i)
            words[i] = (static_cast<std::uint32_t>(input[i * 4]) << 24) |
                       (static_cast<std::uint32_t>(input[i * 4 + 1]) << 16) |
                       (static_cast<std::uint32_t>(input[i * 4 + 2]) << 8) |
                       static_cast<std::uint32_t>(input[i * 4 + 3]);
        for (unsigned i = 16; i < 64; ++i) {
            const auto s0 = rotate(words[i - 15], 7) ^ rotate(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const auto s1 = rotate(words[i - 2], 17) ^ rotate(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        auto a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        auto e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (unsigned i = 0; i < 64; ++i) {
            const auto s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto first = h + s1 + choose + constants_[i] + words[i];
            const auto s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto second = s0 + majority;
            h = g; g = f; f = e; e = d + first;
            d = c; c = b; b = a; a = first + second;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

  public:
    void update(const void* bytes, std::size_t size) {
        const auto* input = static_cast<const unsigned char*>(bytes);
        bytes_ += size;
        while (size) {
            const auto amount = size < block_.size() - used_ ? size : block_.size() - used_;
            std::memcpy(block_.data() + used_, input, amount);
            used_ += amount;
            input += amount;
            size -= amount;
            if (used_ == block_.size()) {
                transform(block_.data());
                used_ = 0;
            }
        }
    }
    std::array<unsigned char, 32> finish() {
        const auto bits = bytes_ * 8;
        block_[used_++] = 0x80;
        if (used_ > 56) {
            while (used_ < block_.size()) block_[used_++] = 0;
            transform(block_.data());
            used_ = 0;
        }
        while (used_ < 56) block_[used_++] = 0;
        for (unsigned i = 0; i < 8; ++i)
            block_[56 + i] = static_cast<unsigned char>(bits >> (56 - i * 8));
        transform(block_.data());
        std::array<unsigned char, 32> output{};
        for (unsigned i = 0; i < 8; ++i) {
            output[i * 4] = static_cast<unsigned char>(state_[i] >> 24);
            output[i * 4 + 1] = static_cast<unsigned char>(state_[i] >> 16);
            output[i * 4 + 2] = static_cast<unsigned char>(state_[i] >> 8);
            output[i * 4 + 3] = static_cast<unsigned char>(state_[i]);
        }
        return output;
    }
};

inline void sha256_hex(const std::array<unsigned char, 32>& digest, char (&output)[65]) {
    constexpr char alphabet[] = "0123456789abcdef";
    for (std::size_t i = 0; i < digest.size(); ++i) {
        output[i * 2] = alphabet[digest[i] >> 4];
        output[i * 2 + 1] = alphabet[digest[i] & 15];
    }
    output[64] = 0;
}
} // namespace shelf
