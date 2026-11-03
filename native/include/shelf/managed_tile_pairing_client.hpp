// SPDX-License-Identifier: GPL-3.0-only
// Offline client transcript for ShadowMount's managed-tile pairing candidate.
// No network transport, UI, secret storage, or tile write is bound here.
#pragma once

#include "pinned_helper_sha256.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace shelf::managed_tile_pairing_client {

constexpr std::size_t secret_bytes = 12;
constexpr std::size_t code_bytes = 20;
constexpr std::size_t nonce_bytes = 16;
constexpr std::size_t mac_bytes = 32;
constexpr std::size_t max_body_bytes = 4096;
constexpr std::size_t max_response_bytes = 4096;
constexpr std::size_t max_path_bytes = 128;
using Secret = std::array<unsigned char, secret_bytes>;
using Nonce = std::array<unsigned char, nonce_bytes>;
using Mac = std::array<unsigned char, mac_bytes>;

struct Session {
    Mac key{};
    std::uint64_t next_counter = 0;
    bool active = false;
};

inline void clear(void* bytes, std::size_t size) {
    volatile auto* at = static_cast<volatile unsigned char*>(bytes);
    while (size-- != 0) *at++ = 0;
}

inline void clear(Secret& secret) { clear(secret.data(), secret.size()); }
inline void clear(Session& session) { clear(&session, sizeof(session)); }

inline bool equal_mac(const Mac& left, const Mac& right) {
    unsigned char difference = 0;
    for (std::size_t i = 0; i < mac_bytes; ++i)
        difference |= static_cast<unsigned char>(left[i] ^ right[i]);
    return difference == 0;
}

inline bool decode_code(const char* code, std::size_t size, Secret& output) {
    constexpr char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    clear(output);
    if (!code || size != code_bytes) return false;
    Secret candidate{};
    bool valid = true;
    for (std::size_t symbol = 0; symbol < code_bytes && valid; ++symbol) {
        unsigned value = 32;
        for (unsigned digit = 0; digit < 32; ++digit)
            if (static_cast<unsigned char>(code[symbol]) ==
                static_cast<unsigned char>(alphabet[digit])) value = digit;
        if (value == 32) { valid = false; break; }
        for (std::size_t bit = 0; bit < 5; ++bit) {
            const auto position = symbol * 5 + bit;
            const auto digit = (value >> (4 - bit)) & 1u;
            if (position < secret_bytes * 8)
                candidate[position / 8] |= static_cast<unsigned char>(digit << (7 - position % 8));
            else if (digit != 0) valid = false; // Canonical zero padding.
        }
    }
    if (valid) output = candidate;
    clear(candidate);
    return valid;
}

struct Part { const void* data; std::size_t size; };

inline bool hmac(const void* key, std::size_t key_size, const Part* parts,
                 std::size_t part_count, Mac& output) {
    output.fill(0);
    if (!key || key_size == 0 || !parts || part_count == 0) return false;
    for (std::size_t i = 0; i < part_count; ++i)
        if (!parts[i].data && parts[i].size != 0) return false;
    unsigned char block[64]{};
    unsigned char inner_pad[64]{};
    unsigned char outer_pad[64]{};
    Mac inner{};
    if (key_size > sizeof(block)) {
        tile_job::detail::HelperSha256 hash;
        hash.update(static_cast<const unsigned char*>(key), key_size);
        auto digest = hash.finish();
        std::memcpy(block, digest.data(), digest.size());
        clear(&hash, sizeof(hash));
        clear(digest.data(), digest.size());
    } else {
        std::memcpy(block, key, key_size);
    }
    for (std::size_t i = 0; i < sizeof(block); ++i) {
        inner_pad[i] = static_cast<unsigned char>(block[i] ^ 0x36u);
        outer_pad[i] = static_cast<unsigned char>(block[i] ^ 0x5cu);
    }
    tile_job::detail::HelperSha256 first;
    first.update(inner_pad, sizeof(inner_pad));
    for (std::size_t i = 0; i < part_count; ++i)
        if (parts[i].size != 0)
            first.update(static_cast<const unsigned char*>(parts[i].data), parts[i].size);
    inner = first.finish();
    clear(&first, sizeof(first));
    tile_job::detail::HelperSha256 second;
    second.update(outer_pad, sizeof(outer_pad));
    second.update(inner.data(), inner.size());
    output = second.finish();
    clear(&second, sizeof(second));
    clear(block, sizeof(block));
    clear(inner_pad, sizeof(inner_pad));
    clear(outer_pad, sizeof(outer_pad));
    clear(inner.data(), inner.size());
    return true;
}

inline bool pair_mac(const Secret& secret, const char* domain,
                     std::size_t domain_size, const Nonce& server_nonce,
                     const Nonce& client_nonce, Mac& output) {
    const Part parts[]{{domain, domain_size}, {server_nonce.data(), nonce_bytes},
                       {client_nonce.data(), nonce_bytes}};
    return hmac(secret.data(), secret.size(), parts, 3, output);
}

inline bool client_proof(const Secret& secret, const Nonce& server_nonce,
                         const Nonce& client_nonce, Mac& output) {
    constexpr char domain[] = "ISHELF-PAIR-CLIENT-V1";
    return pair_mac(secret, domain, sizeof(domain) - 1, server_nonce, client_nonce, output);
}

inline bool finish(const Secret& secret, const Nonce& server_nonce,
                   const Nonce& client_nonce, const Mac& server_proof,
                   Session& session) {
    clear(session);
    constexpr char proof_domain[] = "ISHELF-PAIR-SERVER-V1";
    constexpr char key_domain[] = "ISHELF-PAIR-SESSION-V1";
    Mac expected{};
    Mac derived{};
    const bool valid = pair_mac(secret, proof_domain, sizeof(proof_domain) - 1,
                                server_nonce, client_nonce, expected) &&
                       equal_mac(expected, server_proof) &&
                       pair_mac(secret, key_domain, sizeof(key_domain) - 1,
                                server_nonce, client_nonce, derived);
    if (valid) {
        session.key = derived;
        session.next_counter = 1;
        session.active = true;
    }
    clear(expected.data(), expected.size());
    clear(derived.data(), derived.size());
    return valid;
}

inline bool allowed_path(const char* path, std::size_t size) {
    constexpr char create[] = "/api/v1/isshelf/tiles/create";
    constexpr char status[] = "/api/v1/isshelf/tiles/status";
    constexpr char remove[] = "/api/v1/isshelf/tiles/remove";
    return path && size > 0 && size <= max_path_bytes &&
           ((size == sizeof(create) - 1 && std::memcmp(path, create, size) == 0) ||
            (size == sizeof(status) - 1 && std::memcmp(path, status, size) == 0) ||
            (size == sizeof(remove) - 1 && std::memcmp(path, remove, size) == 0));
}

// Preparing an outbound request reserves its counter before any transport call.
// A caller must not reuse that counter or MAC after a lost response. The server
// still needs to accept authenticated counter gaps before this can be wired.
inline bool prepare_request(Session& session, const char* path,
                            std::size_t path_size, const void* body,
                            std::size_t body_size, std::uint64_t& counter,
                            Mac& output) {
    output.fill(0);
    counter = 0;
    if (!session.active || session.next_counter == 0 ||
        session.next_counter == UINT64_MAX || !allowed_path(path, path_size) ||
        !body || body_size > max_body_bytes) return false;
    const auto selected_counter = session.next_counter;
    constexpr char domain[] = "ISHELF-MANAGED-REQUEST-V1";
    unsigned char counter_wire[8];
    unsigned char path_size_wire[2];
    unsigned char body_size_wire[4];
    constexpr unsigned char post = 2;
    for (std::size_t i = 0; i < 8; ++i)
        counter_wire[i] = static_cast<unsigned char>(selected_counter >> (56 - 8 * i));
    path_size_wire[0] = static_cast<unsigned char>(path_size >> 8);
    path_size_wire[1] = static_cast<unsigned char>(path_size);
    for (std::size_t i = 0; i < 4; ++i)
        body_size_wire[i] = static_cast<unsigned char>(body_size >> (24 - 8 * i));
    const Part parts[]{{domain, sizeof(domain) - 1}, {counter_wire, sizeof(counter_wire)},
                       {&post, 1}, {path_size_wire, sizeof(path_size_wire)},
                       {path, path_size}, {body_size_wire, sizeof(body_size_wire)},
                       {body, body_size}};
    const bool valid = hmac(session.key.data(), session.key.size(), parts, 7, output);
    clear(counter_wire, sizeof(counter_wire));
    clear(path_size_wire, sizeof(path_size_wire));
    clear(body_size_wire, sizeof(body_size_wire));
    if (valid) {
        counter = selected_counter;
        session.next_counter = selected_counter + 1;
    }
    return valid;
}

// Verify the exact HTTP response bytes before a caller parses a receipt or
// STATUS. An unsigned error or a transport failure has no trusted body.
inline bool response_mac(const Session& session, std::uint64_t request_counter,
                         const Mac& request_mac, unsigned http_status,
                         const void* body, std::size_t body_size, Mac& output) {
    output.fill(0);
    if (!session.active || request_counter == 0 ||
        request_counter >= session.next_counter ||
        http_status < 100 || http_status > 599 || body_size > max_response_bytes ||
        (!body && body_size != 0)) return false;
    constexpr char domain[] = "ISHELF-MANAGED-RESPONSE-V1";
    unsigned char counter_wire[8];
    unsigned char status_wire[2];
    unsigned char body_size_wire[4];
    for (std::size_t i = 0; i < 8; ++i)
        counter_wire[i] = static_cast<unsigned char>(request_counter >> (56 - 8 * i));
    status_wire[0] = static_cast<unsigned char>(http_status >> 8);
    status_wire[1] = static_cast<unsigned char>(http_status);
    for (std::size_t i = 0; i < 4; ++i)
        body_size_wire[i] = static_cast<unsigned char>(body_size >> (24 - 8 * i));
    const Part parts[]{{domain, sizeof(domain) - 1},
                       {counter_wire, sizeof(counter_wire)},
                       {request_mac.data(), request_mac.size()},
                       {status_wire, sizeof(status_wire)},
                       {body_size_wire, sizeof(body_size_wire)},
                       {body, body_size}};
    const bool valid = hmac(session.key.data(), session.key.size(), parts, 6, output);
    clear(counter_wire, sizeof(counter_wire));
    clear(status_wire, sizeof(status_wire));
    clear(body_size_wire, sizeof(body_size_wire));
    return valid;
}

inline bool verify_response(const Session& session, std::uint64_t request_counter,
                            const Mac& request_mac, unsigned http_status,
                            const void* body, std::size_t body_size,
                            const Mac& received_mac) {
    Mac expected{};
    const bool valid = response_mac(session, request_counter, request_mac,
                                    http_status, body, body_size, expected) &&
                       equal_mac(expected, received_mac);
    clear(expected.data(), expected.size());
    return valid;
}

} // namespace shelf::managed_tile_pairing_client
