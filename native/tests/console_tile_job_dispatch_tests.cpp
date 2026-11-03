// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_job_dispatch.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace {
using namespace shelf;
using namespace shelf::tile_job;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL console tile dispatch: %s\n", message);
        std::exit(1);
    }
}
struct Store {
    std::map<std::string, std::string> files;
    unsigned next_nonce = 1;
    unsigned sends = 0;
    bool send_ok = true;
    bool fail_reads = false;
    bool corrupt_readback = false;
    bool fail_next_create = false;
    static bool entropy(void* raw, unsigned char (&output)[16]) {
        auto& self = *static_cast<Store*>(raw);
        for (unsigned i = 0; i < 16; ++i)
            output[i] = static_cast<unsigned char>((self.next_nonce >> ((i % 4) * 8)) + i);
        ++self.next_nonce;
        return true;
    }
    static FileStatus read(void* raw, const char* path, char* output,
                           std::size_t capacity, std::size_t& used) {
        auto& self = *static_cast<Store*>(raw);
        used = 0;
        if (self.fail_reads) return FileStatus::error;
        const auto found = self.files.find(path);
        if (found == self.files.end()) return FileStatus::missing;
        const auto& body = found->second;
        if (body.size() >= capacity) return FileStatus::error;
        used = body.size();
        std::memcpy(output, body.data(), used);
        if (self.corrupt_readback && used) output[0] = 'x';
        return FileStatus::present;
    }
    static bool create(void* raw, const char* path, std::string_view body) {
        auto& self = *static_cast<Store*>(raw);
        const bool created = self.files.emplace(path, std::string(body)).second;
        if (self.fail_next_create) { self.fail_next_create = false; return false; }
        return created;
    }
    static bool send(void* raw) {
        auto& self = *static_cast<Store*>(raw);
        ++self.sends;
        return self.send_ok;
    }
    JobIo io() { return {this, entropy, read, create}; }
};
bool route(const void*, std::size_t system, std::string_view title) {
    return system < system_count && catalog[system].id == "ps1" && title == "PPSA99248";
}
ConsoleTileIntent intent(std::size_t system, ConsoleTileDestination destination = ConsoleTileDestination::library,
                         const char* emulator = "") {
    ConsoleTileIntent result;
    result.system = system;
    result.destination = destination;
    std::snprintf(result.emulator_title, sizeof(result.emulator_title), "%s", emulator);
    return result;
}
void receipt(Store& store, const Dispatch& dispatch, Result result,
             const char* title = nullptr) {
    Receipt value;
    std::memcpy(value.nonce, dispatch.request().nonce, sizeof(value.nonce));
    value.action = dispatch.request().action;
    value.intent = dispatch.request().intent;
    value.result = result;
    if (result == Result::absent || result == Result::held || result == Result::retired_source) {
        value.intent.destination = ConsoleTileDestination::library;
        value.intent.emulator_title[0] = 0;
    }
    if (result != Result::absent && result != Result::held)
        std::snprintf(value.title_id, sizeof(value.title_id), "%s",
                      title ? title : owned_title_for(dispatch.request().intent.system));
    char wire[maximum_receipt_bytes + 1]{};
    check(encode_receipt(value, wire, sizeof(wire)), "fixture receipt encodes");
    char path[maximum_job_path]{};
    check(job_path(JobFile::result, dispatch.request().nonce, path), "receipt path");
    store.files[path] = wire;
}
}

int main() {
    static_assert(system_count == 52, "all catalog consoles are covered");
    Store store;
    for (std::size_t system = 0; system < system_count; ++system) {
        for (auto action : {Action::inspect, Action::upsert, Action::remove}) {
            Dispatch job;
            check(job.begin(store.io(), action, intent(system)), "all 52 actions create a request");
            check(job.phase() == Phase::queued, "request remains queued before send");
            check(job.attempt_once(store.io(), Store::send, &store), "one send succeeds");
            check(!job.attempt_once(store.io(), Store::send, &store), "repeated send blocked");
            Receipt parsed;
            check(job.read_receipt(store.io(), parsed) == ReceiptStatus::pending,
                  "missing result remains pending");
            receipt(store, job, action == Action::inspect ? Result::absent :
                                action == Action::remove ? Result::retired_source : Result::upserted_source);
            check(job.read_receipt(store.io(), parsed) == ReceiptStatus::accepted,
                  "bound result accepted");
            const Phase expected = action == Action::inspect ? Phase::inspected_absent :
                                   action == Action::remove ? Phase::removal_pending : Phase::source_observed;
            check(job.phase() == expected, "receipt stops short of TV confirmation");
            check(job.read_receipt(store.io(), parsed) == ReceiptStatus::rejected,
                  "receipt is consumed once");
        }
    }
    check(store.sends == system_count * 3, "exactly one sender invocation per job");

    for (std::size_t system = 0; system < system_count; ++system) {
        Dispatch home;
        check(home.begin(store.io(), Action::upsert, intent(system, ConsoleTileDestination::home)) &&
              home.attempt_once(store.io(), Store::send, &store),
              "all 52 Home destinations create and send once");
        receipt(store, home, Result::upserted_source);
        Receipt parsed_home;
        check(home.read_receipt(store.io(), parsed_home) == ReceiptStatus::accepted &&
              parsed_home.intent.destination == ConsoleTileDestination::home,
              "all 52 Home routes retain their exact destination");
    }

    Dispatch emulator;
    std::size_t ps1 = system_count, ps3 = system_count;
    for (std::size_t i = 0; i < system_count; ++i) {
        if (catalog[i].id == "ps1") ps1 = i;
        if (catalog[i].id == "ps3") ps3 = i;
    }
    check(ps1 < system_count && ps3 < system_count, "PS1 and PS3 catalog entries exist");
    check(!emulator.begin(store.io(), Action::upsert,
                          intent(ps3, ConsoleTileDestination::emulator, "PPSA99248"), route),
          "wrong console emulator route refused before request creation");
    check(emulator.begin(store.io(), Action::upsert,
                         intent(ps1, ConsoleTileDestination::emulator, "PPSA99248"), route),
          "matching emulator route accepted");
    const auto saved_nonce = std::string(emulator.request().nonce);
    Dispatch reopened;
    char nonce[33]{};
    std::memcpy(nonce, saved_nonce.c_str(), sizeof(nonce));
    check(reopened.restore(store.io(), nonce, route) && reopened.phase() == Phase::queued,
          "unsent job restored as queued without sending");
    check(!reopened.attempt_once(store.io(), Store::send, &store),
          "a recovered queued request is never auto-dispatched");
    check(store.sends == system_count * 4, "recovery never calls sender");
    check(emulator.attempt_once(store.io(), Store::send, &store), "emulator request sent once");
    Dispatch after_send;
    check(after_send.restore(store.io(), nonce, route) &&
          after_send.phase() == Phase::sent_unverified,
          "attempt marker persists across app relaunch");
    check(store.sends == system_count * 4 + 1, "sent recovery cannot dispatch again");
    Receipt parsed;
    receipt(store, emulator, Result::upserted_source);
    check(after_send.read_receipt(store.io(), parsed, route) == ReceiptStatus::accepted,
          "reopened app verifies emulator receipt");

    Store lost_reply;
    lost_reply.send_ok = false;
    Dispatch uncertain;
    check(uncertain.begin(lost_reply.io(), Action::upsert, intent(0)), "uncertain job created");
    check(!uncertain.attempt_once(lost_reply.io(), Store::send, &lost_reply) &&
          uncertain.phase() == Phase::sent_unverified,
          "failed transport remains sent and unverified");
    check(!uncertain.attempt_once(lost_reply.io(), Store::send, &lost_reply) &&
          lost_reply.sends == 1, "transport failure never retries");
    Dispatch uncertain_reopened;
    check(uncertain_reopened.restore(lost_reply.io(), uncertain.request().nonce) &&
          uncertain_reopened.phase() == Phase::sent_unverified,
          "uncertain attempt persists across relaunch");

    Store tampered;
    Dispatch bad;
    check(bad.begin(tampered.io(), Action::inspect, intent(0)) &&
          bad.attempt_once(tampered.io(), Store::send, &tampered), "tamper fixture sent");
    receipt(tampered, bad, Result::absent);
    char path[maximum_job_path]{};
    check(job_path(JobFile::result, bad.request().nonce, path), "tamper result path");
    tampered.files[path].replace(tampered.files[path].find("action=inspect"), 14, "action=remove ");
    check(bad.read_receipt(tampered.io(), parsed) == ReceiptStatus::rejected &&
          bad.phase() == Phase::held, "mismatched result holds job");

    Store wrong_title_store;
    Dispatch wrong_title_job;
    check(wrong_title_job.begin(wrong_title_store.io(), Action::upsert, intent(ps1)) &&
          wrong_title_job.attempt_once(wrong_title_store.io(), Store::send, &wrong_title_store),
          "wrong title fixture sent");
    receipt(wrong_title_store, wrong_title_job, Result::upserted_source);
    check(job_path(JobFile::result, wrong_title_job.request().nonce, path),
          "wrong title result path");
    auto& title_wire = wrong_title_store.files[path];
    title_wire.replace(title_wire.find("title=PPSA99704"), 15, "title=PPSA99702");
    check(wrong_title_job.read_receipt(wrong_title_store.io(), parsed) == ReceiptStatus::rejected &&
          wrong_title_job.phase() == Phase::held,
          "another valid PPSA title cannot prove this console's source");

    Store collision;
    char duplicate_nonce[33]{};
    check(make_nonce(collision.io(), duplicate_nonce), "nonce fixture");
    char duplicate_path[maximum_job_path]{};
    check(job_path(JobFile::result, duplicate_nonce, duplicate_path), "collision path");
    collision.files[duplicate_path] = "old";
    collision.next_nonce = 1;
    Dispatch collided;
    check(!collided.begin(collision.io(), Action::inspect, intent(0)) &&
          collision.files.size() == 1, "stale result rejects nonce collision without overwrite");

    Store corrupt;
    corrupt.corrupt_readback = true;
    Dispatch damaged;
    check(!damaged.begin(corrupt.io(), Action::inspect, intent(0)) &&
          damaged.phase() == Phase::held, "request readback mismatch stops dispatch");
    check(corrupt.files.size() == 1, "failed readback leaves consumed request as evidence");

    Store failed_request_create;
    failed_request_create.fail_next_create = true;
    Dispatch partial_request;
    check(!partial_request.begin(failed_request_create.io(), Action::inspect, intent(ps1)) &&
          partial_request.phase() == Phase::held && failed_request_create.files.size() == 1,
          "failed create after O_EXCL leaves held request and evidence");
    check(!partial_request.begin(failed_request_create.io(), Action::inspect, intent(ps1)) &&
          failed_request_create.files.size() == 1,
          "same dispatch object cannot generate a new nonce after consumed create");

    Store failed_marker_create;
    Dispatch partial_marker;
    check(partial_marker.begin(failed_marker_create.io(), Action::upsert, intent(ps1)),
          "marker failure fixture created request");
    failed_marker_create.fail_next_create = true;
    check(!partial_marker.attempt_once(failed_marker_create.io(), Store::send,
                                       &failed_marker_create) &&
          partial_marker.phase() == Phase::held && failed_marker_create.sends == 0,
          "failed marker create holds job without calling loader");
    check(!partial_marker.attempt_once(failed_marker_create.io(), Store::send,
                                       &failed_marker_create) && failed_marker_create.sends == 0,
          "partial marker cannot be retried");

    Store premature;
    Dispatch before_send;
    check(before_send.begin(premature.io(), Action::inspect, intent(0)),
          "premature receipt fixture queued");
    receipt(premature, before_send, Result::absent);
    Dispatch premature_reopened;
    check(!premature_reopened.restore(premature.io(), before_send.request().nonce) &&
          premature_reopened.phase() == Phase::held,
          "result without a send marker cannot prove a resumed job");

    std::puts("PASS: 52-console one-shot dispatch, route checks, durable recovery and exact receipts");
}
