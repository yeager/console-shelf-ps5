// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/managed_tile_api_v1.hpp"
#include "cjson/cJSON.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
using namespace shelf;
using namespace shelf::managed_tile_api_v1;
constexpr char create_nonce[] = "0123456789abcdef0123456789abcdef";
constexpr char remove_nonce[] = "fedcba9876543210fedcba9876543210";

void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "managed API v1: %s\n", message); std::exit(1); }
}

std::size_t system_for(std::string_view id) {
    for (std::size_t i = 0; i < system_count; ++i)
        if (catalog[i].id == id) return i;
    std::abort();
}

struct Fake {
    int http = 200;
    std::string body;
    std::string route;
    std::string request;
    int calls = 0;
    char* mutate_first = nullptr;
    char* mutate_second = nullptr;
};

int post(void* context, const char* route, const char* request,
         char* response, std::size_t capacity) {
    auto& fake = *static_cast<Fake*>(context);
    ++fake.calls;
    fake.route = route;
    fake.request = request;
    if (fake.mutate_first) fake.mutate_first[0] = 'a';
    if (fake.mutate_second) fake.mutate_second[0] = 'a';
    if (fake.http < 0) return fake.http;
    check(fake.body.size() < capacity, "fixture response fits");
    std::memcpy(response, fake.body.c_str(), fake.body.size() + 1);
    return fake.http;
}

std::string status_body(const char* title, const char* state,
                        const char* nonce = create_nonce,
                        const char* source = "true",
                        const char* mount = "null",
                        const char* registration = "null",
                        const char* reason = "null") {
    const std::string intent = nonce
        ? "\"create_nonce\":\"" + std::string(nonce) +
              "\",\"route\":\"library\",\"emulator_title_id\":\"-\","
        : "\"create_nonce\":null,\"route\":null,\"emulator_title_id\":null,";
    return "{\"status\":0,\"contract_version\":1,\"title_id\":\"" +
           std::string(title) + "\",\"state\":\"" + state + "\"," + intent +
           "\"source_verified\":" + source + ",\"mount_verified\":" +
           mount + ",\"registration_verified\":" + registration +
           ",\"reason\":" + reason + "}";
}

std::string operation_body(const char* title, const char* operation,
                           const char* nonce, const char* state,
                           bool replay = false) {
    return "{\"status\":0,\"contract_version\":1,\"title_id\":\"" +
           std::string(title) + "\",\"operation\":\"" + operation +
           "\",\"nonce\":\"" + nonce + "\",\"state\":\"" + state +
           "\",\"replay\":" + (replay ? "true" : "false") + "}";
}

bool receipt_for(const Operation& operation, const char* title,
                 const char* nonce, const char* create_generation,
                 bool remove, ReceiptState receipt) {
    return operation.receipt == receipt && operation.remove == remove &&
           std::strcmp(operation.title_id, title) == 0 &&
           std::strcmp(operation.nonce, nonce) == 0 &&
           std::strcmp(operation.create_nonce, create_generation) == 0;
}

void catalog_and_requests() {
    char request[256];
    int eligible = 0;
    for (std::size_t system = 0; system < system_count; ++system) {
        check(encode_status(system, request, sizeof(request)),
              "every fixed catalog title is status-readable");
        const char* title = tile_job::owned_title_for(system);
        check(title && std::strstr(request, title), "status uses pinned title");
        ConsoleTileIntent intent{system, ConsoleTileDestination::library, {}};
        const bool eligible_here = mutable_title(system);
        eligible += eligible_here;
        check(encode_create(intent, create_nonce, request, sizeof(request)) ==
                  eligible_here, "create eligibility tracks pinned source");
        if (eligible_here) {
            cJSON* parsed = cJSON_Parse(request);
            check(parsed && std::strcmp(cJSON_GetObjectItemCaseSensitive(
                        parsed, "title_id")->valuestring, title) == 0 &&
                  std::strcmp(cJSON_GetObjectItemCaseSensitive(
                        parsed, "system_id")->valuestring,
                        std::string(catalog[system].id).c_str()) == 0,
                  "create pairs exact title and system");
            cJSON_Delete(parsed);
            check(encode_remove(system, create_nonce, remove_nonce,
                                request, sizeof(request)),
                  "new title remove request encodes both nonces");
        } else {
            check(!encode_remove(system, create_nonce, remove_nonce,
                                 request, sizeof(request)),
                  "four pilots cannot be mutated");
        }
    }
    check(eligible == 48, "exactly 48 new titles are mutable");
    check(!encode_status(system_count, request, sizeof(request)),
          "out-of-catalog status is refused");
    check(!nonce_valid("0123456789abcdef0123456789abcdeF") &&
          !nonce_valid("0123456789abcdef0123456789abcde") &&
          nonce_valid(create_nonce), "nonce requires exact lowercase hex");
    ConsoleTileIntent emulator{system_for("snes"),
                               ConsoleTileDestination::emulator, {}};
    bool found = false;
    for (const auto& menu : isshelf_badge::generic_tile::menus) {
        if (menu.system != emulator.system) continue;
        std::snprintf(emulator.emulator_title, sizeof(emulator.emulator_title),
                      "%s", menu.title_id);
        found = true;
        break;
    }
    check(found && encode_create(emulator, create_nonce, request,
                                 sizeof(request)) &&
          std::strstr(request, emulator.emulator_title),
          "pinned emulator menu route encodes");
    std::snprintf(emulator.emulator_title, sizeof(emulator.emulator_title),
                  "PPSA00000");
    check(!encode_create(emulator, create_nonce, request, sizeof(request)),
          "foreign emulator is refused");
    check(!encode_create({system_for("snes"),
                          ConsoleTileDestination::library, {}},
                         "not-a-nonce", request, sizeof(request)),
          "bad nonce is refused before send");
}

void capability_and_status() {
    const std::string enabled =
        "{\"status\":0,\"api_version\":1,\"capabilities\":["
        "\"web_ui\",\"isshelf_managed_tiles_v1\"],"
        "\"isshelf_managed_tiles\":{\"contract_version\":1,"
        "\"enabled\":true}}";
    check(parse_capability(enabled), "enabled v1 capability parses");
    check(!parse_capability(std::string(enabled).replace(
              enabled.find("\"enabled\":true"), 14, "\"enabled\":false")),
          "disabled capability cannot be accepted");
    check(!parse_capability(std::string(enabled).replace(
              enabled.find("\"api_version\":1"), 15,
              "\"api_version\":1,\"api_version\":1")),
          "duplicate version key refused");
    const auto system = system_for("snes");
    const auto title = tile_job::owned_title_for(system);
    ConsoleTileIntent expected{system, ConsoleTileDestination::library, {}};
    Status status;
    check(parse_status(system, status_body(title, "registering"), status,
                       create_nonce, &expected) == ParseResult::valid &&
          status.stage == Stage::registering &&
          status.source_verified == Proof::yes &&
          status.mount_verified == Proof::unproved,
          "async registering keeps unproved mount distinct from false");
    check(parse_status(system,
          status_body(title, "installed", create_nonce, "true", "true",
                      "true"), status, status.create_nonce,
          &status.intent) == ParseResult::valid &&
          status.stage == Stage::installed,
          "in-place status keeps the original route and nonce expectations");
    check(parse_status(system,
          status_body(title, "installed", create_nonce, "true", "true",
                      "true"), status, create_nonce, &expected) ==
                  ParseResult::valid &&
          status.stage == Stage::installed,
          "installed requires three proofs");
    auto wrong_route = status_body(title, "installed", create_nonce,
                                   "true", "true", "true");
    const auto route_at = wrong_route.find("\"route\":\"library\"");
    check(route_at != std::string::npos, "route fixture contains library");
    wrong_route.replace(route_at, std::strlen("\"route\":\"library\""),
                        "\"route\":\"home\"");
    check(parse_status(system, wrong_route, status, create_nonce, &expected) ==
                  ParseResult::mismatched,
          "same-generation installed reply for another route is refused");
    check(parse_status(system, status_body(title, "registering"), status,
                       create_nonce) == ParseResult::mismatched &&
          parse_status(system, status_body(title, "registering"), status,
                       {}, &expected) == ParseResult::mismatched,
          "generation-specific status requires nonce and route together");
    ConsoleTileIntent wrong_system{system_for("nes"),
                                   ConsoleTileDestination::library, {}};
    check(parse_status(system, status_body(title, "registering"), status,
                       create_nonce, &wrong_system) == ParseResult::mismatched,
          "expected intent must belong to the requested system");
    check(parse_status(system,
          status_body(title, "installed", create_nonce, "true", "null",
                      "true"), status) == ParseResult::malformed,
          "installed with unproved mount is refused");
    check(parse_status(system,
          status_body(title, "held_unknown", create_nonce, "true", "null",
                      "null", "\"journal_uncertain\""), status) ==
                  ParseResult::held_unknown &&
          status.stage == Stage::held_unknown,
          "ambiguous transaction is preserved, never success");
    check(parse_status(system, status_body(title, "conflict", create_nonce,
          "null", "null", "null", "\"foreign_title\""), status) ==
                  ParseResult::conflict, "foreign title is conflict");
    check(parse_status(system,
          status_body(title, "absent", nullptr, "false", "false", "false"),
          status, create_nonce, &expected) == ParseResult::valid &&
          status.stage == Stage::absent &&
          !status.has_intent, "proven absent has no adopted intent");
    check(parse_status(system,
          status_body(title, "absent", nullptr, "null", "false", "false"),
          status) == ParseResult::malformed,
          "unproved source cannot be reported absent");
    auto removed = status_body(title, "removed", create_nonce, "true",
                               "false", "false");
    removed.insert(removed.size() - 1, ",\"source_retained\":true");
    check(parse_status(system, removed, status, create_nonce, &expected) ==
                  ParseResult::valid && status.source_retained,
          "removed retains source evidence but mount and registration absent");
    check(parse_status(system, status_body(title, "removed", create_nonce,
          "true", "false", "false"), status, create_nonce, &expected) ==
                  ParseResult::malformed,
          "removed with verified source requires retained-source flag");
    auto unreported_source = status_body(title, "removed", create_nonce,
                                         "true", "false", "false");
    unreported_source.insert(unreported_source.size() - 1,
                             ",\"source_retained\":false");
    check(parse_status(system, unreported_source, status, create_nonce,
                       &expected) ==
                  ParseResult::malformed,
          "removed cannot claim verified source without retention");
    check(parse_status(system, status_body("PPSA00000", "registering"),
                       status) == ParseResult::mismatched,
          "cross-title reply is refused");
    check(parse_status(system, status_body(title, "registering"), status,
                       remove_nonce, &expected) == ParseResult::mismatched,
          "cross-generation status is refused");
    auto duplicate = status_body(title, "registering");
    duplicate.insert(1, "\"title_id\":\"PPSA00000\",");
    check(parse_status(system, duplicate, status) == ParseResult::malformed,
          "duplicate response key is refused");
}

void transport_and_operations() {
    const auto system = system_for("snes");
    const auto title = tile_job::owned_title_for(system);
    ConsoleTileIntent intent{system, ConsoleTileDestination::library, {}};
    Fake fake;
    Operation operation;
    fake.http = 202;
    fake.body = operation_body(title, "create", create_nonce, "preparing");
    check(post_create(post, &fake, intent, create_nonce, operation) ==
                  ParseResult::needs_status && fake.calls == 1 &&
          fake.route == create_route &&
          receipt_for(operation, title, create_nonce, create_nonce, false,
                      ReceiptState::needs_status) &&
          fake.request.find("\"system_id\":\"snes\"") != std::string::npos,
          "CREATE POST receipt keeps identity but requires status");
    fake.http = 200;
    fake.body = operation_body(title, "create", create_nonce, "registering", true);
    check(post_create(post, &fake, intent, create_nonce, operation) ==
                  ParseResult::needs_status && operation.replay &&
          receipt_for(operation, title, create_nonce, create_nonce, false,
                      ReceiptState::needs_status),
          "same-nonce replay still requires status");
    fake.body = operation_body(title, "create", create_nonce, "installed", true);
    check(post_create(post, &fake, intent, create_nonce, operation) ==
                  ParseResult::needs_status && operation.replay &&
          receipt_for(operation, title, create_nonce, create_nonce, false,
                      ReceiptState::needs_status),
          "terminal CREATE replay is only a receipt");
    Status status;
    check(parse_status(system,
          status_body(title, "installed", create_nonce, "true", "null",
                      "true"), status, operation.create_nonce, &intent) ==
                  ParseResult::malformed,
          "terminal CREATE receipt cannot replace mounted-view status proof");
    check(parse_status(system,
          status_body(title, "installed", create_nonce, "true", "true",
                      "true"), status, operation.create_nonce, &intent) ==
                  ParseResult::valid && status.stage == Stage::installed,
          "exact route and generation allow separately proved installed status");
    fake.body = operation_body(title, "create", remove_nonce, "preparing");
    check(post_create(post, &fake, intent, create_nonce, operation) ==
                  ParseResult::held_unknown &&
          receipt_for(operation, title, create_nonce, create_nonce, false,
                      ReceiptState::held_unknown),
          "cross-nonce CREATE receipt retains original nonce");
    fake.http = 409;
    fake.body = "{\"status\":17,\"error_reason\":\"nonce_conflict\"}";
    check(parse_operation(system, create_nonce, create_nonce, false, fake.http,
                          fake.body,
                          operation) == ParseResult::held_unknown &&
          receipt_for(operation, title, create_nonce, create_nonce, false,
                      ReceiptState::held_unknown),
          "direct operation parser retains nonce after error response");
    check(post_create(post, &fake, intent, create_nonce, operation) ==
                  ParseResult::held_unknown &&
          receipt_for(operation, title, create_nonce, create_nonce, false,
                      ReceiptState::held_unknown),
          "sent CREATE conflict remains indeterminate until status");
    fake.http = 202;
    fake.body = "{\"status\":0";
    check(post_create(post, &fake, intent, create_nonce, operation) ==
                  ParseResult::held_unknown &&
          receipt_for(operation, title, create_nonce, create_nonce, false,
                      ReceiptState::held_unknown),
          "malformed CREATE receipt retains nonce for reconciliation");
    fake.http = 500;
    fake.body = "{\"status\":5,\"error_reason\":\"journal_uncertain\"}";
    check(post_create(post, &fake, intent, create_nonce, operation) ==
                  ParseResult::held_unknown &&
          receipt_for(operation, title, create_nonce, create_nonce, false,
                      ReceiptState::held_unknown),
          "sent CREATE error retains nonce for reconciliation");
    fake.http = -1;
    check(post_create(post, &fake, intent, create_nonce, operation) ==
                  ParseResult::held_unknown &&
          receipt_for(operation, title, create_nonce, create_nonce, false,
                      ReceiptState::held_unknown),
          "CREATE transport uncertainty keeps nonce for reconciliation");
    char changed_create_nonce[sizeof(create_nonce)];
    std::memcpy(changed_create_nonce, create_nonce, sizeof(create_nonce));
    Fake changed_create;
    changed_create.http = -1;
    changed_create.mutate_first = changed_create_nonce;
    check(post_create(post, &changed_create, intent, changed_create_nonce,
                      operation) == ParseResult::held_unknown &&
          changed_create_nonce[0] == 'a' &&
          receipt_for(operation, title, create_nonce, create_nonce, false,
                      ReceiptState::held_unknown),
          "CREATE receipt retains sent nonce if callback changes caller buffer");
    const int calls_before_invalid = fake.calls;
    check(post_create(post, &fake, intent, "invalid", operation) ==
                  ParseResult::mismatched && fake.calls == calls_before_invalid,
          "invalid CREATE is refused before POST");
    check(post_create(nullptr, &fake, intent, create_nonce, operation) ==
                  ParseResult::mismatched && fake.calls == calls_before_invalid,
          "missing CREATE sender is refused before POST");
    fake.http = 202;
    fake.body = operation_body(title, "remove", remove_nonce, "removing");
    check(post_remove(post, &fake, system, create_nonce, remove_nonce,
                      operation) == ParseResult::needs_status &&
          receipt_for(operation, title, remove_nonce, create_nonce, true,
                      ReceiptState::needs_status) &&
          fake.route == remove_route &&
          fake.request.find("expected_create_nonce") != std::string::npos,
          "REMOVE receipt keeps both nonces and requires status");
    fake.http = 200;
    fake.body = operation_body(title, "remove", remove_nonce, "removed", true);
    check(post_remove(post, &fake, system, create_nonce, remove_nonce,
                      operation) == ParseResult::needs_status &&
          operation.replay &&
          receipt_for(operation, title, remove_nonce, create_nonce, true,
                      ReceiptState::needs_status),
          "terminal REMOVE replay is only a receipt");
    check(parse_status(system,
          status_body(title, "removed", create_nonce, "true", "false",
                      "true"), status, operation.create_nonce, &intent) ==
                  ParseResult::malformed,
          "terminal REMOVE receipt cannot replace registration absence proof");
    auto removed_status = status_body(title, "removed", create_nonce, "true",
                                      "false", "false");
    removed_status.insert(removed_status.size() - 1,
                          ",\"source_retained\":true");
    check(parse_status(system, removed_status, status,
                       operation.create_nonce, &intent) == ParseResult::valid &&
          status.stage == Stage::removed && status.source_retained,
          "exact route and create generation allow separately proved removal");
    auto removed_wrong_route = removed_status;
    const auto removed_route_at = removed_wrong_route.find("\"route\":\"library\"");
    check(removed_route_at != std::string::npos,
          "removed fixture contains expected route");
    removed_wrong_route.replace(removed_route_at,
                                std::strlen("\"route\":\"library\""),
                                "\"route\":\"home\"");
    check(parse_status(system, removed_wrong_route, status,
                       operation.create_nonce, &intent) ==
                  ParseResult::mismatched &&
          parse_status(system, removed_status, status, remove_nonce,
                       &intent) == ParseResult::mismatched,
          "REMOVE reconciliation rejects wrong route or create generation");
    fake.http = 202;
    fake.body = operation_body(title, "remove", remove_nonce,
                               "held_unknown");
    check(post_remove(post, &fake, system, create_nonce, remove_nonce,
                      operation) == ParseResult::held_unknown &&
          receipt_for(operation, title, remove_nonce, create_nonce, true,
                      ReceiptState::held_unknown),
          "REMOVE held state is not a success");
    fake.body = operation_body(title, "remove", create_nonce, "removing");
    check(post_remove(post, &fake, system, create_nonce, remove_nonce,
                      operation) == ParseResult::held_unknown &&
          receipt_for(operation, title, remove_nonce, create_nonce, true,
                      ReceiptState::held_unknown),
          "cross-nonce REMOVE receipt retains both original nonces");
    fake.http = 409;
    fake.body = "{\"status\":17,\"error_reason\":\"nonce_conflict\"}";
    check(post_remove(post, &fake, system, create_nonce, remove_nonce,
                      operation) == ParseResult::held_unknown &&
          receipt_for(operation, title, remove_nonce, create_nonce, true,
                      ReceiptState::held_unknown),
          "sent REMOVE conflict remains indeterminate until status");
    fake.http = 202;
    fake.body = "{\"status\":0";
    check(post_remove(post, &fake, system, create_nonce, remove_nonce,
                      operation) == ParseResult::held_unknown &&
          receipt_for(operation, title, remove_nonce, create_nonce, true,
                      ReceiptState::held_unknown),
          "malformed REMOVE receipt retains nonce for reconciliation");
    fake.http = 500;
    fake.body = "{\"status\":5,\"error_reason\":\"journal_uncertain\"}";
    check(post_remove(post, &fake, system, create_nonce, remove_nonce,
                      operation) == ParseResult::held_unknown &&
          receipt_for(operation, title, remove_nonce, create_nonce, true,
                      ReceiptState::held_unknown),
          "sent REMOVE error retains nonce for reconciliation");
    fake.http = -1;
    check(post_remove(post, &fake, system, create_nonce, remove_nonce,
                      operation) == ParseResult::held_unknown &&
          receipt_for(operation, title, remove_nonce, create_nonce, true,
                      ReceiptState::held_unknown),
          "REMOVE transport uncertainty keeps nonce for reconciliation");
    char changed_remove_create[sizeof(create_nonce)];
    char changed_remove_nonce[sizeof(remove_nonce)];
    std::memcpy(changed_remove_create, create_nonce, sizeof(create_nonce));
    std::memcpy(changed_remove_nonce, remove_nonce, sizeof(remove_nonce));
    Fake changed_remove;
    changed_remove.http = -1;
    changed_remove.mutate_first = changed_remove_create;
    changed_remove.mutate_second = changed_remove_nonce;
    check(post_remove(post, &changed_remove, system, changed_remove_create,
                      changed_remove_nonce, operation) ==
                  ParseResult::held_unknown &&
          changed_remove_create[0] == 'a' && changed_remove_nonce[0] == 'a' &&
          receipt_for(operation, title, remove_nonce, create_nonce, true,
                      ReceiptState::held_unknown),
          "REMOVE receipt retains sent create and remove nonces");
    const int calls_before_invalid_remove = fake.calls;
    check(post_remove(post, &fake, system, "invalid", remove_nonce,
                      operation) == ParseResult::mismatched &&
          fake.calls == calls_before_invalid_remove,
          "invalid REMOVE is refused before POST");
    check(post_remove(nullptr, &fake, system, create_nonce, remove_nonce,
                      operation) == ParseResult::mismatched &&
          fake.calls == calls_before_invalid_remove,
          "missing REMOVE sender is refused before POST");
    fake.http = 200;
    fake.body = status_body(title, "registering");
    const int calls_before_unpaired_status = fake.calls;
    check(post_status(post, &fake, system, status, create_nonce) ==
                  ParseResult::mismatched &&
          post_status(post, &fake, system, status, {}, &intent) ==
                  ParseResult::mismatched &&
          fake.calls == calls_before_unpaired_status,
          "unpaired status expectation is refused before POST");
    check(post_status(post, &fake, system, status, create_nonce, &intent) ==
                  ParseResult::valid && fake.route == status_route,
          "status transport parses the queued operation");
    fake.body = status_body(title, "installed", create_nonce, "true", "true",
                            "true");
    check(post_status(post, &fake, system, status, status.create_nonce,
                      &status.intent) == ParseResult::valid &&
          status.stage == Stage::installed,
          "status transport snapshots in-place nonce and route before clearing output");
    const auto route_at = fake.body.find("\"route\":\"library\"");
    check(route_at != std::string::npos, "transport fixture contains route");
    fake.body.replace(route_at, std::strlen("\"route\":\"library\""),
                      "\"route\":\"home\"");
    check(post_status(post, &fake, system, status, create_nonce, &intent) ==
                  ParseResult::mismatched,
          "status transport refuses same-generation wrong route");
    fake.body = status_body(title, "absent", nullptr, "false", "false",
                            "false");
    check(post_status(post, &fake, system, status, create_nonce, &intent) ==
                  ParseResult::valid && status.stage == Stage::absent,
          "status transport accepts independently proven absence");
    fake.http = -1;
    check(post_status(post, &fake, system, status, create_nonce, &intent) ==
                  ParseResult::held_unknown &&
          status.stage == Stage::held_unknown,
          "status transport failure cannot imply absence");
}
} // namespace

int main() {
    catalog_and_requests();
    capability_and_status();
    transport_and_operations();
    std::puts("managed API v1: 52 catalog, 48 mutable, async and failure fixtures passed");
}
