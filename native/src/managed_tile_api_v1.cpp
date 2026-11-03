// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/managed_tile_api_v1.hpp"
#include "cjson/cJSON.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace shelf::managed_tile_api_v1 {
namespace {

constexpr std::size_t max_reply = 4096;

class Document {
    cJSON* root_ = nullptr;

  public:
    explicit Document(std::string_view json) {
        if (json.empty() || json.size() > max_reply ||
            std::memchr(json.data(), 0, json.size())) return;
        char bytes[max_reply + 1];
        std::memcpy(bytes, json.data(), json.size());
        bytes[json.size()] = 0;
        root_ = cJSON_ParseWithLengthOpts(bytes, json.size() + 1, nullptr, 1);
        if (!cJSON_IsObject(root_)) {
            cJSON_Delete(root_);
            root_ = nullptr;
        }
    }
    ~Document() { cJSON_Delete(root_); }
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    cJSON* get() const { return root_; }
};

bool unique_tree(const cJSON* node, unsigned depth = 0) {
    if (!node || depth > 8) return false;
    if (cJSON_IsObject(node)) {
        for (auto* item = node->child; item; item = item->next) {
            if (!item->string || !item->string[0]) return false;
            for (auto* earlier = node->child; earlier != item; earlier = earlier->next)
                if (std::strcmp(earlier->string, item->string) == 0) return false;
            if (!unique_tree(item, depth + 1)) return false;
        }
    } else if (cJSON_IsArray(node)) {
        for (auto* item = node->child; item; item = item->next)
            if (!unique_tree(item, depth + 1)) return false;
    }
    return true;
}

bool only_fields(const cJSON* object, std::initializer_list<const char*> allowed) {
    if (!cJSON_IsObject(object)) return false;
    for (auto* item = object->child; item; item = item->next) {
        bool known = false;
        for (auto* name : allowed) known = known || std::strcmp(item->string, name) == 0;
        if (!known) return false;
    }
    return true;
}

const cJSON* field(const cJSON* object, const char* name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

bool number_is(const cJSON* item, int expected) {
    return cJSON_IsNumber(item) && item->valuedouble == expected &&
           item->valueint == expected;
}

std::string_view text(const cJSON* item) {
    return cJSON_IsString(item) && item->valuestring
               ? std::string_view(item->valuestring)
               : std::string_view();
}

bool copy(std::string_view value, char* output, std::size_t capacity) {
    if (value.size() >= capacity) return false;
    std::memcpy(output, value.data(), value.size());
    output[value.size()] = 0;
    return true;
}

bool reason_valid(std::string_view reason) {
    if (reason.empty() || reason.size() > 64) return false;
    for (char ch : reason)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '_')) return false;
    return true;
}

bool stage_from(std::string_view text_value, Stage& stage) {
    if (text_value == "absent") stage = Stage::absent;
    else if (text_value == "preparing") stage = Stage::preparing;
    else if (text_value == "registering") stage = Stage::registering;
    else if (text_value == "installed") stage = Stage::installed;
    else if (text_value == "removing") stage = Stage::removing;
    else if (text_value == "removed") stage = Stage::removed;
    else if (text_value == "conflict") stage = Stage::conflict;
    else if (text_value == "held_unknown") stage = Stage::held_unknown;
    else return false;
    return true;
}

bool proof_from(const cJSON* item, Proof& proof) {
    if (cJSON_IsNull(item)) proof = Proof::unproved;
    else if (cJSON_IsTrue(item)) proof = Proof::yes;
    else if (cJSON_IsFalse(item)) proof = Proof::no;
    else return false;
    return true;
}

const char* title_for(std::size_t system) {
    const char* title = tile_job::owned_title_for(system);
    return title && std::strlen(title) == 9 ? title : nullptr;
}

bool valid_version(const cJSON* root) {
    return number_is(field(root, "status"), 0) &&
           number_is(field(root, "api_version"), 1);
}

// A mutation receipt is parsed only after a POST might have committed an
// intent. Keep its original identity until status proves the outcome.
ParseResult operation_indeterminate(std::size_t system, std::string_view nonce,
                                    std::string_view create_nonce, bool remove,
                                    Operation& result) {
    result = {};
    copy(title_for(system), result.title_id, sizeof(result.title_id));
    copy(nonce, result.nonce, sizeof(result.nonce));
    copy(create_nonce, result.create_nonce, sizeof(result.create_nonce));
    result.remove = remove;
    return ParseResult::held_unknown;
}

#ifndef SHELF_PS5
bool call(Post post, void* context, const char* route, const char* request,
          char (&reply)[max_reply], int& http_status) {
    if (!post) return false;
    std::memset(reply, 0, sizeof(reply));
    http_status = post(context, route, request, reply, sizeof(reply));
    return http_status >= 0 && std::memchr(reply, 0, sizeof(reply)) != nullptr;
}

#endif

} // namespace

bool nonce_valid(std::string_view nonce) {
    if (nonce.size() != 32) return false;
    for (char ch : nonce)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    return true;
}

bool mutable_title(std::size_t system) {
    const char* title = title_for(system);
    if (!title || isshelf_badge::generic_tile::tiles[system].adopted)
        return false;
    for (const auto& file : isshelf_badge::generic_tile::tiles[system].files)
        if (!file.bytes || !file.sha256 || std::strlen(file.sha256) != 64)
            return false;
    for (const char* pilot : {"PPSA99702", "PPSA99703", "PPSA99704",
                              "PPSA99711"})
        if (std::strcmp(title, pilot) == 0) return false;
    return true;
}

bool encode_create(const ConsoleTileIntent& intent, std::string_view nonce,
                   char* output, std::size_t capacity) {
    if (!output || !capacity || !mutable_title(intent.system) ||
        !console_tile_intent_valid(intent) || !nonce_valid(nonce)) return false;
    const auto system = catalog[intent.system].id;
    const auto route = console_tile_destination_name(intent.destination);
    const char* emulator = intent.destination == ConsoleTileDestination::emulator
                               ? intent.emulator_title : "-";
    const int used = std::snprintf(output, capacity,
        "{\"contract_version\":1,\"title_id\":\"%s\",\"system_id\":\"%.*s\","
        "\"route\":\"%.*s\",\"emulator_title_id\":\"%s\",\"nonce\":\"%.*s\"}",
        title_for(intent.system), static_cast<int>(system.size()), system.data(),
        static_cast<int>(route.size()), route.data(), emulator,
        static_cast<int>(nonce.size()), nonce.data());
    return used > 0 && static_cast<std::size_t>(used) < capacity && used < 4096;
}

bool encode_status(std::size_t system, char* output, std::size_t capacity) {
    const char* title = title_for(system);
    if (!output || !capacity || !title) return false;
    const int used = std::snprintf(output, capacity,
        "{\"contract_version\":1,\"title_id\":\"%s\"}", title);
    return used > 0 && static_cast<std::size_t>(used) < capacity && used < 4096;
}

bool encode_remove(std::size_t system, std::string_view create_nonce,
                   std::string_view remove_nonce, char* output,
                   std::size_t capacity) {
    if (!output || !capacity || !mutable_title(system) ||
        !nonce_valid(create_nonce) || !nonce_valid(remove_nonce)) return false;
    const int used = std::snprintf(output, capacity,
        "{\"contract_version\":1,\"title_id\":\"%s\","
        "\"expected_create_nonce\":\"%.*s\",\"nonce\":\"%.*s\"}",
        title_for(system), static_cast<int>(create_nonce.size()),
        create_nonce.data(), static_cast<int>(remove_nonce.size()),
        remove_nonce.data());
    return used > 0 && static_cast<std::size_t>(used) < capacity && used < 4096;
}

bool parse_capability(std::string_view json) {
    Document doc(json);
    auto* root = doc.get();
    if (!root || !unique_tree(root) || !valid_version(root)) return false;
    const auto* capabilities = field(root, "capabilities");
    const auto* managed = field(root, "isshelf_managed_tiles");
    if (!cJSON_IsArray(capabilities) || !cJSON_IsObject(managed) ||
        !only_fields(managed, {"contract_version", "enabled", "reason"}) ||
        !number_is(field(managed, "contract_version"), 1) ||
        !cJSON_IsTrue(field(managed, "enabled"))) return false;
    const auto* reason = field(managed, "reason");
    if (reason && !cJSON_IsNull(reason)) return false;
    bool found = false;
    for (auto* item = capabilities->child; item; item = item->next) {
        if (!cJSON_IsString(item) || text(item).empty() ||
            text(item).size() > 64) return false;
        for (auto* earlier = capabilities->child; earlier != item;
             earlier = earlier->next)
            if (text(earlier) == text(item)) return false;
        const auto name = text(item);
        if (name == capability) {
            if (found) return false;
            found = true;
        }
    }
    return found;
}

ParseResult parse_status(std::size_t system, std::string_view json,
                         Status& result,
                         std::string_view expected_create_nonce,
                         const ConsoleTileIntent* expected_intent) {
    const bool has_nonce = !expected_create_nonce.empty();
    const bool has_intent = expected_intent != nullptr;
    char expected_nonce_copy[33]{};
    ConsoleTileIntent expected_intent_copy{};
    if (has_nonce && !nonce_valid(expected_create_nonce)) {
        result = {};
        return ParseResult::mismatched;
    }
    if (has_nonce)
        copy(expected_create_nonce, expected_nonce_copy,
             sizeof(expected_nonce_copy));
    if (has_intent) expected_intent_copy = *expected_intent;
    result = {};
    const char* title = title_for(system);
    if (!title || has_nonce != has_intent ||
        (has_intent &&
         (expected_intent_copy.system != system ||
          !console_tile_intent_valid(expected_intent_copy))))
        return ParseResult::mismatched;
    Document doc(json);
    auto* root = doc.get();
    if (!root || !unique_tree(root) ||
        !only_fields(root, {"status", "contract_version", "title_id", "state",
                            "create_nonce", "route", "emulator_title_id",
                            "source_verified", "mount_verified",
                            "registration_verified", "reason", "source_retained"}) ||
        !number_is(field(root, "status"), 0) ||
        !number_is(field(root, "contract_version"), 1))
        return ParseResult::malformed;
    if (text(field(root, "title_id")) != title)
        return ParseResult::mismatched;
    Status parsed;
    if (!copy(title, parsed.title_id, sizeof(parsed.title_id)) ||
        !stage_from(text(field(root, "state")), parsed.stage) ||
        !proof_from(field(root, "source_verified"), parsed.source_verified) ||
        !proof_from(field(root, "mount_verified"), parsed.mount_verified) ||
        !proof_from(field(root, "registration_verified"),
                    parsed.registration_verified))
        return ParseResult::malformed;
    const auto* nonce_item = field(root, "create_nonce");
    const auto* route_item = field(root, "route");
    const auto* emulator_item = field(root, "emulator_title_id");
    const bool null_intent = cJSON_IsNull(nonce_item) && cJSON_IsNull(route_item) &&
                             cJSON_IsNull(emulator_item);
    if (!null_intent) {
        const auto nonce = text(nonce_item);
        const auto route = text(route_item);
        const auto emulator = text(emulator_item);
        if (!nonce_valid(nonce) || !console_tile_fields(catalog[system].id,
                                                       route, emulator,
                                                       parsed.intent) ||
            !copy(nonce, parsed.create_nonce, sizeof(parsed.create_nonce)))
            return ParseResult::malformed;
        parsed.has_intent = true;
    }
    const auto* reason_item = field(root, "reason");
    if (!cJSON_IsNull(reason_item)) {
        const auto reason = text(reason_item);
        if (!reason_valid(reason) ||
            !copy(reason, parsed.reason, sizeof(parsed.reason)))
            return ParseResult::malformed;
    }
    const auto* retained = field(root, "source_retained");
    if (retained) {
        if (!cJSON_IsBool(retained) || parsed.stage != Stage::removed)
            return ParseResult::malformed;
        parsed.source_retained = cJSON_IsTrue(retained);
    }
    if (parsed.stage == Stage::absent &&
        (parsed.has_intent || parsed.reason[0] ||
         parsed.source_verified != Proof::no ||
         parsed.mount_verified != Proof::no ||
         parsed.registration_verified != Proof::no))
        return ParseResult::malformed;
    if ((parsed.stage == Stage::preparing || parsed.stage == Stage::registering ||
         parsed.stage == Stage::installed || parsed.stage == Stage::removing ||
         parsed.stage == Stage::removed) &&
        !parsed.has_intent) return ParseResult::malformed;
    if (parsed.stage == Stage::installed &&
        (parsed.source_verified != Proof::yes ||
         parsed.mount_verified != Proof::yes ||
         parsed.registration_verified != Proof::yes || parsed.reason[0]))
        return ParseResult::malformed;
    if (parsed.stage == Stage::removed &&
        (parsed.mount_verified != Proof::no ||
         parsed.registration_verified != Proof::no || parsed.reason[0] ||
         (parsed.source_verified == Proof::yes &&
          (!retained || !parsed.source_retained))))
        return ParseResult::malformed;
    if ((parsed.stage == Stage::conflict || parsed.stage == Stage::held_unknown) &&
        !parsed.reason[0]) return ParseResult::malformed;
    // Proven absence has no create generation or route to compare.
    if (parsed.stage != Stage::absent && has_nonce &&
        (!parsed.has_intent ||
         std::string_view(parsed.create_nonce) != expected_nonce_copy))
        return ParseResult::mismatched;
    if (parsed.has_intent && has_intent &&
        !console_tile_intents_equal(parsed.intent, expected_intent_copy))
        return ParseResult::mismatched;
    result = parsed;
    if (parsed.stage == Stage::conflict) return ParseResult::conflict;
    if (parsed.stage == Stage::held_unknown) return ParseResult::held_unknown;
    return ParseResult::valid;
}

ParseResult parse_operation(std::size_t system, std::string_view nonce,
                            std::string_view expected_create_nonce,
                            bool remove, int http_status,
                            std::string_view json, Operation& result) {
    const bool valid_nonce = nonce_valid(nonce);
    const bool valid_generation =
        remove ? nonce_valid(expected_create_nonce)
               : (expected_create_nonce.empty() ||
                  expected_create_nonce == nonce);
    char nonce_copy[33]{}, create_copy[33]{};
    if (valid_nonce && valid_generation) {
        copy(nonce, nonce_copy, sizeof(nonce_copy));
        copy(remove ? expected_create_nonce : nonce,
             create_copy, sizeof(create_copy));
    }
    result = {};
    const char* title = title_for(system);
    if (!title || !valid_nonce || !valid_generation)
        return ParseResult::mismatched;
    operation_indeterminate(system, nonce_copy, create_copy, remove, result);
    if (http_status != 202 && http_status != 200)
        return ParseResult::held_unknown;
    Document doc(json);
    auto* root = doc.get();
    if (!root || !unique_tree(root) ||
        !only_fields(root, {"status", "contract_version", "title_id",
                            "operation", "nonce", "state", "replay"}) ||
        !number_is(field(root, "status"), 0) ||
        !number_is(field(root, "contract_version"), 1) ||
        !cJSON_IsBool(field(root, "replay")))
        return ParseResult::held_unknown;
    if (text(field(root, "title_id")) != title ||
        text(field(root, "nonce")) != nonce_copy ||
        text(field(root, "operation")) != (remove ? "remove" : "create"))
        return ParseResult::held_unknown;
    Stage reported_stage = Stage::held_unknown;
    if (!stage_from(text(field(root, "state")), reported_stage))
        return ParseResult::held_unknown;
    const bool replay = cJSON_IsTrue(field(root, "replay"));
    if ((!replay && http_status != 202) ||
        (remove && reported_stage != Stage::removing &&
         reported_stage != Stage::removed &&
         reported_stage != Stage::held_unknown) ||
        (!remove && reported_stage != Stage::preparing &&
         reported_stage != Stage::registering &&
         reported_stage != Stage::installed &&
         reported_stage != Stage::held_unknown))
        return ParseResult::held_unknown;
    result.replay = replay;
    if (reported_stage == Stage::held_unknown)
        return ParseResult::held_unknown;
    result.receipt = ReceiptState::needs_status;
    return ParseResult::needs_status;
}

#ifndef SHELF_PS5
ParseResult post_status(Post post, void* context, std::size_t system,
                        Status& result,
                        std::string_view expected_create_nonce,
                        const ConsoleTileIntent* expected_intent) {
    const bool has_nonce = !expected_create_nonce.empty();
    const bool has_intent = expected_intent != nullptr;
    char expected_nonce_copy[33]{};
    ConsoleTileIntent expected_intent_copy{};
    if (has_nonce && !nonce_valid(expected_create_nonce)) {
        result = {};
        return ParseResult::mismatched;
    }
    if (has_nonce)
        copy(expected_create_nonce, expected_nonce_copy,
             sizeof(expected_nonce_copy));
    if (has_intent) expected_intent_copy = *expected_intent;
    result = {};
    char request[128], reply[max_reply];
    int code = -1;
    if (!encode_status(system, request, sizeof(request)) ||
        has_nonce != has_intent ||
        (has_intent &&
         (expected_intent_copy.system != system ||
          !console_tile_intent_valid(expected_intent_copy))))
        return ParseResult::mismatched;
    if (!call(post, context, status_route, request, reply, code))
        return ParseResult::held_unknown;
    if (code != 200) return code == 409 ? ParseResult::conflict
                                        : ParseResult::malformed;
    return parse_status(system, reply, result,
                        has_nonce ? std::string_view(expected_nonce_copy)
                                  : std::string_view(),
                        has_intent ? &expected_intent_copy : nullptr);
}

ParseResult post_create(Post post, void* context,
                        const ConsoleTileIntent& intent,
                        std::string_view nonce, Operation& result) {
    const ConsoleTileIntent intent_copy = intent;
    char nonce_copy[33]{};
    if (!nonce_valid(nonce)) {
        result = {};
        return ParseResult::mismatched;
    }
    copy(nonce, nonce_copy, sizeof(nonce_copy));
    result = {};
    char request[256], reply[max_reply];
    int code = -1;
    if (!encode_create(intent_copy, nonce_copy, request, sizeof(request)))
        return ParseResult::mismatched;
    if (!post) return ParseResult::mismatched;
    if (!call(post, context, create_route, request, reply, code))
        return operation_indeterminate(intent_copy.system, nonce_copy,
                                       nonce_copy, false,
                                       result);
    return parse_operation(intent_copy.system, nonce_copy, nonce_copy, false,
                           code, reply, result);
}

ParseResult post_remove(Post post, void* context, std::size_t system,
                        std::string_view create_nonce,
                        std::string_view remove_nonce, Operation& result) {
    char create_copy[33]{}, remove_copy[33]{};
    if (!nonce_valid(create_nonce) || !nonce_valid(remove_nonce)) {
        result = {};
        return ParseResult::mismatched;
    }
    copy(create_nonce, create_copy, sizeof(create_copy));
    copy(remove_nonce, remove_copy, sizeof(remove_copy));
    result = {};
    char request[256], reply[max_reply];
    int code = -1;
    if (!encode_remove(system, create_copy, remove_copy, request,
                       sizeof(request)))
        return ParseResult::mismatched;
    if (!post) return ParseResult::mismatched;
    if (!call(post, context, remove_route, request, reply, code))
        return operation_indeterminate(system, remove_copy, create_copy, true,
                                       result);
    return parse_operation(system, remove_copy, create_copy, true, code,
                           reply, result);
}
#endif

} // namespace shelf::managed_tile_api_v1
