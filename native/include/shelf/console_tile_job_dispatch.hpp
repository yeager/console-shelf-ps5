// SPDX-License-Identifier: GPL-3.0-only
// One-shot app-side transport for the shared console tile job contract.
// The caller supplies storage and a pinned helper sender. This code does not
// enable the PS5 UI write gate or infer Home visibility from a source receipt.
#pragma once

#include "console_tile_job.hpp"
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace shelf::tile_job {

inline constexpr std::size_t maximum_job_path = 128;

enum class JobFile { request, result, sent };
enum class FileStatus { missing, present, error };
enum class ReceiptStatus { pending, accepted, rejected };

struct JobIo {
    void* context = nullptr;
    bool (*entropy)(void*, unsigned char (&)[16]) = nullptr;
    FileStatus (*read)(void*, const char*, char*, std::size_t, std::size_t&) = nullptr;
    bool (*create)(void*, const char*, std::string_view) = nullptr;
};

inline bool job_path(JobFile file, const char (&nonce)[33], char (&out)[maximum_job_path]) {
    if (!nonce_valid(nonce)) return false;
    const char* prefix = file == JobFile::request ? "console-tile-job-" :
                         file == JobFile::result ? "console-tile-result-" :
                         file == JobFile::sent ? "console-tile-sent-" : nullptr;
    if (!prefix) return false;
    const int used = std::snprintf(out, sizeof(out), "%s/%s%s.txt", job_root, prefix, nonce);
    return used > 0 && static_cast<std::size_t>(used) < sizeof(out);
}

inline bool sent_record(const char (&nonce)[33], char (&out)[96]) {
    if (!nonce_valid(nonce)) return false;
    const int used = std::snprintf(out, sizeof(out),
        "format=isshelf-console-tile-send/1\nnonce=%s\n", nonce);
    return used > 0 && static_cast<std::size_t>(used) < sizeof(out);
}

inline bool make_nonce(const JobIo& io, char (&nonce)[33]) {
    if (!io.entropy) return false;
    unsigned char random[16]{};
    if (!io.entropy(io.context, random)) return false;
    constexpr char alphabet[] = "0123456789abcdef";
    for (std::size_t i = 0; i < 16; ++i) {
        nonce[i * 2] = alphabet[random[i] >> 4];
        nonce[i * 2 + 1] = alphabet[random[i] & 15];
    }
    nonce[32] = 0;
    return nonce_valid(nonce);
}

inline bool exact_file(const JobIo& io, const char* path, std::string_view expected) {
    if (!io.read || expected.size() > maximum_receipt_bytes) return false;
    char bytes[maximum_receipt_bytes + 1]{};
    std::size_t used = 0;
    return io.read(io.context, path, bytes, sizeof(bytes), used) == FileStatus::present &&
           used == expected.size() && std::memcmp(bytes, expected.data(), used) == 0;
}

class Dispatch {
public:
    const Request& request() const { return request_; }
    Phase phase() const { return phase_; }
    bool transfer_complete() const { return transfer_complete_; }

    // Create-only request with exact readback. A pre-existing receipt or send
    // marker is a collision, never a reason to reuse this nonce or retry.
    bool begin(const JobIo& io, Action action, ConsoleTileIntent intent,
               EmulatorRouteCheck route_check = nullptr, const void* route_context = nullptr) {
        if (phase_ != Phase::none || !io.read || !io.create ||
            !make_nonce(io, request_.nonce)) return false;
        request_.action = action;
        request_.intent = intent;
        if (action != Action::upsert &&
            (intent.destination != ConsoleTileDestination::library || intent.emulator_title[0]))
            return false;
        char bytes[maximum_request_bytes + 1]{}, request_path[maximum_job_path]{};
        char result_path[maximum_job_path]{}, sent_path[maximum_job_path]{};
        if (!encode_request(request_, bytes, sizeof(bytes), route_check, route_context) ||
            !job_path(JobFile::request, request_.nonce, request_path) ||
            !job_path(JobFile::result, request_.nonce, result_path) ||
            !job_path(JobFile::sent, request_.nonce, sent_path)) return false;
        char scratch[maximum_receipt_bytes + 1]{};
        std::size_t used = 0;
        if (io.read(io.context, result_path, scratch, sizeof(scratch), used) != FileStatus::missing ||
            io.read(io.context, sent_path, scratch, sizeof(scratch), used) != FileStatus::missing)
            return false;
        // The create callback may have consumed O_EXCL before reporting a
        // write/fsync error. Treat *any* create attempt as consumed.
        phase_ = Phase::held;
        if (!io.create(io.context, request_path, bytes)) return false;
        if (!exact_file(io, request_path, bytes)) { phase_ = Phase::held; return false; }
        phase_ = Phase::none;
        return advance(action, phase_, Observation::request_created);
    }

    // The durable marker is created and read back before the sender is called.
    // It records an attempted send even if the socket reply is lost. A caller
    // may invoke this method once; a relaunched app may only inspect the job.
    bool attempt_once(const JobIo& io, bool (*send)(void*), void* send_context) {
        if (phase_ != Phase::queued || restored_ || !io.create || !send) return false;
        char path[maximum_job_path]{}, bytes[96]{};
        if (!job_path(JobFile::sent, request_.nonce, path) || !sent_record(request_.nonce, bytes))
            return false;
        phase_ = Phase::held;
        if (!io.create(io.context, path, bytes)) return false;
        if (!exact_file(io, path, bytes)) { phase_ = Phase::held; return false; }
        phase_ = Phase::queued;
        if (!advance(request_.action, phase_, Observation::send_attempted)) return false;
        transfer_complete_ = send(send_context);
        return transfer_complete_;
    }

    // Recovery never sends. The immutable request and send marker survive an
    // app relaunch; a queued request is left queued for explicit disposition.
    bool restore(const JobIo& io, const char (&nonce)[33],
                 EmulatorRouteCheck route_check = nullptr, const void* route_context = nullptr) {
        if (phase_ != Phase::none || !io.read || !nonce_valid(nonce)) return false;
        char path[maximum_job_path]{}, bytes[maximum_request_bytes + 1]{};
        std::size_t used = 0;
        if (!job_path(JobFile::request, nonce, path) ||
            io.read(io.context, path, bytes, sizeof(bytes), used) != FileStatus::present ||
            used > maximum_request_bytes ||
            !parse_request({bytes, used}, request_, route_check, route_context) ||
            std::strcmp(request_.nonce, nonce) != 0) return false;
        char canonical[maximum_request_bytes + 1]{};
        if (!encode_request(request_, canonical, sizeof(canonical), route_check, route_context) ||
            std::strlen(canonical) != used || std::memcmp(canonical, bytes, used) != 0)
            return false;
        if (!advance(request_.action, phase_, Observation::request_created)) return false;
        restored_ = true;
        char marker[96]{}, readback[maximum_receipt_bytes + 1]{};
        if (!job_path(JobFile::sent, nonce, path) || !sent_record(nonce, marker)) return false;
        const auto status = io.read(io.context, path, readback, sizeof(readback), used);
        if (status == FileStatus::error) { phase_ = Phase::held; return false; }
        if (status == FileStatus::missing) {
            char result_path[maximum_job_path]{};
            if (!job_path(JobFile::result, nonce, result_path) ||
                io.read(io.context, result_path, readback, sizeof(readback), used) != FileStatus::missing) {
                phase_ = Phase::held;
                return false;
            }
            return true;
        }
        if (used != std::strlen(marker) || std::memcmp(marker, readback, used) != 0) {
            phase_ = Phase::held;
            return false;
        }
        return advance(request_.action, phase_, Observation::send_attempted);
    }

    ReceiptStatus read_receipt(const JobIo& io, Receipt& receipt,
                               EmulatorRouteCheck route_check = nullptr,
                               const void* route_context = nullptr) {
        if (phase_ != Phase::sent_unverified || !io.read) return ReceiptStatus::rejected;
        char path[maximum_job_path]{}, bytes[maximum_receipt_bytes + 1]{};
        std::size_t used = 0;
        if (!job_path(JobFile::result, request_.nonce, path)) return ReceiptStatus::rejected;
        const auto status = io.read(io.context, path, bytes, sizeof(bytes), used);
        if (status == FileStatus::missing) return ReceiptStatus::pending;
        if (status != FileStatus::present || used > maximum_receipt_bytes ||
            !parse_receipt({bytes, used}, receipt) ||
            !observe_receipt(request_, receipt, phase_, route_check, route_context)) {
            phase_ = Phase::held;
            return ReceiptStatus::rejected;
        }
        return ReceiptStatus::accepted;
    }

private:
    Request request_{};
    Phase phase_ = Phase::none;
    bool transfer_complete_ = false;
    bool restored_ = false;
};

} // namespace shelf::tile_job
