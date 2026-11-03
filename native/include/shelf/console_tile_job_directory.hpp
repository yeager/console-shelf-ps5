// SPDX-License-Identifier: GPL-3.0-only
// Portable identity checks used by the PS5 job directory adapter.
#pragma once
#include <cstdint>

namespace shelf::tile_job {
struct DirectoryIdentity {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t owner = 0;
    unsigned mode = 0;
    bool directory = false;
    bool symlink = false;
};

inline bool same_directory(const DirectoryIdentity& named,
                           const DirectoryIdentity& held) {
    return named.directory && held.directory && !named.symlink && !held.symlink &&
           named.device == held.device && named.inode == held.inode &&
           named.device != 0 && named.inode != 0;
}

// The observed console-shelf parent is UID 0 and mode 0777. Its mode is not a
// privacy boundary; only the child below can hold requests and receipts.
inline bool parent_identity_valid(const DirectoryIdentity& named,
                                  const DirectoryIdentity& held,
                                  std::uint64_t expected_owner = 0) {
    return same_directory(named, held) && named.owner == expected_owner &&
           held.owner == expected_owner;
}

inline bool private_child_identity_valid(const DirectoryIdentity& named,
                                         const DirectoryIdentity& held,
                                         std::uint64_t expected_owner) {
    return same_directory(named, held) && named.owner == expected_owner &&
           held.owner == expected_owner && named.mode == 0700u && held.mode == 0700u;
}
} // namespace shelf::tile_job
