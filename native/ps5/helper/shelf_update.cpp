// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/update_helper.hpp"
#include "shelf/sha256.hpp"
#include "shelf/update_protocol.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef SHELF_UPDATE_DATA_ROOT
#define SHELF_UPDATE_DATA_ROOT "/data/console-shelf"
#endif
#ifndef SHELF_UPDATE_APP_ROOT
#define SHELF_UPDATE_APP_ROOT "/data/homebrew/PPSA99541"
#endif

namespace shelf {
namespace {
[[gnu::used]] const char update_transaction_marker[] =
    "ISSHELF_IN_APP_UPDATER_V1";
[[gnu::used]] const char update_program_modes_marker[] =
    "ISSHELF_UPDATE_PROGRAM_MODES_V2";
constexpr char manifest_path[] = SHELF_UPDATE_DATA_ROOT "/update/ready.manifest";
constexpr char manifest_temporary[] = SHELF_UPDATE_DATA_ROOT "/update/installed.manifest.tmp";
constexpr char installed_manifest[] = SHELF_UPDATE_DATA_ROOT "/update/installed.manifest";
constexpr char journal_path[] = SHELF_UPDATE_DATA_ROOT "/update/journal.txt";
constexpr char journal_temporary[] = SHELF_UPDATE_DATA_ROOT "/update/journal.tmp";
constexpr char stage_root[] = SHELF_UPDATE_DATA_ROOT "/update/stage/PPSA99541";
constexpr char stage_parent[] = SHELF_UPDATE_DATA_ROOT "/update/stage";
constexpr char backup_root[] = SHELF_UPDATE_DATA_ROOT "/update/backup/PPSA99541";
constexpr char backup_parent[] = SHELF_UPDATE_DATA_ROOT "/update/backup";
constexpr char target_root[] = SHELF_UPDATE_APP_ROOT;
constexpr char result_path[] = SHELF_UPDATE_DATA_ROOT "/update/result.txt";
constexpr char result_temporary[] = SHELF_UPDATE_DATA_ROOT "/update/result.tmp";

void announce(UpdateTransactionNotifier notifier, void* context,
              UpdateTransactionStage stage, const char* tag) {
    if (notifier)
        notifier(context, stage, tag);
}

enum class Node { missing, file, directory, other, failed };
struct LoadedManifest {
    char* text = nullptr;
    std::size_t bytes = 0;
    UpdateManifest header;
    UpdateManifestEntry* entries = nullptr;
    char sha256[65]{};
    ~LoadedManifest() {
        std::free(text);
        std::free(entries);
    }
};
Node node(const char* name, std::uint64_t* bytes = nullptr) {
    struct stat info{};
    if (lstat(name, &info) != 0) return errno == ENOENT ? Node::missing : Node::failed;
    if (bytes) *bytes = info.st_size >= 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
    if (S_ISREG(info.st_mode)) return Node::file;
    if (S_ISDIR(info.st_mode)) return Node::directory;
    return Node::other;
}
mode_t program_mode(const char* relative) {
    return std::strcmp(relative, "eboot.bin") == 0 ||
           std::strcmp(relative, "sce_module/libc.prx") == 0
        ? 0755 : 0644;
}
bool mode_matches(const char* name, mode_t expected) {
    struct stat info{};
    return lstat(name, &info) == 0 && S_ISREG(info.st_mode) &&
           (info.st_mode & 0777) == expected;
}
bool make_path(char* output, std::size_t capacity, const char* root, const char* relative) {
    const int used = std::snprintf(output, capacity, "%s/%s", root, relative);
    return used > 0 && static_cast<std::size_t>(used) < capacity;
}
bool sync_parent(const char* name) {
    char parent[2304];
    if (!name || std::strlen(name) >= sizeof(parent)) return false;
    std::strcpy(parent, name);
    char* slash = std::strrchr(parent, '/');
    if (!slash) return false;
    if (slash == parent)
        slash[1] = 0;
    else
        *slash = 0;
    const int descriptor = open(parent, O_RDONLY | O_DIRECTORY);
    if (descriptor < 0) return false;
    bool okay = fsync(descriptor) == 0;
    okay = close(descriptor) == 0 && okay;
    return okay;
}
bool set_mode_durable(const char* name, mode_t expected) {
    const int descriptor = open(name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return false;
    bool okay = fchmod(descriptor, expected) == 0;
    if (okay) okay = fsync(descriptor) == 0;
    struct stat info{};
    if (okay)
        okay = fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) &&
               (info.st_mode & 0777) == expected;
    okay = close(descriptor) == 0 && okay;
    return okay && mode_matches(name, expected);
}
bool rename_durable(const char* source, const char* target) {
    return std::rename(source, target) == 0 && sync_parent(target) && sync_parent(source);
}
bool unlink_durable(const char* name) {
    return unlink(name) == 0 && sync_parent(name);
}
bool mkdirs(const char* name) {
    if (!name || name[0] != '/') return false;
    char value[2304];
    if (std::strlen(name) >= sizeof(value)) return false;
    std::strcpy(value, name);
    for (char* at = value + 1; *at; ++at) {
        if (*at != '/') continue;
        *at = 0;
        const auto state = node(value);
        if (state == Node::missing) {
            if (mkdir(value, 0777) != 0 || !sync_parent(value)) return false;
        } else if (state != Node::directory) return false;
        *at = '/';
    }
    const auto state = node(value);
    return state == Node::directory ||
           (state == Node::missing && mkdir(value, 0777) == 0 && sync_parent(value));
}
bool mkdirs_parent(const char* name) {
    char parent[2304];
    if (!name || std::strlen(name) >= sizeof(parent)) return false;
    std::strcpy(parent, name);
    char* slash = std::strrchr(parent, '/');
    if (!slash || slash == parent) return false;
    *slash = 0;
    return mkdirs(parent);
}
bool remove_tree(const char* name) {
    const auto state = node(name);
    if (state == Node::missing) return true;
    if (state == Node::file || state == Node::other) return unlink_durable(name);
    if (state != Node::directory) return false;
    DIR* opened = opendir(name);
    if (!opened) return false;
    bool okay = true;
    while (okay) {
        errno = 0;
        dirent* entry = readdir(opened);
        if (!entry) {
            okay = errno == 0;
            break;
        }
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        char child[2304];
        if (!make_path(child, sizeof(child), name, entry->d_name) || !remove_tree(child)) okay = false;
    }
    closedir(opened);
    return okay && rmdir(name) == 0 && sync_parent(name);
}
bool flush(FILE* file) {
    return file && std::fflush(file) == 0 && fsync(fileno(file)) == 0;
}
bool write_atomic(const char* target, const char* temporary, const char* text) {
    std::remove(temporary);
    FILE* file = std::fopen(temporary, "wb");
    if (!file) return false;
    const auto bytes = std::strlen(text);
    bool okay = std::fwrite(text, 1, bytes, file) == bytes && flush(file);
    okay = std::fclose(file) == 0 && okay;
    if (okay && rename_durable(temporary, target)) return true;
    std::remove(temporary);
    return false;
}
bool copy_file_atomic(const char* source, const char* target, const char* temporary) {
    FILE* incoming = std::fopen(source, "rb");
    if (!incoming) return false;
    std::remove(temporary);
    FILE* outgoing = std::fopen(temporary, "wb");
    unsigned char buffer[65536];
    bool okay = outgoing != nullptr;
    while (okay) {
        const auto used = std::fread(buffer, 1, sizeof(buffer), incoming);
        if (used && std::fwrite(buffer, 1, used, outgoing) != used) okay = false;
        if (used < sizeof(buffer)) {
            okay = okay && std::feof(incoming) && !std::ferror(incoming);
            break;
        }
    }
    okay = std::fclose(incoming) == 0 && okay;
    if (outgoing) {
        const bool flushed = flush(outgoing);
        const bool closed = std::fclose(outgoing) == 0;
        okay = flushed && closed && okay;
    }
    if (okay && rename_durable(temporary, target)) return true;
    std::remove(temporary);
    return false;
}
bool read_file(const char* name, char*& output, std::size_t& bytes, std::size_t limit) {
    output = nullptr;
    bytes = 0;
    std::uint64_t size = 0;
    if (node(name, &size) != Node::file || !size || size > limit) return false;
    output = static_cast<char*>(std::malloc(static_cast<std::size_t>(size) + 1));
    if (!output) return false;
    FILE* file = std::fopen(name, "rb");
    if (!file) {
        std::free(output);
        output = nullptr;
        return false;
    }
    bool okay = std::fread(output, 1, static_cast<std::size_t>(size), file) == size && !std::ferror(file);
    okay = std::fclose(file) == 0 && okay;
    if (!okay) {
        std::free(output);
        output = nullptr;
        return false;
    }
    output[size] = 0;
    bytes = static_cast<std::size_t>(size);
    return true;
}
bool hash_file(const char* name, char (&output)[65], std::uint64_t* bytes = nullptr) {
    FILE* file = std::fopen(name, "rb");
    if (!file) return false;
    Sha256 hash;
    unsigned char buffer[65536];
    std::uint64_t total = 0;
    bool okay = true;
    while (okay) {
        const auto used = std::fread(buffer, 1, sizeof(buffer), file);
        if (used) {
            hash.update(buffer, used);
            total += used;
        }
        if (used < sizeof(buffer)) {
            okay = std::feof(file) && !std::ferror(file);
            break;
        }
    }
    okay = std::fclose(file) == 0 && okay;
    if (!okay) return false;
    sha256_hex(hash.finish(), output);
    if (bytes) *bytes = total;
    return true;
}
bool load_manifest(const LaunchRequest& request, LoadedManifest& loaded) {
    if (!read_file(manifest_path, loaded.text, loaded.bytes, 256 * 1024)) return false;
    loaded.entries = static_cast<UpdateManifestEntry*>(std::calloc(update_max_files, sizeof(UpdateManifestEntry)));
    if (!loaded.entries || !parse_update_manifest({loaded.text, loaded.bytes}, loaded.header,
                                                   loaded.entries, update_max_files) ||
        std::strcmp(loaded.header.tag, request.rom) != 0 ||
        !hash_file(manifest_path, loaded.sha256) || std::strcmp(loaded.sha256, request.core) != 0)
        return false;
    return true;
}
bool journal(char* state, char (&manifest_sha)[65]) {
    char* text = nullptr;
    std::size_t bytes = 0;
    if (!read_file(journal_path, text, bytes, 512)) return false;
    char extra = 0;
    const int fields = std::sscanf(text,
        "isshelf-update-journal 1\nmanifest=%64[0-9a-f]\nstate=%15[A-Z]\n%c",
        manifest_sha, state, &extra);
    std::free(text);
    return fields == 2 && update_hex_sha256(manifest_sha) &&
           (std::strcmp(state, "BEGIN") == 0 || std::strcmp(state, "COMMIT") == 0);
}
bool write_journal(const char* sha, const char* state) {
    char text[160];
    const int used = std::snprintf(text, sizeof(text),
        "isshelf-update-journal 1\nmanifest=%s\nstate=%s\n", sha, state);
    return used > 0 && static_cast<std::size_t>(used) < sizeof(text) &&
           write_atomic(journal_path, journal_temporary, text);
}
void result(const char* state, const char* tag, const char* detail) {
    char text[512];
    const int used = std::snprintf(text, sizeof(text),
        "isshelf-update-result 1\nstate=%s\ntag=%s\ndetail=%s\n", state, tag, detail);
    if (used > 0 && static_cast<std::size_t>(used) < sizeof(text))
        (void)write_atomic(result_path, result_temporary, text);
}
bool staged_files_match(const LoadedManifest& loaded) {
    for (std::size_t i = 0; i < loaded.header.count; ++i) {
        char staged[2304], observed[65];
        std::uint64_t bytes = 0;
        if (!make_path(staged, sizeof(staged), stage_root, loaded.entries[i].path) ||
            node(staged, &bytes) != Node::file || bytes != loaded.entries[i].bytes ||
            !hash_file(staged, observed) || std::strcmp(observed, loaded.entries[i].sha256) != 0)
            return false;
    }
    return true;
}
bool target_files_match(const LoadedManifest& loaded) {
    for (std::size_t i = 0; i < loaded.header.count; ++i) {
        char target[2304], observed[65];
        std::uint64_t bytes = 0;
        if (!make_path(target, sizeof(target), target_root, loaded.entries[i].path) ||
            node(target, &bytes) != Node::file || bytes != loaded.entries[i].bytes ||
            !mode_matches(target, program_mode(loaded.entries[i].path)) ||
            !hash_file(target, observed) || std::strcmp(observed, loaded.entries[i].sha256) != 0)
            return false;
    }
    return true;
}
bool only_empty_directories(const char* name) {
    const auto state = node(name);
    if (state == Node::missing) return true;
    if (state != Node::directory) return false;
    DIR* opened = opendir(name);
    if (!opened) return false;
    bool okay = true;
    while (okay) {
        errno = 0;
        dirent* entry = readdir(opened);
        if (!entry) {
            okay = errno == 0;
            break;
        }
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        char child[2304];
        if (!make_path(child, sizeof(child), name, entry->d_name) ||
            !only_empty_directories(child))
            okay = false;
    }
    closedir(opened);
    return okay;
}
bool preflight_targets(const LoadedManifest& loaded) {
    const auto backup_state = node(backup_parent);
    if (backup_state == Node::directory && only_empty_directories(backup_parent)) {
        if (!remove_tree(backup_parent)) return false;
    } else if (backup_state != Node::missing) {
        return false;
    }
    if (node(target_root) != Node::directory || !mkdirs(backup_root))
        return false;
    for (std::size_t i = 0; i < loaded.header.count; ++i) {
        char target[2304], backup[2304];
        if (!make_path(target, sizeof(target), target_root, loaded.entries[i].path) ||
            !make_path(backup, sizeof(backup), backup_root, loaded.entries[i].path) ||
            !mkdirs_parent(target) || !mkdirs_parent(backup))
            return false;
        const auto target_state = node(target);
        if (target_state != Node::missing && target_state != Node::file) return false;
        if (target_state == Node::file &&
            !mode_matches(target, program_mode(loaded.entries[i].path))) return false;
    }
    return true;
}
#ifdef SHELF_UPDATE_HOST_TEST
bool test_fail_before_placement(std::size_t placed) {
    const char* value = std::getenv("ISSHELF_TEST_FAIL_BEFORE_PLACEMENT");
    if (!value || !value[0]) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long selected = std::strtoul(value, &end, 10);
    return errno == 0 && end != value && end && !*end && selected == placed;
}
#endif
bool rollback(const LoadedManifest& loaded, FILE* log) {
    bool okay = true;
    for (std::size_t index = loaded.header.count; index > 0; --index) {
        const auto& entry = loaded.entries[index - 1];
        char staged[2304], target[2304], backup[2304];
        if (!make_path(staged, sizeof(staged), stage_root, entry.path) ||
            !make_path(target, sizeof(target), target_root, entry.path) ||
            !make_path(backup, sizeof(backup), backup_root, entry.path)) {
            okay = false;
            continue;
        }
        const auto staged_state = node(staged), target_state = node(target), backup_state = node(backup);
        if (backup_state == Node::file) {
            if (target_state == Node::file) {
                if (staged_state != Node::missing || !mkdirs_parent(staged) ||
                    !rename_durable(target, staged)) {
                    okay = false;
                    continue;
                }
            } else if (target_state != Node::missing) {
                okay = false;
                continue;
            }
            if (!rename_durable(backup, target) ||
                !mode_matches(target, program_mode(entry.path))) okay = false;
        } else if (backup_state != Node::missing) {
            okay = false;
        } else if (target_state == Node::file && staged_state == Node::missing) {
            if (!mkdirs_parent(staged) || !rename_durable(target, staged)) okay = false;
        } else if ((target_state != Node::missing && target_state != Node::file) ||
                   (staged_state != Node::missing && staged_state != Node::file)) {
            okay = false;
        }
    }
    if (okay) okay = remove_tree(backup_parent);
    if (log) {
        std::fprintf(log, "update rollback: %s\n", okay ? "complete" : "failed");
        std::fflush(log);
    }
    return okay;
}
bool finalize(const LoadedManifest& loaded, FILE* log) {
    bool okay = copy_file_atomic(manifest_path, installed_manifest, manifest_temporary);
    if (okay) okay = remove_tree(backup_parent);
    if (okay) okay = remove_tree(stage_parent);
    if (okay) okay = unlink_durable(journal_path);
    if (okay) okay = unlink_durable(manifest_path);
    if (log) {
        std::fprintf(log, "update commit cleanup: %s tag=%s files=%zu\n",
                     okay ? "complete" : "pending", loaded.header.tag, loaded.header.count);
        std::fflush(log);
    }
    return okay;
}
int recover(const LaunchRequest& request, LoadedManifest& loaded, FILE* log, bool& found,
            UpdateTransactionNotifier notifier, void* notifier_context) {
    found = false;
    const auto state = node(journal_path);
    if (state == Node::missing) return 0;
    if (state != Node::file) return -EIO;
    found = true;
    char journal_state[16]{}, journal_sha[65]{};
    if (!journal(journal_state, journal_sha) || std::strcmp(journal_sha, loaded.sha256) != 0)
        return -EINVAL;
    if (std::strcmp(journal_state, "COMMIT") == 0) {
        announce(notifier, notifier_context, UpdateTransactionStage::finishing, request.rom);
        if (!target_files_match(loaded)) {
            result("recovery-failed", request.rom,
                   "Committed program bytes or executable permissions no longer match the approved update.");
            return -EIO;
        }
        if (!finalize(loaded, log)) return -EIO;
        result("installed", request.rom, "Interrupted cleanup completed; the verified update was already committed.");
        return 1;
    }
    announce(notifier, notifier_context, UpdateTransactionStage::restoring, request.rom);
    if (!rollback(loaded, log)) {
        result("recovery-failed", request.rom, "Rollback could not restore every previous program file.");
        return -EIO;
    }
    if (!unlink_durable(journal_path)) return -EIO;
    result("rolled-back", request.rom, "The interrupted update was rolled back; previous program files were restored.");
    return 2;
}
int apply(const LaunchRequest& request, LoadedManifest& loaded, FILE* log,
          UpdateTransactionNotifier notifier, void* notifier_context) {
    if (!staged_files_match(loaded) || !preflight_targets(loaded)) return -EINVAL;
    announce(notifier, notifier_context, UpdateTransactionStage::installing, request.rom);
    if (!write_journal(loaded.sha256, "BEGIN")) return -EIO;
    bool okay = true;
#ifdef SHELF_UPDATE_HOST_TEST
    std::size_t placed = 0;
#endif
    for (int pass = 0; pass < 2 && okay; ++pass) {
        for (std::size_t i = 0; i < loaded.header.count && okay; ++i) {
            const auto& entry = loaded.entries[i];
            const bool executable = std::strcmp(entry.path, "eboot.bin") == 0;
            if (executable != (pass == 1)) continue;
#ifdef SHELF_UPDATE_HOST_TEST
            if (test_fail_before_placement(placed)) {
                okay = false;
                break;
            }
#endif
            char staged[2304], target[2304], backup[2304];
            okay = make_path(staged, sizeof(staged), stage_root, entry.path) &&
                   make_path(target, sizeof(target), target_root, entry.path) &&
                   make_path(backup, sizeof(backup), backup_root, entry.path);
            if (!okay) break;
            const auto target_state = node(target);
            if (target_state == Node::file)
                okay = rename_durable(target, backup);
            else
                okay = target_state == Node::missing;
            const mode_t expected_mode = program_mode(entry.path);
            if (okay) okay = rename_durable(staged, target) &&
                             set_mode_durable(target, expected_mode);
#ifdef SHELF_UPDATE_HOST_TEST
            if (okay) ++placed;
#endif
            if (log) {
                std::fprintf(log, "update file: %s %s mode=%04o%s\n",
                             okay ? "placed" : "failed", entry.path,
                             static_cast<unsigned>(expected_mode),
                             executable ? " (entrypoint last)" : "");
                std::fflush(log);
            }
        }
    }
    if (okay) okay = target_files_match(loaded);
    if (!okay) {
        announce(notifier, notifier_context, UpdateTransactionStage::restoring, request.rom);
        const bool restored = rollback(loaded, log);
        if (restored) (void)unlink_durable(journal_path);
        result(restored ? "rolled-back" : "recovery-failed", request.rom,
               restored ? "The update failed and every previous program file was restored."
                        : "The update failed and automatic rollback needs attention.");
        return -EIO;
    }
    if (!write_journal(loaded.sha256, "COMMIT")) {
        announce(notifier, notifier_context, UpdateTransactionStage::restoring, request.rom);
        const bool restored = rollback(loaded, log);
        if (restored) (void)unlink_durable(journal_path);
        result(restored ? "rolled-back" : "recovery-failed", request.rom,
               restored ? "The update could not commit; previous program files were restored."
                        : "The update could not commit and automatic rollback needs attention.");
        return -EIO;
    }
    announce(notifier, notifier_context, UpdateTransactionStage::finishing, request.rom);
    if (!finalize(loaded, log)) {
        result("cleanup-pending", request.rom,
               "The verified update committed; cleanup will finish the next time isShelf opens.");
        return 1;
    }
    result("installed", request.rom, "The verified production update installed successfully.");
    return 0;
}
} // namespace

int apply_update_transaction(const LaunchRequest& request, FILE* log,
                             UpdateTransactionNotifier notifier,
                             void* notifier_context) {
    if (log) {
        std::fprintf(log, "update transaction: %s modes=%s\n",
                     update_transaction_marker, update_program_modes_marker);
        std::fflush(log);
    }
    if ((request.mode != LaunchMode::self_update && request.mode != LaunchMode::update_recover) ||
        std::strcmp(request.title, shelf_title_id) != 0 || !update_hex_sha256(request.core))
        return -EINVAL;
    announce(notifier, notifier_context, UpdateTransactionStage::verifying, request.rom);
    LoadedManifest loaded;
    if (!load_manifest(request, loaded)) {
        result("recovery-failed", request.rom, "The staged manifest no longer matches the owner-approved update.");
        return -EINVAL;
    }
    bool found = false;
    if (node(journal_path) != Node::missing)
        announce(notifier, notifier_context, UpdateTransactionStage::recovering, request.rom);
    const int recovered = recover(request, loaded, log, found, notifier, notifier_context);
    if (recovered < 0) return recovered;
    // A BEGIN recovery restores every staged file, so the same explicitly
    // approved transaction can now continue once. A staged manifest with no
    // journal covers power loss between preparation and the first install
    // mutation. A committed recovery is already complete and is never
    // reinstalled.
    if (recovered == 1) return 0;
    // finalize() removes COMMIT before ready.manifest. If power is lost in
    // that narrow two-unlink window, the installed manifest and every target
    // byte prove that the transaction had already committed. Finish only the
    // updater-owned cleanup; never reapply or touch an unlisted file.
    if (!found && node(installed_manifest) == Node::file) {
        char installed_sha[65]{};
        if (hash_file(installed_manifest, installed_sha) &&
            std::strcmp(installed_sha, loaded.sha256) == 0 && target_files_match(loaded)) {
            announce(notifier, notifier_context, UpdateTransactionStage::finishing, request.rom);
            const bool cleaned = remove_tree(backup_parent) && remove_tree(stage_parent) &&
                                 unlink_durable(manifest_path);
            result(cleaned ? "installed" : "cleanup-pending", request.rom,
                   cleaned ? "Interrupted committed cleanup completed; user data was left unchanged."
                           : "The verified update is installed; updater cleanup will retry later.");
            return cleaned ? 0 : 1;
        }
    }
    return apply(request, loaded, log, notifier, notifier_context);
}
} // namespace shelf
