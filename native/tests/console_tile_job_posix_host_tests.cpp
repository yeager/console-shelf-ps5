// SPDX-License-Identifier: GPL-3.0-only
// Run on a Linux host with the checked-at host adapter; never touches /data.
#include "shelf/console_tile_job_posix.hpp"
#include "shelf/console_tile_job_routes.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <string>
#include <sys/wait.h>

namespace {
void check(bool okay, const char* message) {
    if (!okay) {
        std::fprintf(stderr, "FAIL POSIX tile jobs: %s\n", message);
        std::exit(1);
    }
}
std::string fixture() {
    char path[] = "/tmp/isshelf-job-dir-XXXXXX";
    check(mkdtemp(path) != nullptr, "private test root created");
    const std::string parent = std::string(path) + "/console-shelf";
    check(mkdir(parent.c_str(), 0777) == 0 && chmod(parent.c_str(), 0777) == 0,
          "world-writable parent fixture created");
    return path;
}
bool sent(void* raw) {
    ++*static_cast<unsigned*>(raw);
    return true;
}
bool fixture_installed_route(const void* raw, std::size_t system, std::string_view title) {
    return *static_cast<const bool*>(raw) &&
           shelf::tile_job::pinned_historical_emulator_route(nullptr, system, title);
}
}

int main() {
    using namespace shelf::tile_job;
    const auto owner = static_cast<std::uint64_t>(geteuid());
    const auto root = fixture();
    const auto parent = root + "/console-shelf";
    const auto child = parent + "/tile-jobs-v1";
    PosixJobStore store(root.c_str(), owner);
    check(store.open_root(true) && store.identity_fresh(),
          "clean install creates an anchored private child");
    struct stat info{};
    check(lstat(child.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
          static_cast<std::uint64_t>(info.st_uid) == owner &&
          (info.st_mode & 0777u) == 0700u,
          "new child has exact owner and mode");
    Dispatch job;
    shelf::ConsoleTileIntent intent;
    intent.system = 0;
    check(job.begin(store.io(), Action::upsert, intent),
          "request created below held child descriptor");
    char request_path[maximum_job_path]{};
    check(job_path(JobFile::request, job.request().nonce, request_path), "request path formed");
    const char* leaf = owned_job_leaf(request_path);
    check(leaf != nullptr, "only canonical request leaf is accepted");
    const auto actual = child + "/" + leaf;
    check(lstat(actual.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
          (info.st_mode & 0777u) == 0600u,
          "request is private and create-only");
    check(owned_job_leaf("/data/console-shelf/console-tile-job-"
                         "0123456789abcdef0123456789abcdef.txt") == nullptr,
          "old world-writable parent path is rejected");
    check(owned_job_leaf("/data/console-shelf/tile-jobs-v1/../escape") == nullptr,
          "path traversal is rejected");

    {
    const auto lock_root = fixture();
    int ready[2]{};
    int lock_gate[2]{};
    check(pipe(ready) == 0 && pipe(lock_gate) == 0,
          "sender lock synchronization pipes created");
    const pid_t lock_owner = fork();
    check(lock_owner >= 0, "sender lock owner forked");
    if (lock_owner == 0) {
        close(ready[0]);
        close(lock_gate[1]);
        PosixJobStore child_store(lock_root.c_str(), owner);
        const unsigned char okay = child_store.open_root(true) &&
            child_store.acquire_send_lock() &&
            can_begin_new_upsert(child_store, 0, nullptr, nullptr) ? 1 : 0;
        if (write(ready[1], &okay, 1) != 1) _exit(10);
        // Keep the descriptor open until killed; EOF releases the child if an
        // earlier parent assertion fails and exits unexpectedly.
        unsigned char release = 0;
        const auto release_count = read(lock_gate[0], &release, 1);
        _exit(release_count == 0 ? 11 : 14);
    }
    close(ready[1]);
    close(lock_gate[0]);
    unsigned char child_ready = 0;
    check(read(ready[0], &child_ready, 1) == 1 && child_ready == 1 &&
          close(ready[0]) == 0, "child holds clean sender lock");
    PosixJobStore competing(lock_root.c_str(), owner);
    check(competing.open_root(false) && !competing.acquire_send_lock(),
          "second process cannot enter preflight while child holds lock");
    check(kill(lock_owner, SIGKILL) == 0, "locked child killed without cleanup");
    int child_status = 0;
    check(waitpid(lock_owner, &child_status, 0) == lock_owner &&
          WIFSIGNALED(child_status) && close(lock_gate[1]) == 0,
          "locked child exited by signal");
    check(competing.acquire_send_lock() &&
          can_begin_new_upsert(competing, 0, nullptr, nullptr),
          "kernel releases sender lock after crash and clean history is reusable");
    const auto lock_file = lock_root + "/console-shelf/" + send_lock_leaf;
    check(lstat(lock_file.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
          info.st_size == 0 && (info.st_mode & 07777u) == 0600u,
          "persistent lock file stays outside strict helper job inventory");
    PosixJobStore same_process(lock_root.c_str(), owner);
    check(same_process.open_root(false) && !same_process.acquire_send_lock(),
          "process-local guard blocks a second sender in one process");
    const auto displaced_lock = lock_root + "/console-shelf/displaced-send.lock";
    check(rename(lock_file.c_str(), displaced_lock.c_str()) == 0,
          "writable-parent replacement fixture moved locked file");
    check(!competing.identity_fresh() &&
          !can_begin_new_upsert(competing, 0, nullptr, nullptr),
          "named lock replacement prevents further sender work");
    }

    const auto pending_root = fixture();
    int pending_ready[2]{};
    int pending_gate[2]{};
    check(pipe(pending_ready) == 0 && pipe(pending_gate) == 0,
          "pending sender synchronization pipes created");
    const pid_t pending_owner = fork();
    check(pending_owner >= 0, "pending sender forked");
    if (pending_owner == 0) {
        close(pending_ready[0]);
        close(pending_gate[1]);
        PosixJobStore child_store(pending_root.c_str(), owner);
        Dispatch queued;
        shelf::ConsoleTileIntent queued_intent;
        queued_intent.system = 0;
        const unsigned char okay = child_store.open_root(true) &&
            child_store.acquire_send_lock() &&
            can_begin_new_upsert(child_store, 0, nullptr, nullptr) &&
            queued.begin(child_store.io(), Action::upsert, queued_intent) ? 1 : 0;
        if (write(pending_ready[1], &okay, 1) != 1) _exit(12);
        unsigned char release = 0;
        const auto release_count = read(pending_gate[0], &release, 1);
        _exit(release_count == 0 ? 13 : 15);
    }
    close(pending_ready[1]);
    close(pending_gate[0]);
    unsigned char child_ready = 0;
    check(read(pending_ready[0], &child_ready, 1) == 1 && child_ready == 1 &&
          close(pending_ready[0]) == 0, "first sender persisted one queued request");
    PosixJobStore after_crash(pending_root.c_str(), owner);
    check(after_crash.open_root(false) && !after_crash.acquire_send_lock(),
          "racing sender cannot pass the held lock");
    int child_status = 0;
    check(kill(pending_owner, SIGKILL) == 0 &&
          waitpid(pending_owner, &child_status, 0) == pending_owner &&
          WIFSIGNALED(child_status) && close(pending_gate[1]) == 0,
          "queued sender crashed");
    check(after_crash.acquire_send_lock() &&
          !can_begin_new_upsert(after_crash, 1, nullptr, nullptr),
          "post-crash preflight sees the queued request and forbids another nonce");

    const auto preflight_root = fixture();
    PosixJobStore preflight_store(preflight_root.c_str(), owner);
    check(preflight_store.open_root(true), "preflight fixture child created");
    check(can_begin_new_upsert(preflight_store, 0, nullptr, nullptr),
          "empty private directory admits the first create");
    Dispatch first;
    check(first.begin(preflight_store.io(), Action::upsert, intent),
          "first create request queued");
    check(!can_begin_new_upsert(preflight_store, 1, nullptr, nullptr),
          "queued request blocks a second create");
    unsigned first_sends = 0;
    check(first.attempt_once(preflight_store.io(), sent, &first_sends) && first_sends == 1,
          "first create send marker recorded");
    check(!can_begin_new_upsert(preflight_store, 1, nullptr, nullptr),
          "sent request without a receipt blocks a second create");
    Receipt accepted;
    std::memcpy(accepted.nonce, first.request().nonce, sizeof(accepted.nonce));
    accepted.action = Action::upsert;
    accepted.intent = intent;
    accepted.result = Result::upserted_source;
    std::strcpy(accepted.title_id, owned_title_for(0));
    char receipt_body[maximum_receipt_bytes + 1]{}, receipt_path[maximum_job_path]{};
    check(encode_receipt(accepted, receipt_body, sizeof(receipt_body)) &&
          job_path(JobFile::result, accepted.nonce, receipt_path) &&
          preflight_store.io().create(preflight_store.io().context, receipt_path,
                                      receipt_body),
          "bound source receipt created");
    check(can_begin_new_upsert(preflight_store, 1, nullptr, nullptr),
          "completed source receipt admits a different console");
    check(!can_begin_new_upsert(preflight_store, 0, nullptr, nullptr),
          "same console cannot consume a second create without route update");
    const auto unexpected = preflight_root + "/console-shelf/tile-jobs-v1/unexpected.txt";
    const int unexpected_fd = open(unexpected.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    check(unexpected_fd >= 0 && close(unexpected_fd) == 0,
          "unexpected entry fixture created");
    check(!can_begin_new_upsert(preflight_store, 1, nullptr, nullptr),
          "unexpected directory entry fails closed");

    const auto orphan_lease_root = fixture();
    PosixJobStore orphan_lease_store(orphan_lease_root.c_str(), owner);
    check(orphan_lease_store.open_root(true), "orphan lease fixture child created");
    const auto orphan_lease = orphan_lease_root + "/console-shelf/tile-jobs-v1/console-tile-lease-" +
                              std::string(owned_title_for(0)) + ".txt";
    const int orphan_lease_fd = open(orphan_lease.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    check(orphan_lease_fd >= 0 && close(orphan_lease_fd) == 0,
          "orphan lease fixture created");
    check(!can_begin_new_upsert(orphan_lease_store, 1, nullptr, nullptr),
          "lease without a completed request blocks a new create");

    const auto orphan_sent_root = fixture();
    PosixJobStore orphan_sent_store(orphan_sent_root.c_str(), owner);
    check(orphan_sent_store.open_root(true), "orphan marker fixture child created");
    char orphan_nonce[33] = "0123456789abcdef0123456789abcdef";
    char orphan_path[maximum_job_path]{}, orphan_body[96]{};
    const auto orphan_io = orphan_sent_store.io();
    check(job_path(JobFile::sent, orphan_nonce, orphan_path) &&
          sent_record(orphan_nonce, orphan_body) &&
          orphan_io.create(orphan_io.context, orphan_path, orphan_body),
          "orphan send marker fixture created");
    check(!can_begin_new_upsert(orphan_sent_store, 1, nullptr, nullptr),
          "send marker without a request blocks a new create");

    const auto historical_root = fixture();
    PosixJobStore historical_store(historical_root.c_str(), owner);
    check(historical_store.open_root(true), "historical menu fixture child created");
    shelf::ConsoleTileIntent menu_intent;
    menu_intent.system = 2;
    menu_intent.destination = shelf::ConsoleTileDestination::emulator;
    std::strcpy(menu_intent.emulator_title, "PPSA99303");
    bool menu_installed = true;
    Dispatch menu_job;
    check(menu_job.begin(historical_store.io(), Action::upsert, menu_intent,
                         fixture_installed_route, &menu_installed),
          "installed pinned menu request created");
    unsigned menu_sends = 0;
    check(menu_job.attempt_once(historical_store.io(), sent, &menu_sends) &&
          menu_sends == 1, "historical menu send marker recorded");
    Receipt menu_receipt;
    std::memcpy(menu_receipt.nonce, menu_job.request().nonce, sizeof(menu_receipt.nonce));
    menu_receipt.action = Action::upsert;
    menu_receipt.intent = menu_intent;
    menu_receipt.result = Result::upserted_source;
    std::strcpy(menu_receipt.title_id, owned_title_for(menu_intent.system));
    char menu_body[maximum_receipt_bytes + 1]{};
    char menu_path[maximum_job_path]{};
    const auto menu_io = historical_store.io();
    check(encode_receipt(menu_receipt, menu_body, sizeof(menu_body)) &&
          job_path(JobFile::result, menu_receipt.nonce, menu_path) &&
          menu_io.create(menu_io.context, menu_path, menu_body),
          "historical menu source receipt created");
    menu_installed = false;
    Dispatch recovered_menu;
    Receipt recovered_receipt;
    check(recovered_menu.restore(historical_store.io(), menu_job.request().nonce,
                                 pinned_historical_emulator_route, nullptr) &&
          recovered_menu.read_receipt(historical_store.io(), recovered_receipt,
                                      pinned_historical_emulator_route, nullptr) ==
              ReceiptStatus::accepted &&
          recovered_menu.phase() == Phase::source_observed,
          "startup can read completed pinned menu history after uninstall");
    check(!can_begin_new_upsert(historical_store, 1, fixture_installed_route,
                                &menu_installed),
          "current installation cannot revalidate a removed historical menu");
    check(can_begin_new_upsert(historical_store, 1,
                               pinned_historical_emulator_route, nullptr),
          "pinned completed menu history admits another console after uninstall");
    check(!can_begin_new_upsert(historical_store, menu_intent.system,
                                pinned_historical_emulator_route, nullptr),
          "historical menu title still blocks duplicate title creation");

    char missing_parent_root[] = "/tmp/isshelf-job-missing-parent-XXXXXX";
    check(mkdtemp(missing_parent_root) != nullptr, "missing parent fixture created");
    PosixJobStore missing_parent_store(missing_parent_root, owner);
    check(!missing_parent_store.open_root(true), "app refuses absent root-owned parent");
    const auto missing_parent = std::string(missing_parent_root) + "/console-shelf";
    check(lstat(missing_parent.c_str(), &info) != 0 && errno == ENOENT,
          "app did not create an app-owned substitute parent");

    const auto displaced = parent + "/displaced";
    check(rename(child.c_str(), displaced.c_str()) == 0 && mkdir(child.c_str(), 0700) == 0,
          "attacker rename/replacement fixture created");
    check(!store.identity_fresh(), "fresh name-to-held inode check detects replacement");
    unsigned sends = 0;
    check(!job.attempt_once(store.io(), sent, &sends) && sends == 0 &&
          job.phase() == Phase::held,
          "renamed private child prevents loader send after consumed marker attempt");

    const auto wrong_mode_root = fixture();
    const auto wrong_mode = wrong_mode_root + "/console-shelf/tile-jobs-v1";
    check(mkdir(wrong_mode.c_str(), 0700) == 0 && chmod(wrong_mode.c_str(), 0755) == 0,
          "preexisting wrong-mode child fixture created");
    PosixJobStore mode_store(wrong_mode_root.c_str(), owner);
    check(!mode_store.open_root(false), "preexisting wrong-mode child is refused");

    const auto symlink_root = fixture();
    const auto symlink_child = symlink_root + "/console-shelf/tile-jobs-v1";
    check(symlink("/tmp", symlink_child.c_str()) == 0,
          "preexisting symlink child fixture created");
    PosixJobStore symlink_store(symlink_root.c_str(), owner);
    check(!symlink_store.open_root(false), "preexisting symlink child is refused");

    char parent_symlink_root[] = "/tmp/isshelf-job-parent-XXXXXX";
    check(mkdtemp(parent_symlink_root) != nullptr, "parent symlink test root created");
    const auto parent_symlink = std::string(parent_symlink_root) + "/console-shelf";
    check(symlink("/tmp", parent_symlink.c_str()) == 0,
          "preexisting symlink parent fixture created");
    PosixJobStore parent_symlink_store(parent_symlink_root, owner);
    check(!parent_symlink_store.open_root(false), "preexisting symlink parent is refused");

    if (owner == 0) {
        const auto wrong_owner_root = fixture();
        const auto wrong_owner = wrong_owner_root + "/console-shelf/tile-jobs-v1";
        check(mkdir(wrong_owner.c_str(), 0700) == 0 && chown(wrong_owner.c_str(), 1, 1) == 0,
              "preexisting wrong-owner child fixture created");
        PosixJobStore owner_store(wrong_owner_root.c_str(), owner);
        check(!owner_store.open_root(false), "preexisting wrong-owner child is refused");
    }
    std::puts("PASS: POSIX job preflight, private child, mode, symlink and rename race checks");
}
