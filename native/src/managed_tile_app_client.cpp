// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/managed_tile_app_client.hpp"
#include <cstring>

namespace shelf::managed_tile_app_client {
namespace {
constexpr std::size_t reply_capacity = 4096;
} // namespace

bool Client::ready() const {
    return enabled_ && binding_.service_identity_verified &&
           binding_.credential_bound && binding_.capability_verified &&
           binding_.send;
}

StatusRead Client::read(std::size_t system, const char* nonce,
                        const ConsoleTileIntent* intent) const {
    StatusRead result;
    char request[128]{};
    char expected_nonce[33]{};
    ConsoleTileIntent expected_intent{};
    const bool has_nonce = nonce != nullptr;
    const bool has_intent = intent != nullptr;
    if (has_nonce && nonce[32] == 0 &&
        managed_tile_api_v1::nonce_valid(std::string_view(nonce, 32)))
        std::memcpy(expected_nonce, nonce, sizeof(expected_nonce));
    if (has_intent) expected_intent = *intent;
    if (!managed_tile_api_v1::encode_status(system, request,
                                             sizeof(request)) ||
        has_nonce != has_intent ||
        (has_nonce && !managed_tile_api_v1::nonce_valid(expected_nonce)) ||
        (has_intent && (expected_intent.system != system ||
                        !console_tile_intent_valid(expected_intent)))) {
        result.state = State::invalid_request;
        return result;
    }
    if (!ready()) return result;
    char reply[reply_capacity]{};
    std::size_t reply_bytes = 0;
    const int http = binding_.send(binding_.context,
        managed_tile_api_v1::status_route, request, reply, sizeof(reply),
        reply_bytes);
    if (http < 0 || reply_bytes == 0 || reply_bytes > sizeof(reply)) {
        result.state = State::transport_unknown;
        return result;
    }
    if (http != 200) {
        result.state = http == 409 ? State::conflict : State::reply_unknown;
        return result;
    }
    const auto parsed = managed_tile_api_v1::parse_status(system,
        std::string_view(reply, reply_bytes),
        result.status, has_nonce ? expected_nonce : "",
        has_intent ? &expected_intent : nullptr);
    result.state = parsed == managed_tile_api_v1::ParseResult::valid
                       ? State::observed
                       : parsed == managed_tile_api_v1::ParseResult::conflict
                           ? State::conflict
                           : State::reply_unknown;
    return result;
}

StatusRead Client::status(std::size_t system) const {
    return read(system, nullptr, nullptr);
}

CreateAttempt Client::create(const ConsoleTileIntent& intent,
                             const Library* library) const {
    CreateAttempt result;
    result.intent = intent;
    if (!managed_tile_api_v1::mutable_title(result.intent.system) ||
        !tile_job::intent_valid(result.intent, tile_job::installed_emulator_route,
                                library)) {
        result.state = State::invalid_request;
        return result;
    }
    if (!ready()) return result;
    char generated_nonce[33]{};
    if (!binding_.nonce ||
        !binding_.nonce(binding_.context, generated_nonce,
                        sizeof(generated_nonce)) ||
        generated_nonce[32] != 0 ||
        !managed_tile_api_v1::nonce_valid(
            std::string_view(generated_nonce, 32))) {
        result.state = State::nonce_unavailable;
        return result;
    }
    std::memcpy(result.nonce, generated_nonce, sizeof(result.nonce));
    char request[256]{};
    if (!managed_tile_api_v1::encode_create(result.intent, result.nonce,
                                              request, sizeof(request))) {
        result.state = State::invalid_request;
        return result;
    }
    ConsoleTileIntent journal_intent = result.intent;
    char journal_nonce[33]{};
    std::memcpy(journal_nonce, result.nonce, sizeof(journal_nonce));
    if (!binding_.journal ||
        !binding_.journal(binding_.context, journal_intent, journal_nonce) ||
        journal_intent.system != result.intent.system ||
        journal_intent.destination != result.intent.destination ||
        std::memcmp(journal_intent.emulator_title,
                    result.intent.emulator_title,
                    sizeof(result.intent.emulator_title)) != 0 ||
        std::memcmp(journal_nonce, result.nonce, sizeof(journal_nonce)) != 0) {
        result.state = State::journal_unavailable;
        return result;
    }
    result.journaled = true;
    result.may_have_sent = true;
    char reply[reply_capacity]{};
    std::size_t reply_bytes = 0;
    const int http = binding_.send(binding_.context,
        managed_tile_api_v1::create_route, request, reply, sizeof(reply),
        reply_bytes);
    if (http < 0 || reply_bytes == 0 || reply_bytes > sizeof(reply)) {
        result.state = State::transport_unknown;
        return result;
    }
    const auto parsed = managed_tile_api_v1::parse_operation(result.intent.system,
        result.nonce, result.nonce, false, http,
        std::string_view(reply, reply_bytes), result.receipt);
    result.state = parsed == managed_tile_api_v1::ParseResult::needs_status
                       ? State::receipt_needs_status : State::reply_unknown;
    return result;
}

StatusRead Client::reconcile(const CreateAttempt& attempt) const {
    StatusRead result;
    if (!attempt.journaled || !attempt.may_have_sent ||
        !managed_tile_api_v1::mutable_title(attempt.intent.system) ||
        attempt.nonce[32] != 0 ||
        !managed_tile_api_v1::nonce_valid(
            std::string_view(attempt.nonce, 32)) ||
        !console_tile_intent_valid(attempt.intent)) {
        result.state = State::invalid_request;
        return result;
    }
    result = read(attempt.intent.system, attempt.nonce, &attempt.intent);
    if (result.state != State::observed) return result;
    if (result.status.stage == managed_tile_api_v1::Stage::installed)
        result.state = State::installed_reported;
    else if (result.status.stage == managed_tile_api_v1::Stage::preparing ||
             result.status.stage == managed_tile_api_v1::Stage::registering)
        result.state = State::pending;
    else
        result.state = State::reply_unknown;
    return result;
}

} // namespace shelf::managed_tile_app_client
