// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/managed_tile_app_client.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
using namespace shelf;
using namespace shelf::managed_tile_app_client;
constexpr char nonce_value[] = "0123456789abcdef0123456789abcdef";

void check(bool okay, const char* why) {
    if (!okay) { std::fprintf(stderr, "managed app client: %s\n", why); std::exit(1); }
}

struct Fake {
    std::size_t system = 0;
    int calls = 0, journal_calls = 0, nonce_calls = 0;
    int http = 200;
    bool journal_ok = true, nonce_ok = true;
    bool mutate_journal_intent = false, mutate_journal_nonce = false;
    bool mutate_journal_unterminated = false;
    ConsoleTileIntent* mutate_input = nullptr;
    CreateAttempt* mutate_attempt = nullptr;
    enum class Reply { absent, create, registering, installed, wrong_route, wrong_nonce,
                       broken, trailing_bytes } reply = Reply::absent;
    std::string last_route, last_request;
};

bool new_nonce(void* context, char* output, std::size_t capacity) {
    auto& fake = *static_cast<Fake*>(context);
    ++fake.nonce_calls;
    if (!fake.nonce_ok || capacity < sizeof(nonce_value)) return false;
    std::memcpy(output, nonce_value, sizeof(nonce_value));
    return true;
}

bool journal(void* context, const ConsoleTileIntent& intent, const char* nonce) {
    auto& fake = *static_cast<Fake*>(context);
    ++fake.journal_calls;
    check(intent.system == fake.system && std::strcmp(nonce, nonce_value) == 0,
          "journal receives exact intent and nonce");
    if (fake.mutate_journal_intent)
        const_cast<ConsoleTileIntent&>(intent).destination =
            ConsoleTileDestination::home;
    if (fake.mutate_journal_nonce)
        const_cast<char*>(nonce)[0] = 'f';
    if (fake.mutate_journal_unterminated)
        std::memset(const_cast<ConsoleTileIntent&>(intent).emulator_title,
                    'P', sizeof(intent.emulator_title));
    return fake.journal_ok;
}

int send(void* context, const char* route, const char* request,
         char* response, std::size_t capacity, std::size_t& response_bytes) {
    auto& fake = *static_cast<Fake*>(context);
    ++fake.calls;
    fake.last_route = route;
    fake.last_request = request;
    if (fake.mutate_input)
        fake.mutate_input->destination = ConsoleTileDestination::home;
    if (fake.mutate_attempt && fake.last_route ==
                                   managed_tile_api_v1::status_route) {
        fake.mutate_attempt->intent.destination =
            ConsoleTileDestination::home;
        fake.mutate_attempt->nonce[0] = 'f';
    }
    if (fake.http < 0) return fake.http;
    const auto* title = tile_job::owned_title_for(fake.system);
    check(title != nullptr && fake.last_request.find(title) != std::string::npos,
          "request binds fixed catalog title");
    const std::string identity = "\"create_nonce\":\"" +
        std::string(fake.reply == Fake::Reply::wrong_nonce
                        ? "fedcba9876543210fedcba9876543210" : nonce_value) +
        "\",\"route\":\"" +
        (fake.reply == Fake::Reply::wrong_route ? "home" : "library") +
        "\",\"emulator_title_id\":\"-\",";
    std::string body;
    if (fake.reply == Fake::Reply::create)
        body = "{\"status\":0,\"contract_version\":1,\"title_id\":\"" +
               std::string(title) + "\",\"operation\":\"create\",\"nonce\":\"" +
               nonce_value + "\",\"state\":\"installed\",\"replay\":true}";
    else if (fake.reply == Fake::Reply::broken) body = "{broken";
    else {
        const bool absent = fake.reply == Fake::Reply::absent;
        const bool registering = fake.reply == Fake::Reply::registering;
        body = "{\"status\":0,\"contract_version\":1,\"title_id\":\"" +
               std::string(title) + "\",\"state\":\"" +
               (absent ? "absent" : registering ? "registering" : "installed") + "\"," +
               (absent ? "\"create_nonce\":null,\"route\":null,\"emulator_title_id\":null,"
                       : identity) +
               "\"source_verified\":" + (absent ? "false" : "true") +
               ",\"mount_verified\":" + (absent ? "false" : registering ? "null" : "true") +
               ",\"registration_verified\":" + (absent ? "false" : registering ? "null" : "true") +
               ",\"reason\":null}";
    }
    if (fake.reply == Fake::Reply::trailing_bytes)
        body.append("\0junk", 5);
    check(body.size() < capacity, "bounded fake response");
    std::memcpy(response, body.data(), body.size());
    response_bytes = body.size();
    return fake.http;
}

Binding binding(Fake& fake) {
    Binding result;
    result.context = &fake;
    result.send = send;
    result.nonce = new_nonce;
    result.journal = journal;
    result.service_identity_verified = true;
    result.credential_bound = true;
    result.capability_verified = true;
    return result;
}
} // namespace

int main() {
    using shelf::managed_tile_api_v1::Stage;
    Fake fake;
    Client default_client;
    check(default_client.status(0).state == State::credential_unavailable &&
          fake.calls == 0, "default client cannot contact a service");
    Client disabled(false, binding(fake));
    check(disabled.status(0).state == State::credential_unavailable &&
          fake.calls == 0, "explicitly disabled client ignores a bound transport");
    auto incomplete = binding(fake);
    incomplete.credential_bound = false;
    Client no_credential(true, incomplete);
    check(no_credential.status(0).state == State::credential_unavailable &&
          fake.calls == 0, "unbound credential stops read-only request");
    incomplete.credential_bound = true;
    incomplete.capability_verified = false;
    check(Client(true, incomplete).status(0).state == State::credential_unavailable &&
          fake.calls == 0, "unverified capability stops read-only request");
    incomplete.capability_verified = true;
    incomplete.service_identity_verified = false;
    check(Client(true, incomplete).status(0).state == State::credential_unavailable &&
          fake.calls == 0, "unverified service identity stops read-only request");

    Client client(true, binding(fake));
    std::size_t mutable_count = 0;
    for (std::size_t system = 0; system < system_count; ++system) {
        fake.system = system;
        fake.reply = Fake::Reply::absent;
        fake.http = 200;
        const auto status = client.status(system);
        check(status.state == State::observed &&
              status.status.stage == Stage::absent &&
              fake.last_route == managed_tile_api_v1::status_route,
              "all 52 catalog statuses use managed read route");
        ConsoleTileIntent intent{system, ConsoleTileDestination::library, {}};
        const int before = fake.calls;
        if (!managed_tile_api_v1::mutable_title(system)) {
            check(client.create(intent, nullptr).state == State::invalid_request &&
                  fake.calls == before, "four adopted pilots cannot CREATE");
            continue;
        }
        ++mutable_count;
        fake.reply = Fake::Reply::create;
        fake.http = 200;
        const auto attempt = client.create(intent, nullptr);
        check(attempt.state == State::receipt_needs_status &&
              attempt.journaled && attempt.may_have_sent &&
              std::strcmp(attempt.nonce, nonce_value) == 0 &&
              fake.calls == before + 1 &&
              fake.last_route == managed_tile_api_v1::create_route,
              "each new title CREATE is a journaled receipt only");
        fake.reply = Fake::Reply::installed;
        const auto reconciled = client.reconcile(attempt);
        check(reconciled.state == State::installed_reported &&
              reconciled.status.stage == Stage::installed &&
              fake.last_route == managed_tile_api_v1::status_route,
              "separate exact-generation status reports installed");
        fake.reply = Fake::Reply::registering;
        check(client.reconcile(attempt).state == State::pending,
              "in-progress generation stays pending");
        fake.reply = Fake::Reply::wrong_route;
        check(client.reconcile(attempt).state == State::reply_unknown,
              "another route cannot satisfy original CREATE");
        fake.reply = Fake::Reply::wrong_nonce;
        check(client.reconcile(attempt).state == State::reply_unknown,
              "another generation cannot satisfy original CREATE");
        fake.reply = Fake::Reply::absent;
        check(client.reconcile(attempt).state == State::reply_unknown,
              "post-CREATE absence cannot authorize retry");
    }
    check(mutable_count == 48, "exactly 48 new titles are mutable");
    fake.system = 0;
    ConsoleTileIntent intent{0, ConsoleTileDestination::library, {}};
    fake.journal_ok = false;
    int before = fake.calls;
    check(client.create(intent, nullptr).state == State::journal_unavailable &&
          fake.calls == before, "missing durable journal stops before POST");
    fake.journal_ok = true;
    fake.mutate_journal_intent = true;
    check(client.create(intent, nullptr).state == State::journal_unavailable &&
          fake.calls == before, "journal callback cannot change route before POST");
    fake.mutate_journal_intent = false;
    fake.mutate_journal_nonce = true;
    check(client.create(intent, nullptr).state == State::journal_unavailable &&
          fake.calls == before, "journal callback cannot change nonce before POST");
    fake.mutate_journal_nonce = false;
    fake.mutate_journal_unterminated = true;
    check(client.create(intent, nullptr).state == State::journal_unavailable &&
          fake.calls == before, "unterminated callback title is bounded and held");
    fake.mutate_journal_unterminated = false;
    fake.nonce_ok = false;
    check(client.create(intent, nullptr).state == State::nonce_unavailable &&
          fake.calls == before, "missing CSPRNG stops before journal and POST");
    fake.nonce_ok = true;
    fake.http = -1;
    const auto uncertain = client.create(intent, nullptr);
    check(uncertain.state == State::transport_unknown && uncertain.journaled &&
          uncertain.may_have_sent && std::strcmp(uncertain.nonce, nonce_value) == 0,
          "lost reply retains original nonce for status only");
    fake.http = 200;
    fake.reply = Fake::Reply::broken;
    check(client.reconcile(uncertain).state == State::reply_unknown,
          "malformed status cannot become installed");
    fake.reply = Fake::Reply::trailing_bytes;
    check(client.reconcile(uncertain).state == State::reply_unknown,
          "valid JSON followed by NUL and bytes cannot be accepted");
    check(client.status(system_count).state == State::invalid_request,
          "out-of-catalog status cannot send");
    fake.reply = Fake::Reply::create;
    fake.mutate_input = &intent;
    const auto copied = client.create(intent, nullptr);
    fake.mutate_input = nullptr;
    check(copied.state == State::receipt_needs_status &&
          copied.intent.destination == ConsoleTileDestination::library &&
          intent.destination == ConsoleTileDestination::home,
          "send callback cannot change the recorded CREATE route");
    auto mutable_attempt = copied;
    fake.reply = Fake::Reply::installed;
    fake.mutate_attempt = &mutable_attempt;
    check(client.reconcile(mutable_attempt).state == State::installed_reported,
          "status callback cannot change the pinned generation or route");
    fake.mutate_attempt = nullptr;
    auto unterminated_attempt = copied;
    std::memset(unterminated_attempt.nonce, 'a',
                sizeof(unterminated_attempt.nonce));
    const int calls_before_bad_attempt = fake.calls;
    check(client.reconcile(unterminated_attempt).state ==
              State::invalid_request && fake.calls == calls_before_bad_attempt,
          "unterminated attempt nonce is rejected before transport");
    std::puts("PASS: managed app client 52 status / 48 receipt-only CREATE boundaries");
}
