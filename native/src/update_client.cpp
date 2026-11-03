// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/update_client.hpp"
#include "shelf/json.hpp"
#include "shelf/sha256.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

#ifdef SHELF_PS5
#include "fe_https.h"
#include <dirent.h>
#include <fcntl.h>
#include <minizip/unzip.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace shelf {
namespace {
void message(UpdateWorkOutcome& outcome, const char* text) {
    std::snprintf(outcome.message, sizeof(outcome.message), "%s", text ? text : "Update failed.");
}
bool copy(char* output, std::size_t capacity, const char* input) {
    if (!output || !capacity || !input || std::strlen(input) >= capacity) return false;
    std::memcpy(output, input, std::strlen(input) + 1);
    return true;
}
bool path(char* output, std::size_t capacity, const char* root, const char* suffix) {
    if (!output || !capacity || !root || !suffix) return false;
    const int used = std::snprintf(output, capacity, "%s/%s", root, suffix);
    return used > 0 && static_cast<std::size_t>(used) < capacity;
}

#ifdef SHELF_PS5
enum class PathState { missing, file, directory, other, failed };
PathState path_state(const char* name, std::uint64_t* bytes = nullptr) {
    struct stat info{};
    if (lstat(name, &info) != 0) {
        // PS5 application/data aliases can be readable even when pathname
        // metadata is unavailable. Fall back to an opened, no-symlink
        // descriptor so resumable state is not silently mistaken for missing.
        const int file = open(name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (file < 0) return errno == ENOENT ? PathState::missing : PathState::failed;
        const bool stated = fstat(file, &info) == 0;
        const bool closed = close(file) == 0;
        if (!stated || !closed) return PathState::failed;
    }
    if (bytes) *bytes = info.st_size >= 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
    if (S_ISREG(info.st_mode) && info.st_size >= 0) return PathState::file;
    if (S_ISDIR(info.st_mode)) return PathState::directory;
    return PathState::other;
}
bool directory(const char* name) {
    return path_state(name) == PathState::directory;
}
bool mkdirs(const char* name) {
    if (!name || name[0] != '/') return false;
    char value[2304];
    if (!copy(value, sizeof(value), name)) return false;
    for (char* at = value + 1; *at; ++at) {
        if (*at != '/') continue;
        *at = 0;
        if (mkdir(value, 0777) != 0 && errno != EEXIST) return false;
        if (!directory(value)) return false;
        *at = '/';
    }
    return (mkdir(value, 0777) == 0 || errno == EEXIST) && directory(value);
}
bool mkdirs_parent(const char* name) {
    char parent[2304];
    if (!copy(parent, sizeof(parent), name)) return false;
    char* slash = std::strrchr(parent, '/');
    if (!slash || slash == parent) return false;
    *slash = 0;
    return mkdirs(parent);
}
bool remove_tree(const char* name) {
    struct stat info{};
    if (lstat(name, &info) != 0) return errno == ENOENT;
    if (!S_ISDIR(info.st_mode)) return unlink(name) == 0;
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
        if (!path(child, sizeof(child), name, entry->d_name) || !remove_tree(child)) okay = false;
    }
    closedir(opened);
    return okay && rmdir(name) == 0;
}
bool flush(FILE* file) {
    return file && std::fflush(file) == 0 && fsync(fileno(file)) == 0;
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
            okay = std::feof(file) != 0 && std::ferror(file) == 0;
            break;
        }
    }
    okay = std::fclose(file) == 0 && okay;
    if (!okay) return false;
    sha256_hex(hash.finish(), output);
    if (bytes) *bytes = total;
    return true;
}
enum class SmallRead { missing, complete, invalid };
SmallRead read_small_state(const char* name, char*& output, std::size_t& bytes, std::size_t limit) {
    output = nullptr;
    bytes = 0;
    if (!name || !limit || limit == SIZE_MAX) return SmallRead::invalid;
    // /app0 is a PS5 application mount. Its files are readable through the
    // opened descriptor even on systems where a preceding lstat of the alias
    // is unavailable. Open the fixed path once, refuse symlinks, and enforce
    // the bound from the bytes actually read instead of trusting pathname
    // metadata in a separate check/use step.
    const int file = open(name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (file < 0) return errno == ENOENT ? SmallRead::missing : SmallRead::invalid;
    output = static_cast<char*>(std::malloc(limit + 1));
    bool okay = output != nullptr;
    std::size_t used = 0;
    while (okay && used <= limit) {
        const auto count = read(file, output + used, limit + 1 - used);
        if (count > 0) {
            used += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) break;
        if (errno != EINTR) okay = false;
    }
    okay = close(file) == 0 && okay && used > 0 && used <= limit;
    if (!okay) {
        std::free(output);
        output = nullptr;
        return SmallRead::invalid;
    }
    output[used] = 0;
    bytes = used;
    return SmallRead::complete;
}
bool read_small(const char* name, char*& output, std::size_t& bytes, std::size_t limit) {
    return read_small_state(name, output, bytes, limit) == SmallRead::complete;
}
bool write_atomic(const char* target, const char* temporary, const char* bytes, std::size_t size) {
    std::remove(temporary);
    FILE* file = std::fopen(temporary, "wb");
    bool okay = file && std::fwrite(bytes, 1, size, file) == size && flush(file);
    if (file) okay = std::fclose(file) == 0 && okay;
    if (!okay) {
        std::remove(temporary);
        return false;
    }
    if (std::rename(temporary, target) == 0) return true;
    std::remove(temporary);
    return false;
}

struct TextSink {
    char* data = nullptr;
    std::size_t used = 0, capacity = 0;
    SDL_atomic_t* cancelled = nullptr;
};
bool receive_text(TextSink& sink, const void* input, std::size_t bytes) {
    if (!sink.data || SDL_AtomicGet(sink.cancelled) || bytes > sink.capacity - sink.used) return false;
    std::memcpy(sink.data + sink.used, input, bytes);
    sink.used += bytes;
    return true;
}
struct FileSink {
    FILE* file = nullptr;
    std::uint64_t used = 0, expected = 0;
    SDL_atomic_t* cancelled = nullptr;
    SDL_atomic_t* progress = nullptr;
    bool failed = false;
};
bool receive_file(FileSink& sink, const void* input, std::size_t bytes) {
    if (!sink.file || SDL_AtomicGet(sink.cancelled) || bytes > sink.expected - sink.used ||
        std::fwrite(input, 1, bytes, sink.file) != bytes) {
        sink.failed = true;
        return false;
    }
    sink.used += bytes;
    if (sink.expected)
        SDL_AtomicSet(sink.progress, 5 + static_cast<int>(sink.used * 45 / sink.expected));
    return true;
}
bool allowed_api_host(const std::string& host) {
    return host == "api.github.com";
}
bool allowed_release_host(const std::string& host) {
    return host == "github.com" || host == "release-assets.githubusercontent.com" ||
           host == "objects.githubusercontent.com";
}
bool initialize_https(fe::HttpsClient& client, const char* ca_path,
                      char* error, std::size_t error_capacity) {
    char* pem = nullptr;
    std::size_t pem_bytes = 0;
    if (!read_small(ca_path, pem, pem_bytes, 512 * 1024)) {
        std::snprintf(error, error_capacity, "The updater certificate bundle is unavailable.");
        return false;
    }
    client.SkipBundledRoots();
    client.user_agent = "isShelf-updater/1";
    std::string detail;
    const bool okay = client.AddRoots(pem, detail) && client.Init(detail);
    std::free(pem);
    if (!okay)
        std::snprintf(error, error_capacity, "%s", detail.empty() ?
                      "The updater certificate bundle could not be loaded." : detail.c_str());
    return okay;
}
class ActiveHttps {
    std::mutex& mutex_;
    fe::HttpsClient*& active_;
    fe::HttpsClient* client_;

  public:
    ActiveHttps(std::mutex& mutex, fe::HttpsClient*& active, fe::HttpsClient& client)
        : mutex_(mutex), active_(active), client_(&client) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = client_;
    }
    ~ActiveHttps() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ == client_) active_ = nullptr;
    }
};
bool get_release_text(const char* ca_path, SDL_atomic_t& cancelled, char* data,
                      std::size_t capacity, std::size_t& used, long& status,
                      char* error, std::size_t error_capacity,
                      std::mutex& https_mutex, fe::HttpsClient*& active_https) {
    TextSink sink{data, 0, capacity, &cancelled};
    fe::HttpsClient client(fe::MakeConsoleHttpsPlatform(0, {}));
    ActiveHttps active(https_mutex, active_https, client);
    client.allow_host = allowed_api_host;
    if (!initialize_https(client, ca_path, error, error_capacity)) return false;
    fe::g_https_extra_headers =
        "Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    std::string detail;
    const int result = client.Request("GET", update_api_url, {}, {},
        [&sink](const void* input, std::size_t bytes) { return receive_text(sink, input, bytes); },
        180000, &detail);
    fe::g_https_extra_headers.clear();
    status = result > 0 ? result : 0;
    const bool okay = !SDL_AtomicGet(&cancelled) && (result == 200 || result == 404);
    if (!okay && error && error_capacity)
        std::snprintf(error, error_capacity, "%s",
                      SDL_AtomicGet(&cancelled) ? "Update check cancelled." :
                      detail.empty() ? "GitHub could not be reached securely." : detail.c_str());
    used = sink.used;
    return okay;
}
bool download_release(const UpdateRelease& release, const char* ca_path, const char* target,
                      SDL_atomic_t& cancelled, SDL_atomic_t& progress, char* error,
                      std::size_t error_capacity, std::mutex& https_mutex,
                      fe::HttpsClient*& active_https) {
    std::uint64_t offset = 0;
    std::uint64_t existing_bytes = 0;
    const auto existing = path_state(target, &existing_bytes);
    if (existing == PathState::file) {
        if (existing_bytes > release.bytes) {
            if (unlink(target) != 0) {
                std::snprintf(error, error_capacity, "The oversized partial update could not be cleared.");
                return false;
            }
        } else {
            offset = existing_bytes;
        }
    } else if (existing != PathState::missing) {
        std::snprintf(error, error_capacity, "The saved partial update could not be inspected.");
        return false;
    }
    if (offset == release.bytes) {
        SDL_AtomicSet(&progress, 50);
        return true;
    }
    FILE* file = std::fopen(target, offset ? "ab" : "wb");
    if (!file) {
        std::snprintf(error, error_capacity, "The update download file could not be created.");
        return false;
    }
    FileSink sink{file, offset, release.bytes, &cancelled, &progress};
    fe::HttpsClient client(fe::MakeConsoleHttpsPlatform(0, {}));
    ActiveHttps active(https_mutex, active_https, client);
    client.allow_host = allowed_release_host;
    std::string detail;
    const bool initialized = initialize_https(client, ca_path, error, error_capacity);
    const int result = initialized ? client.Get(release.asset_url, offset != 0, offset,
        release.bytes - offset,
        [&sink](const void* input, std::size_t bytes) { return receive_file(sink, input, bytes); },
        1200000, &detail) : fe::HttpsClient::kConnect;
    bool okay = initialized && !sink.failed && !SDL_AtomicGet(&cancelled) &&
                result == (offset ? 206 : 200) &&
                sink.used == release.bytes && flush(file);
    okay = std::fclose(file) == 0 && okay;
    if (!okay) {
        // The imported client never delivers an ignored whole response into
        // a partial file. Remove the partial and retry one clean full request.
        const bool retry_full = offset && result == fe::HttpsClient::kIgnoredRange &&
                                !SDL_AtomicGet(&cancelled);
        if (retry_full) std::remove(target);
        if (initialized)
            std::snprintf(error, error_capacity, "%s",
                          SDL_AtomicGet(&cancelled) ? "Update download cancelled." :
                          detail.empty() ?
                          "The update download was interrupted; it can resume later." : detail.c_str());
        if (retry_full)
            return download_release(release, ca_path, target, cancelled, progress,
                                    error, error_capacity, https_mutex, active_https);
        return false;
    }
    return okay;
}

bool download_state_text(const UpdateRelease& release, char* output, std::size_t capacity,
                         std::size_t& used) {
    const int written = std::snprintf(output, capacity,
        "isshelf-download 1\ntag=%s\nbytes=%llu\nsha256=%s\nurl=%s\n",
        release.tag, static_cast<unsigned long long>(release.bytes), release.sha256,
        release.asset_url);
    used = written > 0 ? static_cast<std::size_t>(written) : 0;
    return written > 0 && used < capacity;
}
bool prepare_download_state(const UpdateRelease& release, const char* archive,
                            const char* state, const char* temporary) {
    char expected[1024];
    std::size_t expected_bytes = 0;
    if (!download_state_text(release, expected, sizeof(expected), expected_bytes)) return false;
    char* observed = nullptr;
    std::size_t observed_bytes = 0;
    const auto state_kind = path_state(state);
    if (state_kind != PathState::missing && state_kind != PathState::file) return false;
    const bool have_state = state_kind == PathState::file &&
                            read_small(state, observed, observed_bytes, 4096);
    const bool matches = have_state && observed_bytes == expected_bytes &&
                         std::memcmp(observed, expected, expected_bytes) == 0;
    std::free(observed);
    if (!matches) {
        const auto archive_kind = path_state(archive);
        if (archive_kind == PathState::file) {
            if (unlink(archive) != 0) return false;
        } else if (archive_kind != PathState::missing) {
            return false;
        }
        if (state_kind == PathState::file && unlink(state) != 0) return false;
        if (!write_atomic(state, temporary, expected, expected_bytes)) return false;
    }
    return true;
}

bool zip_member_path(const char* name, std::size_t size, bool directory_entry,
                     char (&relative)[256]) {
    constexpr std::string_view prefix = "PPSA99541/";
    if (!name || !size || size >= 1024) return false;
    std::string_view full(name, size);
    for (const unsigned char c : full)
        if (c < 32 || c > 126 || c == '\\') return false;
    if (full.substr(0, prefix.size()) != prefix || full == "PPSA99541/")
        return directory_entry && full == "PPSA99541/";
    full.remove_prefix(prefix.size());
    if (directory_entry) {
        if (full.empty() || full.back() != '/') return false;
        full.remove_suffix(1);
    } else if (full.empty() || full.back() == '/') {
        return false;
    }
    if (!update_program_path(full)) return false;
    std::memcpy(relative, full.data(), full.size());
    relative[full.size()] = 0;
    return true;
}
bool validate_param(const char* name) {
    char* bytes = nullptr;
    std::size_t size = 0;
    if (!read_small(name, bytes, size, 1024 * 1024)) return false;
    JsonDocument document;
    const bool okay = document.parse(bytes, size) && cJSON_IsObject(document.root) &&
                      std::strcmp(json_string(document.root, "titleId"), "PPSA99541") == 0;
    std::free(bytes);
    return okay;
}
bool extract_release(const char* archive, const char* stage, UpdateManifestEntry* entries,
                     std::size_t& count, std::uint64_t& total, SDL_atomic_t& cancelled,
                     SDL_atomic_t& progress, char* error, std::size_t error_capacity) {
    count = 0;
    total = 0;
    unzFile zip = unzOpen64(archive);
    if (!zip) {
        std::snprintf(error, error_capacity, "The verified update ZIP could not be opened.");
        return false;
    }
    unz_global_info64 global{};
    bool okay = unzGetGlobalInfo64(zip, &global) == UNZ_OK && global.number_entry > 0 &&
                global.number_entry <= update_max_files * 2;
    if (okay) okay = unzGoToFirstFile(zip) == UNZ_OK;
    for (ZPOS64_T index = 0; okay && index < global.number_entry; ++index) {
        if (SDL_AtomicGet(&cancelled)) { okay = false; break; }
        unz_file_info64 info{};
        char name[1024]{};
        okay = unzGetCurrentFileInfo64(zip, &info, name, sizeof(name), nullptr, 0, nullptr, 0) == UNZ_OK &&
               info.size_filename < sizeof(name) && !(info.flag & 1);
        if (!okay) break;
        const bool is_directory = info.size_filename && name[info.size_filename - 1] == '/';
        const auto type = static_cast<unsigned>((info.external_fa >> 16) & S_IFMT);
        if (type && type != static_cast<unsigned>(is_directory ? S_IFDIR : S_IFREG)) { okay = false; break; }
        char relative[256]{};
        if (!zip_member_path(name, info.size_filename, is_directory, relative)) { okay = false; break; }
        if (!is_directory) {
            if (count >= update_max_files || info.uncompressed_size > update_max_archive_bytes ||
                total > update_max_archive_bytes - info.uncompressed_size) { okay = false; break; }
            for (std::size_t prior = 0; prior < count; ++prior)
                if (std::strcmp(entries[prior].path, relative) == 0) { okay = false; break; }
            if (!okay) break;
            copy(entries[count].path, sizeof(entries[count].path), relative);
            entries[count].bytes = info.uncompressed_size;
            total += info.uncompressed_size;
            ++count;
        }
        if (index + 1 < global.number_entry) okay = unzGoToNextFile(zip) == UNZ_OK;
    }
    bool have_eboot = false, have_param = false;
    for (std::size_t i = 0; i < count; ++i) {
        have_eboot = have_eboot || std::strcmp(entries[i].path, "eboot.bin") == 0;
        have_param = have_param || std::strcmp(entries[i].path, "sce_sys/param.json") == 0;
    }
    okay = okay && count && have_eboot && have_param && total <= update_max_archive_bytes;
    if (okay) {
        struct statfs space{};
        if (statfs(stage, &space) == 0) {
            const auto available = static_cast<std::uint64_t>(space.f_bavail) * space.f_bsize;
            okay = available >= total + 32ull * 1024 * 1024;
        }
    }
    if (okay) okay = unzGoToFirstFile(zip) == UNZ_OK;
    std::uint64_t completed = 0;
    for (ZPOS64_T index = 0; okay && index < global.number_entry; ++index) {
        unz_file_info64 info{};
        char name[1024]{};
        okay = !SDL_AtomicGet(&cancelled) &&
               unzGetCurrentFileInfo64(zip, &info, name, sizeof(name), nullptr, 0, nullptr, 0) == UNZ_OK &&
               info.size_filename < sizeof(name);
        if (!okay) break;
        const bool is_directory = info.size_filename && name[info.size_filename - 1] == '/';
        char relative[256]{};
        if (!zip_member_path(name, info.size_filename, is_directory, relative)) { okay = false; break; }
        if (!is_directory) {
            auto* entry = entries;
            while (entry != entries + count && std::strcmp(entry->path, relative) != 0) ++entry;
            if (entry == entries + count || unzOpenCurrentFile(zip) != UNZ_OK) { okay = false; break; }
            char target[2304];
            okay = path(target, sizeof(target), stage, relative) && mkdirs_parent(target);
            const int fd = okay ? open(target, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644) : -1;
            okay = fd >= 0;
            Sha256 hash;
            unsigned char buffer[65536];
            std::uint64_t written = 0;
            while (okay && written < entry->bytes) {
                const auto want = static_cast<unsigned>(std::min<std::uint64_t>(sizeof(buffer), entry->bytes - written));
                const int got = unzReadCurrentFile(zip, buffer, want);
                if (got <= 0 || write(fd, buffer, static_cast<std::size_t>(got)) != got) { okay = false; break; }
                hash.update(buffer, static_cast<std::size_t>(got));
                written += static_cast<std::size_t>(got);
                SDL_AtomicSet(&progress, 50 + static_cast<int>((completed + written) * 45 / (total ? total : 1)));
                if (SDL_AtomicGet(&cancelled)) okay = false;
            }
            if (fd >= 0) okay = fsync(fd) == 0 && close(fd) == 0 && okay;
            const int closed = unzCloseCurrentFile(zip);
            okay = okay && closed == UNZ_OK && written == entry->bytes;
            if (!okay) std::remove(target);
            else sha256_hex(hash.finish(), entry->sha256);
            completed += written;
        }
        if (index + 1 < global.number_entry) okay = unzGoToNextFile(zip) == UNZ_OK;
    }
    unzClose(zip);
    char param[2304];
    okay = okay && path(param, sizeof(param), stage, "sce_sys/param.json") && validate_param(param);
    if (!okay)
        std::snprintf(error, error_capacity, "%s",
                      SDL_AtomicGet(&cancelled) ? "Update preparation cancelled." :
                      "The update ZIP is unsafe, incomplete, or not an isShelf package.");
    return okay;
}
bool write_manifest(const char* root, const UpdateRelease& release, UpdateManifestEntry* entries,
                    std::size_t count, char (&digest)[65]) {
    std::sort(entries, entries + count, [](const auto& left, const auto& right) {
        return std::strcmp(left.path, right.path) < 0;
    });
    char target[2304], temporary[2304];
    if (!path(target, sizeof(target), root, "update/ready.manifest") ||
        !path(temporary, sizeof(temporary), root, "update/ready.manifest.tmp")) return false;
    std::remove(temporary);
    FILE* file = std::fopen(temporary, "wb");
    if (!file || std::fprintf(file, "isshelf-update 1\ntag=%s\ncount=%zu\n", release.tag, count) < 0) {
        if (file) std::fclose(file);
        return false;
    }
    bool okay = true;
    for (std::size_t i = 0; i < count; ++i)
        okay = okay && std::fprintf(file, "file=%s\tbytes=%llu\tsha256=%s\n", entries[i].path,
                                    static_cast<unsigned long long>(entries[i].bytes), entries[i].sha256) > 0;
    okay = okay && flush(file) && std::fclose(file) == 0;
    if (!okay || std::rename(temporary, target) != 0) {
        std::remove(temporary);
        return false;
    }
    return hash_file(target, digest);
}
#endif
} // namespace

UpdateClient::~UpdateClient() { stop(); }

bool UpdateClient::begin(Task task, UpdateWorkState state) {
    if (busy()) return false;
    stop();
    task_ = task;
    outcome_ = {};
    SDL_AtomicSet(&cancelled_, 0);
    SDL_AtomicSet(&progress_, 0);
    SDL_AtomicSet(&state_, static_cast<int>(state));
    thread_ = SDL_CreateThread(run, task == Task::check ? "Shelf update check" : "Shelf update prepare", this);
    if (thread_) return true;
    message(outcome_, "The update worker could not start.");
    outcome_.state = UpdateWorkState::failed;
    SDL_AtomicSet(&state_, static_cast<int>(UpdateWorkState::failed));
    return false;
}
bool UpdateClient::check(const char* data_root, const char* ca_path, const char* current) {
    if (!copy(data_root_, sizeof(data_root_), data_root) || !copy(ca_path_, sizeof(ca_path_), ca_path) ||
        !copy(current_, sizeof(current_), current)) return false;
    return begin(Task::check, UpdateWorkState::checking);
}
bool UpdateClient::prepare(const UpdateRelease& release, const char* data_root, const char* ca_path) {
    if (!copy(data_root_, sizeof(data_root_), data_root) || !copy(ca_path_, sizeof(ca_path_), ca_path)) return false;
    release_ = release;
    return begin(Task::prepare, UpdateWorkState::preparing);
}
bool UpdateClient::busy() const {
    const auto state = static_cast<UpdateWorkState>(SDL_AtomicGet(const_cast<SDL_atomic_t*>(&state_)));
    return state == UpdateWorkState::checking || state == UpdateWorkState::preparing;
}
int UpdateClient::progress() const {
    return SDL_AtomicGet(const_cast<SDL_atomic_t*>(&progress_));
}
bool UpdateClient::take(UpdateWorkOutcome& output) {
    const auto state = static_cast<UpdateWorkState>(SDL_AtomicGet(&state_));
    if (state == UpdateWorkState::idle || state == UpdateWorkState::checking || state == UpdateWorkState::preparing)
        return false;
    if (thread_) {
        SDL_WaitThread(thread_, nullptr);
        thread_ = nullptr;
    }
    output = outcome_;
    task_ = Task::none;
    SDL_AtomicSet(&state_, static_cast<int>(UpdateWorkState::idle));
    return true;
}
void UpdateClient::stop() {
    if (thread_) {
        SDL_AtomicSet(&cancelled_, 1);
#ifdef SHELF_PS5
        {
            std::lock_guard<std::mutex> lock(https_mutex_);
            if (active_https_) active_https_->Abort();
        }
#endif
        SDL_WaitThread(thread_, nullptr);
        thread_ = nullptr;
    }
    task_ = Task::none;
    SDL_AtomicSet(&state_, static_cast<int>(UpdateWorkState::idle));
}
int UpdateClient::run(void* raw) {
    auto& client = *static_cast<UpdateClient*>(raw);
    const int result = client.task_ == Task::check ? client.run_check() : client.run_prepare();
    SDL_AtomicSet(&client.state_, static_cast<int>(client.outcome_.state));
    return result;
}

int UpdateClient::run_check() {
#ifdef SHELF_PS5
    outcome_ = {};
    auto* body = static_cast<char*>(std::malloc(1024 * 1024 + 1));
    std::size_t used = 0;
    long status = 0;
    char error[192]{};
    const bool received = body && get_release_text(ca_path_, cancelled_, body, 1024 * 1024, used, status,
                                                    error, sizeof(error), https_mutex_, active_https_);
    if (received && body) body[used] = 0;
    if (SDL_AtomicGet(&cancelled_)) {
        outcome_.state = UpdateWorkState::cancelled;
        message(outcome_, "Update check cancelled.");
    } else if (!received) {
        outcome_.state = UpdateWorkState::failed;
        message(outcome_, body ? error : "Not enough memory to check for updates.");
    } else if (status == 404) {
        outcome_.state = UpdateWorkState::current;
        message(outcome_, "No production release has been published yet.");
    } else {
        const auto decision = parse_update_release({body, used}, current_, outcome_.release,
                                                   outcome_.message, sizeof(outcome_.message));
        outcome_.state = decision == UpdateDecision::available ? UpdateWorkState::available :
                         decision == UpdateDecision::current ? UpdateWorkState::current :
                         decision == UpdateDecision::newer ? UpdateWorkState::newer : UpdateWorkState::failed;
        if (outcome_.state == UpdateWorkState::available)
            std::snprintf(outcome_.message, sizeof(outcome_.message), "%s is ready to install.",
                          outcome_.release.tag);
        else if (outcome_.state == UpdateWorkState::current)
            std::snprintf(outcome_.message, sizeof(outcome_.message), "isShelf %s is up to date.", current_);
        else if (outcome_.state == UpdateWorkState::newer)
            std::snprintf(outcome_.message, sizeof(outcome_.message),
                          "This build is newer than production %s; nothing was changed.", outcome_.release.tag);
    }
    std::free(body);
    return outcome_.state == UpdateWorkState::failed ? 1 : 0;
#else
    outcome_.state = UpdateWorkState::failed;
    message(outcome_, "Update checks are available on the PS5 build.");
    return 1;
#endif
}

int UpdateClient::run_prepare() {
#ifdef SHELF_PS5
    outcome_ = {};
    outcome_.release = release_;
    char update_root[2304], stage_root[2304], archive[2304], journal[2304];
    char download_state[2304], download_state_temporary[2304];
    if (!path(update_root, sizeof(update_root), data_root_, "update") ||
        !path(stage_root, sizeof(stage_root), update_root, "stage/PPSA99541") ||
        !path(archive, sizeof(archive), update_root, "release.zip.part") ||
        !path(journal, sizeof(journal), update_root, "journal.txt") ||
        !path(download_state, sizeof(download_state), update_root, "download.state") ||
        !path(download_state_temporary, sizeof(download_state_temporary), update_root,
              "download.state.tmp") || !mkdirs(update_root)) {
        outcome_.state = UpdateWorkState::failed;
        message(outcome_, "The owned update folder is unavailable.");
        return 1;
    }
    const auto journal_kind = path_state(journal);
    if (journal_kind != PathState::missing) {
        outcome_.state = UpdateWorkState::failed;
        message(outcome_, journal_kind == PathState::file ?
                         "An interrupted update must be recovered first." :
                         "The update journal path is not a regular owned file.");
        return 1;
    }
    if (!prepare_download_state(release_, archive, download_state,
                                download_state_temporary)) {
        outcome_.state = UpdateWorkState::failed;
        message(outcome_, "The resumable update record could not be prepared safely.");
        return 1;
    }
    char stage_parent[2304];
    path(stage_parent, sizeof(stage_parent), update_root, "stage");
    if (!remove_tree(stage_parent) || !mkdirs(stage_root)) {
        outcome_.state = UpdateWorkState::failed;
        message(outcome_, "Old update staging files could not be cleared safely.");
        return 1;
    }
    char error[192]{};
    bool okay = download_release(release_, ca_path_, archive, cancelled_, progress_, error,
                                 sizeof(error), https_mutex_, active_https_);
    const bool download_complete = okay;
    char archive_sha[65]{};
    std::uint64_t archive_bytes = 0;
    okay = okay && hash_file(archive, archive_sha, &archive_bytes) && archive_bytes == release_.bytes &&
           std::strcmp(archive_sha, release_.sha256) == 0;
    const bool archive_verified = okay;
    if (!okay && !error[0]) std::snprintf(error, sizeof(error), "The downloaded update failed SHA-256 verification.");
    auto* entries = static_cast<UpdateManifestEntry*>(std::calloc(update_max_files, sizeof(UpdateManifestEntry)));
    std::size_t count = 0;
    std::uint64_t total = 0;
    if (okay && entries)
        okay = extract_release(archive, stage_root, entries, count, total, cancelled_, progress_, error, sizeof(error));
    else if (okay)
        std::snprintf(error, sizeof(error), "Not enough memory to verify the update package.");
    if (okay)
        okay = write_manifest(data_root_, release_, entries, count, outcome_.manifest_sha256);
    if (!okay && !error[0]) std::snprintf(error, sizeof(error), "The verified update could not be staged.");
    std::free(entries);
    if (SDL_AtomicGet(&cancelled_)) {
        outcome_.state = UpdateWorkState::cancelled;
        message(outcome_, "Update preparation cancelled.");
        return 1;
    }
    if (!okay) {
        remove_tree(stage_parent);
        // Preserve a bounded partial/full verified download on cancellation or
        // transient preparation failure. A complete digest mismatch is never
        // retained for a future resume attempt.
        if (download_complete && !archive_verified) {
            std::remove(archive);
            std::remove(download_state);
        }
        outcome_.state = UpdateWorkState::failed;
        message(outcome_, error);
        return 1;
    }
    std::remove(archive);
    std::remove(download_state);
    SDL_AtomicSet(&progress_, 100);
    outcome_.state = UpdateWorkState::ready;
    std::snprintf(outcome_.message, sizeof(outcome_.message),
                  "%s verified and ready. isShelf will close, install, and reopen.", release_.tag);
    return 0;
#else
    outcome_.state = UpdateWorkState::failed;
    message(outcome_, "Update installation is available on the PS5 build.");
    return 1;
#endif
}

bool update_recovery_pending(const char* data_root, char (&tag)[32],
                             char (&manifest_sha256)[65], bool& committed) {
    tag[0] = manifest_sha256[0] = 0;
    committed = false;
#ifdef SHELF_PS5
    char journal[2304], manifest_path[2304];
    if (!path(journal, sizeof(journal), data_root, "update/journal.txt") ||
        !path(manifest_path, sizeof(manifest_path), data_root, "update/ready.manifest")) return false;
    char* journal_text = nullptr;
    std::size_t journal_bytes = 0;
    char* manifest_text = nullptr;
    std::size_t manifest_bytes = 0;
    if (read_small_state(manifest_path, manifest_text, manifest_bytes, 256 * 1024) !=
        SmallRead::complete) {
        std::free(journal_text);
        std::free(manifest_text);
        return false;
    }
    UpdateManifestEntry* entries = static_cast<UpdateManifestEntry*>(
        std::calloc(update_max_files, sizeof(UpdateManifestEntry)));
    UpdateManifest parsed;
    const bool parsed_ok = entries && parse_update_manifest({manifest_text, manifest_bytes}, parsed,
                                                             entries, update_max_files);
    char observed[65]{};
    const bool hashed = hash_file(manifest_path, observed);
    char expected[65]{}, state[16]{}, extra = 0;
    const auto journal_state = read_small_state(journal, journal_text, journal_bytes, 512);
    bool journal_ok = journal_state == SmallRead::missing;
    if (journal_state == SmallRead::complete) {
        journal_ok = true;
        const int fields = journal_ok ? std::sscanf(journal_text,
            "isshelf-update-journal 1\nmanifest=%64[0-9a-f]\nstate=%15[A-Z]\n%c",
            expected, state, &extra) : 0;
        journal_ok = journal_ok && fields == 2 && update_hex_sha256(expected) &&
                     std::strcmp(expected, observed) == 0 &&
                     (std::strcmp(state, "BEGIN") == 0 || std::strcmp(state, "COMMIT") == 0);
    }
    const bool okay = parsed_ok && hashed && journal_ok;
    if (okay) {
        copy(tag, sizeof(tag), parsed.tag);
        copy(manifest_sha256, sizeof(manifest_sha256), observed);
        committed = state[0] && std::strcmp(state, "COMMIT") == 0;
    }
    std::free(entries);
    std::free(journal_text);
    std::free(manifest_text);
    return okay;
#else
    (void)data_root;
    return false;
#endif
}

bool take_update_result(const char* data_root, char (&notice)[192]) {
    notice[0] = 0;
#ifdef SHELF_PS5
    char result_path[2304];
    if (!path(result_path, sizeof(result_path), data_root, "update/result.txt")) return false;
    char* text = nullptr;
    std::size_t bytes = 0;
    if (read_small_state(result_path, text, bytes, 1024) != SmallRead::complete) return false;
    constexpr std::string_view header = "isshelf-update-result 1\n";
    std::string_view input(text, bytes);
    auto field = [&input](std::string_view prefix, std::string_view& value) {
        const auto end = input.find('\n');
        if (end == input.npos || end < prefix.size() || input.size() < prefix.size() ||
            std::memcmp(input.data(), prefix.data(), prefix.size()) != 0) return false;
        value = std::string_view(input.data() + prefix.size(), end - prefix.size());
        input.remove_prefix(end + 1);
        return true;
    };
    std::string_view state, tag, detail;
    unsigned version[3]{};
    bool okay = input.size() >= header.size() &&
                std::memcmp(input.data(), header.data(), header.size()) == 0;
    if (okay) input.remove_prefix(header.size());
    okay = okay && field("state=", state) && field("tag=", tag) &&
           field("detail=", detail) && input.empty() && update_semver(tag, version) &&
           detail.size() < sizeof(notice) &&
           (state == "installed" || state == "rolled-back" ||
            state == "cleanup-pending" || state == "recovery-failed");
    for (const unsigned char c : detail)
        if (c < 32 || c == 127) okay = false;
    if (okay) {
        std::memcpy(notice, detail.data(), detail.size());
        notice[detail.size()] = 0;
        okay = std::remove(result_path) == 0;
    }
    std::free(text);
    return okay;
#else
    (void)data_root;
    return false;
#endif
}
} // namespace shelf
