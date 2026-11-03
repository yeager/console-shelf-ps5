// SPDX-License-Identifier: GPL-3.0-only
// Unwired codec for ShadowMount's proposed service-owned isShelf tile API.
#pragma once

#include "console_tile_intent.hpp"
#include <cstddef>
#include <string_view>

namespace shelf::managed_tile_api_v1 {

inline constexpr char capability[] = "isshelf_managed_tiles_v1";
inline constexpr char create_route[] = "/api/v1/isshelf/tiles/create";
inline constexpr char status_route[] = "/api/v1/isshelf/tiles/status";
inline constexpr char remove_route[] = "/api/v1/isshelf/tiles/remove";

enum class Stage {
    absent, preparing, registering, installed, removing, removed,
    conflict, held_unknown,
};
enum class Proof { unproved, yes, no };
enum class ParseResult { valid, needs_status, malformed, mismatched, held_unknown, conflict };
enum class ReceiptState { held_unknown, needs_status };

struct Status {
    Stage stage = Stage::held_unknown;
    char title_id[10]{};
    char create_nonce[33]{};
    ConsoleTileIntent intent{};
    bool has_intent = false;
    Proof source_verified = Proof::unproved;
    Proof mount_verified = Proof::unproved;
    Proof registration_verified = Proof::unproved;
    bool source_retained = false;
    char reason[65]{};
};

struct Operation {
    // A mutation POST is only a receipt. Terminal title state comes from
    // a separately reconciled status response with the exact create generation
    // and requested route; even a replay that reports installed/removed is not
    // an action-complete result here.
    ReceiptState receipt = ReceiptState::held_unknown;
    char title_id[10]{};
    char nonce[33]{};         // This CREATE or REMOVE operation's nonce.
    char create_nonce[33]{};  // CREATE generation, also retained for REMOVE.
    bool remove = false;
    bool replay = false;
};

// A capability is advisory. The caller must separately bind the running
// service to a trusted installed build and credential before any mutation.
bool parse_capability(std::string_view json);
bool nonce_valid(std::string_view nonce);
bool mutable_title(std::size_t system);
bool encode_create(const ConsoleTileIntent& intent, std::string_view nonce,
                   char* output, std::size_t capacity);
bool encode_status(std::size_t system, char* output, std::size_t capacity);
bool encode_remove(std::size_t system, std::string_view create_nonce,
                   std::string_view remove_nonce, char* output,
                   std::size_t capacity);

// A generation-specific read must supply both its create nonce and exact
// requested route. Supplying neither remains a diagnostic catalog read.
ParseResult parse_status(std::size_t system, std::string_view json,
                         Status& result,
                         std::string_view expected_create_nonce = {},
                         const ConsoleTileIntent* expected_intent = nullptr);
// Every well-formed mutation POST returns needs_status, never a terminal title
// result. Unknown replies retain the original title and both relevant nonces.
ParseResult parse_operation(std::size_t system, std::string_view nonce,
                            std::string_view expected_create_nonce,
                            bool remove, int http_status,
                            std::string_view json, Operation& result);

#ifndef SHELF_PS5
// Host fixture only. No sender is compiled into the PS5 app, and this module
// has no callsite in the current UI or ConsoleTileManager.
using Post = int (*)(void* context, const char* route, const char* request,
                     char* response, std::size_t capacity);
ParseResult post_status(Post post, void* context, std::size_t system,
                        Status& result,
                        std::string_view expected_create_nonce = {},
                        const ConsoleTileIntent* expected_intent = nullptr);
ParseResult post_create(Post post, void* context,
                        const ConsoleTileIntent& intent,
                        std::string_view nonce, Operation& result);
ParseResult post_remove(Post post, void* context, std::size_t system,
                        std::string_view create_nonce,
                        std::string_view remove_nonce, Operation& result);
#endif

} // namespace shelf::managed_tile_api_v1
