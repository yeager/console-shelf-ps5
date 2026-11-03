// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_job_routes.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
using namespace shelf;
using namespace shelf::tile_job;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL console tile job: %s\n", message);
        std::exit(1);
    }
}
std::size_t index(std::string_view id) {
    for (std::size_t i = 0; i < system_count; ++i)
        if (catalog[i].id == id) return i;
    return system_count;
}
bool route(const void*, std::size_t system, std::string_view title) {
    return (system == index("ps1") && title == "PPSA99248") ||
           (system == index("ps3") && title == "PPSA99303");
}
Request make_request(std::size_t system, Action action,
                     ConsoleTileDestination destination = ConsoleTileDestination::library,
                     const char* emulator = "") {
    Request request;
    std::memcpy(request.nonce, "0123456789abcdef0123456789abcdef", 33);
    request.action = action;
    request.intent.system = system;
    request.intent.destination = destination;
    std::snprintf(request.intent.emulator_title, sizeof(request.intent.emulator_title),
                  "%s", emulator);
    return request;
}
void round_trip(const Request& source, EmulatorRouteCheck route_check = nullptr) {
    char encoded[maximum_request_bytes + 1]{};
    check(encode_request(source, encoded, sizeof(encoded), route_check),
          "request encode");
    Request parsed;
    check(parse_request(encoded, parsed, route_check), "request parse");
    check(parsed.action == source.action && std::strcmp(parsed.nonce, source.nonce) == 0 &&
          console_tile_intents_equal(parsed.intent, source.intent),
          "request round trip exact");
}
Receipt make_receipt(const Request& request, Result result, const char* title = "") {
    Receipt receipt;
    std::memcpy(receipt.nonce, request.nonce, sizeof(receipt.nonce));
    receipt.action = request.action;
    receipt.intent = request.intent;
    receipt.result = result;
    if (result == Result::held || result == Result::absent ||
        result == Result::retired_source) {
        receipt.intent.destination = ConsoleTileDestination::library;
        receipt.intent.emulator_title[0] = 0;
    }
    if (title) std::snprintf(receipt.title_id, sizeof(receipt.title_id), "%s", title);
    return receipt;
}
void receipt_round_trip(const Request& request, Result result, const char* title,
                        EmulatorRouteCheck route_check = nullptr) {
    const auto source = make_receipt(request, result, title);
    char encoded[maximum_receipt_bytes + 1]{};
    check(encode_receipt(source, encoded, sizeof(encoded)), "receipt encode");
    Receipt parsed;
    check(parse_receipt(encoded, parsed), "receipt parse");
    check(receipt_matches_request(parsed, request, route_check),
          "receipt binds nonce, action, system and route");
}
}

int main() {
    static_assert(system_count == 52, "all catalog consoles must be represented");
    static Library library;
    library.route_count = 1;
    library.routes[0].system = index("ps1");
    std::strcpy(library.routes[0].title_id, "PPSA99248");
    std::strcpy(library.routes[0].folder, "/data/homebrew/PPSA99248");
    std::strcpy(library.routes[0].title_id, "PPSA99303");
    std::strcpy(library.routes[0].folder, "/data/homebrew/PPSA99303");
    check(!installed_emulator_route(&library, index("ps1"), "PPSA99303"),
          "installed but console-mismatched pinned menu is refused");
    std::strcpy(library.routes[0].title_id, "PPSA99248");
    std::strcpy(library.routes[0].folder, "/data/homebrew/PPSA99248");
    check(installed_emulator_route(&library, index("ps1"), "PPSA99248"),
          "installed route matches the requested console");
    check(!installed_emulator_route(&library, index("ps3"), "PPSA99248") &&
          !installed_emulator_route(&library, index("ps1"), "PPSA99303"),
          "installed route rejects mismatched console or title");
    std::strcpy(library.routes[0].folder, "/data/homebrew/other");
    check(!installed_emulator_route(&library, index("ps1"), "PPSA99248"),
          "installed route rejects mismatched title folder");
    std::strcpy(library.routes[0].folder, "/data/homebrew/PPSA99248");
    for (std::size_t system = 0; system < system_count; ++system) {
        round_trip(make_request(system, Action::inspect));
        round_trip(make_request(system, Action::remove));
        auto library_request = make_request(system, Action::upsert);
        round_trip(library_request);
        receipt_round_trip(library_request, Result::upserted_source, owned_title_for(system));
        auto home = make_request(system, Action::upsert, ConsoleTileDestination::home);
        round_trip(home);
        receipt_round_trip(home, Result::upserted_source, owned_title_for(system));
    }
    auto emulator = make_request(index("ps1"), Action::upsert,
                                 ConsoleTileDestination::emulator, "PPSA99248");
    check(intent_valid(emulator.intent, installed_emulator_route, &library),
          "the real library route validates emulator intent");
    check(!intent_valid(emulator.intent, installed_emulator_route, nullptr),
          "missing library route does not validate emulator intent");
    check(!request_valid(emulator) && !encode_request(emulator, nullptr, 0),
          "emulator requests require system-matched route proof");
    round_trip(emulator, route);
    receipt_round_trip(emulator, Result::upserted_source, "PPSA99704", route);
    auto mismatch = emulator;
    mismatch.intent.system = index("ps3");
    check(!request_valid(mismatch, route), "known title mismatched to console is refused");
    char request_wire[maximum_request_bytes + 1]{};
    check(encode_request(emulator, request_wire, sizeof(request_wire), route),
          "valid emulator request encodes");
    auto wrong_system = std::string(request_wire);
    wrong_system.replace(wrong_system.find("system=ps1"), 10, "system=ps3");
    Request ignored;
    check(!parse_request(wrong_system, ignored, route),
          "mismatched system/title is refused on parse");
    for (auto action : {Action::inspect, Action::remove}) {
        auto source = make_request(index("ps1"), action);
        check(encode_request(source, request_wire, sizeof(request_wire)),
              "valid status request encodes");
        auto unknown = std::string(request_wire);
        unknown.replace(unknown.find("system=ps1"), 10, "system=xxxx");
        check(!parse_request(unknown, ignored),
              "unknown inspect/remove console cannot fall back to catalog index zero");
    }
    auto home = make_request(index("ps1"), Action::upsert,
                             ConsoleTileDestination::home);
    check(encode_request(home, request_wire, sizeof(request_wire)), "home request encodes");
    const std::string valid(request_wire);
    for (auto broken : {
        valid + "extra=1\n",
        std::string(valid).replace(valid.find("nonce=0"), 7, "nonce=/"),
        std::string(valid).replace(valid.find("action=upsert"), 13, "action=delete"),
        std::string(valid).replace(valid.find("destination=home"), 16, "destination=path"),
        std::string(valid).replace(valid.find("format="), 7, "format=x"),
    })
        check(!parse_request(broken, ignored), "malformed request refused");

    receipt_round_trip(make_request(index("ps1"), Action::inspect),
                       Result::absent, "");
    receipt_round_trip(make_request(index("ps1"), Action::inspect),
                       Result::owned_source, "PPSA99704");
    receipt_round_trip(make_request(index("ps1"), Action::remove),
                       Result::retired_source, "PPSA99704");
    receipt_round_trip(home, Result::held, "");
    auto applied = make_receipt(home, Result::upserted_source, "PPSA99704");
    check(receipt_valid(applied), "owned source receipt valid");
    auto wrong_nonce = applied;
    wrong_nonce.nonce[0] = 'a';
    check(!receipt_matches_request(wrong_nonce, home), "wrong nonce cannot prove source");
    auto wrong_route = applied;
    wrong_route.intent.destination = ConsoleTileDestination::library;
    check(!receipt_matches_request(wrong_route, home), "wrong destination cannot prove source");
    auto wrong_title = applied;
    std::memcpy(wrong_title.title_id, "CUSA12345", 10);
    check(!receipt_valid(wrong_title), "foreign title ID cannot prove owned source");
    std::memcpy(wrong_title.title_id, "PPSA99702", 10);
    check(!receipt_valid(wrong_title), "another console's valid PPSA title cannot prove owned source");
    auto wrong_status = make_receipt(home, Result::absent, "");
    check(!receipt_valid(wrong_status), "upsert cannot claim absent status");
    char receipt_wire[maximum_receipt_bytes + 1]{};
    check(encode_receipt(applied, receipt_wire, sizeof(receipt_wire)),
          "valid receipt encodes");
    auto missing_route = std::string(receipt_wire);
    missing_route.replace(missing_route.find("destination=home"), 16, "destination=-");
    Receipt ignored_receipt;
    check(!parse_receipt(missing_route, ignored_receipt),
          "successful source receipt must include a route");

    Phase phase = Phase::none;
    check(advance(Action::upsert, phase, Observation::request_created) && phase == Phase::queued,
          "request queued");
    check(!advance(Action::upsert, phase, Observation::api_registered),
          "registration alone cannot bypass helper");
    check(advance(Action::upsert, phase, Observation::send_attempted) &&
          phase == Phase::sent_unverified, "one send attempted");
    check(!advance(Action::upsert, phase, Observation::owner_tile_visible),
          "TV report before bound receipt refused");
    check(observe_receipt(home, applied, phase) && phase == Phase::source_observed,
          "matching receipt proves source only");
    check(advance(Action::upsert, phase, Observation::api_registered) &&
          phase == Phase::registered_unconfirmed, "API registration remains unconfirmed");
    check(advance(Action::upsert, phase, Observation::owner_tile_visible) &&
          phase == Phase::tv_confirmed, "owner confirms visible tile");

    phase = Phase::sent_unverified;
    check(!observe_receipt(home, wrong_nonce, phase) && phase == Phase::held,
          "wrong nonce holds the job");
    phase = Phase::sent_unverified;
    check(observe_receipt(home, make_receipt(home, Result::held), phase) &&
          phase == Phase::held, "explicit helper hold cannot become success");
    auto remove = make_request(index("ps1"), Action::remove);
    phase = Phase::sent_unverified;
    check(observe_receipt(remove, make_receipt(remove, Result::retired_source, "PPSA99704"),
                          phase) && phase == Phase::removal_pending,
          "retired source does not prove tile absent");
    check(!advance(Action::remove, phase, Observation::owner_tile_absent),
          "owner observation alone does not finish removal");
    check(advance(Action::remove, phase, Observation::api_absent) &&
          phase == Phase::removal_api_absent, "API absence precedes owner confirmation");
    check(advance(Action::remove, phase, Observation::owner_tile_absent) &&
          phase == Phase::removed_confirmed, "owner confirms removal");
    auto inspect = make_request(index("ps1"), Action::inspect);
    phase = Phase::sent_unverified;
    check(observe_receipt(inspect, make_receipt(inspect, Result::absent), phase) &&
          phase == Phase::inspected_absent, "read-only absence is a distinct status");

    std::puts("PASS: 52-console job/receipt contract, route match and evidence phases");
}
