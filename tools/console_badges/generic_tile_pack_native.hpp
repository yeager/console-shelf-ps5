// SPDX-License-Identifier: GPL-3.0-only
// Side-effect-free reader for the exact reviewed 51-title source pack.
#pragma once

#include "generic_tile_catalog.generated.hpp"
#include "generic_tile_pack_index.generated.hpp"
#include "sha256_stream.hpp"
#include <cstddef>
#include <cstring>

namespace isshelf_badge::generic_tile {

struct PackView { const unsigned char* data = nullptr; std::size_t bytes = 0; };

inline bool pack_hash_equals(const unsigned char* data, std::size_t size, const char* expected) {
    if (!data || !expected || std::strlen(expected) != 64) return false;
    isshelf_badge::Sha256 hash;
    hash.update(data, size);
    const auto digest = hash.finish();
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < digest.size(); ++i)
        if (expected[2 * i] != hex[digest[i] >> 4] ||
            expected[2 * i + 1] != hex[digest[i] & 15]) return false;
    return true;
}

class VerifiedNativePack {
public:
    VerifiedNativePack(const unsigned char* data, std::size_t size) : data_(data) {
        if (!data || size != pack_bytes ||
            std::memcmp(data, pack_magic, sizeof(pack_magic) - 1) != 0 ||
            !pack_hash_equals(data, size, pack_sha256)) return;
        std::size_t cursor = sizeof(pack_magic) - 1;
        for (const auto& blob : pack_blobs) {
            if (blob.offset != cursor || !blob.bytes || blob.bytes > size - cursor ||
                !pack_hash_equals(data + blob.offset, blob.bytes, blob.sha256)) return;
            cursor += blob.bytes;
        }
        valid_ = cursor == size;
    }

    bool valid() const { return valid_; }

    // The caller selects only a fixed catalog row and one of its seven pinned
    // filenames. PS1 is adopted and has no provisionable bytes in this pack.
    bool file(std::size_t system, std::size_t file_index, PackView& out) const {
        out = {};
        if (!valid_ || system >= tile_count || tiles[system].adopted || file_index >= 7)
            return false;
        const auto& file = tiles[system].files[file_index];
        for (const auto& blob : pack_blobs) {
            if (std::strcmp(blob.sha256, file.sha256) == 0 && blob.bytes == file.bytes &&
                pack_hash_equals(data_ + blob.offset, blob.bytes, blob.sha256)) {
                // A future writer must rehash bytes as it copies them. This
                // view does not keep a mutable caller buffer from changing.
                out = {data_ + blob.offset, blob.bytes};
                return true;
            }
        }
        return false;
    }

private:
    const unsigned char* data_ = nullptr;
    bool valid_ = false;
};

} // namespace isshelf_badge::generic_tile
