// SPDX-License-Identifier: GPL-3.0-only
// Narrow in-memory SHA-256 check for the exact compiled console helper pin.
// The transform follows the existing GPL SHA-256 used by console_tile_manager.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace shelf::tile_job::detail {

class HelperSha256 {
    std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<unsigned char, 64> block_{};
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;

    static std::uint32_t rotate(std::uint32_t value, unsigned bits) {
        return (value >> bits) | (value << (32 - bits));
    }
    void transform() {
        constexpr std::array<std::uint32_t, 64> constants{
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
            0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
            0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
            0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
            0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
            0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
            0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
            0xc67178f2U};
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto* at = block_.data() + i * 4;
            words[i] = (static_cast<std::uint32_t>(at[0]) << 24) |
                       (static_cast<std::uint32_t>(at[1]) << 16) |
                       (static_cast<std::uint32_t>(at[2]) << 8) | at[3];
        }
        for (std::size_t i = 16; i < words.size(); ++i) {
            const auto a = rotate(words[i - 15], 7) ^ rotate(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const auto b = rotate(words[i - 2], 17) ^ rotate(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + a + words[i - 7] + b;
        }
        auto a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        auto e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto t1 = h + s1 + choose + constants[i] + words[i];
            const auto s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + majority;
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

public:
    void update(const unsigned char* bytes, std::size_t size) {
        total_ += size;
        for (std::size_t i = 0; i < size; ++i) {
            block_[used_++] = bytes[i];
            if (used_ == block_.size()) { transform(); used_ = 0; }
        }
    }
    std::array<unsigned char, 32> finish() {
        const std::uint64_t bits = total_ * 8;
        unsigned char padding[128]{};
        padding[0] = 0x80;
        update(padding, used_ < 56 ? 56 - used_ : 120 - used_);
        unsigned char length[8];
        for (int i = 0; i < 8; ++i) length[i] = static_cast<unsigned char>(bits >> (56 - i * 8));
        update(length, sizeof(length));
        std::array<unsigned char, 32> result{};
        for (std::size_t i = 0; i < state_.size(); ++i)
            for (int offset = 0; offset < 4; ++offset)
                result[i * 4 + offset] = static_cast<unsigned char>(state_[i] >> (24 - offset * 8));
        return result;
    }
};

inline bool exact_helper_sha256(const unsigned char* data, std::size_t bytes,
                                std::size_t expected_bytes, const char* expected_sha256) {
    if (!data || !bytes || !expected_bytes || bytes != expected_bytes ||
        !expected_sha256 || std::strlen(expected_sha256) != 64) return false;
    HelperSha256 hash;
    hash.update(data, bytes);
    const auto digest = hash.finish();
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < digest.size(); ++i) {
        if (expected_sha256[2 * i] != hex[digest[i] >> 4] ||
            expected_sha256[2 * i + 1] != hex[digest[i] & 15]) return false;
    }
    return true;
}

} // namespace shelf::tile_job::detail
