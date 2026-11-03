// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_manager.hpp"
#include "shelf/console_tile_forwarder_config.hpp"
#include "shelf/json.hpp"
#include "../../tools/console_badges/generic_tile_catalog.generated.hpp"
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string_view>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace shelf {
namespace {
static_assert(system_count == isshelf_badge::generic_tile::tile_count);
constexpr unsigned preferred_first = 99700;
constexpr unsigned alternate_first = 99752;
constexpr unsigned last_number = 99899;
constexpr unsigned reserved_number = 99764;
constexpr char forwarder_source[] = "https://github.com/heydemoura/ps5-forwarder-format";
constexpr char forwarder_commit[] = "22e8762ad7dbd01e18bd8c0e95f86461fdf8aaf4";
constexpr char owner_format[] = "isshelf-console-tile/1";
constexpr char forwarder_format[] = "ps5-forwarder/1";
constexpr char template_eboot_hash[] = "8e43160e64a4f3fe9f84a2a7cd4b9175772ff9897c3bd8a25870f32b0d936638";
// UI107 added one calm beta11 unmount retry after five idle seconds and paced
// fallback reads. UI105-UI106 added the first unmount assist and duplicate-
// request coalescing. UI103-UI104 used the release-gated v3 relay, UI102 introduced
// truthful shared-RetroArch routing, and UI98-UI101 used the earlier v3 relay.
// Keep those exact binaries, plus the v2 predecessors, as bounded migration
// inputs for the current cooperative v3 forwarder.
// Unknown executables remain conflicts.
constexpr char previous_unmount_assisted_template_eboot_hash[] =
    "0adc27f409cdecd73812b55abca30efc52ff71a7f42c2509b932540eaf44a214";
constexpr char previous_release_gated_template_eboot_hash[] =
    "a74dab3d9798907d3c57ab91bcd442a8306603f1b4429ada52dd487e96262451";
constexpr char previous_retroarch_template_eboot_hash[] =
    "86bbcfb6419a09903704e7e2b092f66147699912d423dc26442674eb9bb7c592";
constexpr char previous_v3_template_eboot_hash[] = "1a0dba583bf2f937f5d063818b57be486bfade9b8a674a0e1002f0915213df65";
constexpr char previous_console_tile_forwarder_runtime[] =
    "isshelf-cooperative-forwarder/2";
constexpr char previous_template_eboot_hash[] = "3c04873fd7ea598f1ccf8a14129c729c60e789be304d49e74d8cfb059060c051";
constexpr char older_cooperative_eboot_hash[] = "052d162ddb7ed754b6475e31311dc219ab30bdd84aec7b0b3699860d6f47322b";
constexpr char legacy_template_eboot_hash[] = "3944ed19d258d348be0dda445959740919be4f727d6e6766e783b8a01c298331";
constexpr char template_libc_hash[] = "e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036";
constexpr char template_param_hash[] = "b70d9efe0c435f2c0e0497e7753de30dff205d7786f9ce47eb7664d3cb0835c3";
constexpr char template_license_hash[] = "230184f60bae2feaf244f10a8bac053c8ff33a183bcc365b4d8b876d2b7f4809";
constexpr char bundle_catalog_hash[] = "6caa93918ed8667ac9a6f0394c8385f869a39bbc55b47daee416b40b271d07c7";
struct PinnedAsset { const char* relative; const char* digest; std::size_t limit; };
constexpr PinnedAsset pinned_assets[] = {
    {"template/eboot.bin", template_eboot_hash, 512 * 1024},
    {"template/sce_module/libc.prx", template_libc_hash, 2 * 1024 * 1024},
    {"template/param.json", template_param_hash, 4096},
    {"LICENSE-forwarder.txt", template_license_hash, 128 * 1024},
    {"catalog.json", bundle_catalog_hash, 32768},
};

// This app's PS5 runtime intentionally omits libc++ basic_string. Keep every
// path, JSON value and HTTP reply bounded while using only libc allocation.
class Text {
    static constexpr std::size_t maximum = 32768;
    char* bytes_ = nullptr;
    std::size_t size_ = 0, capacity_ = 0;
    bool okay_ = true;
    bool reserve(std::size_t required) {
        if (!okay_ || required > maximum) { okay_ = false; return false; }
        if (required + 1 <= capacity_) return true;
        std::size_t next = capacity_ ? capacity_ * 2 : 64;
        if (next < required + 1) next = required + 1;
        if (next > maximum + 1) next = maximum + 1;
        void* allocated = std::realloc(bytes_, next);
        if (!allocated) { okay_ = false; return false; }
        bytes_ = static_cast<char*>(allocated);
        capacity_ = next;
        return true;
    }
  public:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    Text() = default;
    Text(const char* value) { if (value) append(value, std::strlen(value)); }
    Text(const char* value, std::size_t size) { append(value, size); }
    Text(std::string_view value) { append(value.data(), value.size()); }
    Text(const Text& value) { if (value.good()) append(value.data(), value.size()); else okay_ = false; }
    Text(Text&& value) noexcept
        : bytes_(value.bytes_), size_(value.size_), capacity_(value.capacity_), okay_(value.okay_) {
        value.bytes_ = nullptr; value.size_ = value.capacity_ = 0; value.okay_ = true;
    }
    ~Text() { std::free(bytes_); }
    Text& operator=(const Text& value) {
        if (this != &value) { clear(); if (value.good()) append(value.data(), value.size()); else okay_ = false; }
        return *this;
    }
    Text& operator=(Text&& value) noexcept {
        if (this != &value) {
            std::free(bytes_);
            bytes_ = value.bytes_; size_ = value.size_; capacity_ = value.capacity_; okay_ = value.okay_;
            value.bytes_ = nullptr; value.size_ = value.capacity_ = 0; value.okay_ = true;
        }
        return *this;
    }
    Text& operator=(const char* value) { clear(); if (value) append(value, std::strlen(value)); return *this; }
    bool good() const { return okay_; }
    const char* c_str() const { return okay_ && bytes_ ? bytes_ : ""; }
    const char* data() const { return c_str(); }
    std::size_t size() const { return size_; }
    bool empty() const { return okay_ && size_ == 0; }
    char back() const { return size_ && okay_ ? bytes_[size_ - 1] : 0; }
    void clear() { size_ = 0; okay_ = true; if (bytes_) bytes_[0] = 0; }
    void append(const char* value, std::size_t size) {
        if (!value && size) { okay_ = false; return; }
        if (size > maximum - size_ || !reserve(size_ + size)) return;
        if (size) std::memcpy(bytes_ + size_, value, size);
        size_ += size;
        bytes_[size_] = 0;
    }
    void assign(const char* value) { *this = value; }
    void push_back(char value) { append(&value, 1); }
    Text& operator+=(std::string_view value) { append(value.data(), value.size()); return *this; }
    Text& operator+=(const Text& value) {
        if (value.good()) append(value.data(), value.size()); else okay_ = false;
        return *this;
    }
    Text& operator+=(const char* value) { if (value) append(value, std::strlen(value)); return *this; }
    std::size_t find(std::string_view value) const {
        return good() ? std::string_view(data(), size()).find(value) : npos;
    }
    Text substr(std::size_t start, std::size_t count = npos) const {
        if (!good() || start > size_) { Text invalid; invalid.okay_ = false; return invalid; }
        const auto available = size_ - start;
        return Text(data() + start, count < available ? count : available);
    }
    operator std::string_view() const { return good() ? std::string_view(data(), size()) : std::string_view(); }
    friend Text operator+(Text left, std::string_view right) { left += right; return left; }
    friend Text operator+(const char* left, const Text& right) { Text result(left); result += right; return result; }
};

enum class PathKind { missing, file, directory, other };
enum class ApiInfo { missing, registered, pending, conflict, failed };
struct PathProbe { PathKind kind = PathKind::other; int os_error = 0; };
struct ApiProbe {
    bool called = false;
    bool diagnostic_only = false;
    int http_status = -1;
    int api_status = -1;
    ApiInfo result = ApiInfo::failed;
};

// Streaming C++17 adaptation of the pinned native-runtime SHA-256 code:
// native/third_party/ps5-runtime/tooling/native/hash.hpp (BlackBearReloaded,
// GPL-3.0-or-later). Keep the implementation local to this integrity boundary.
class Sha256 {
    std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<unsigned char, 64> block_{};
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;
    static std::uint32_t rotate(std::uint32_t value, unsigned bits) {
        return (value >> bits) | (value << (32 - bits));
    }
    void transform() {
        constexpr std::array<std::uint32_t, 64> constants{
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
            0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
            0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
            0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
            0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
            0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
            0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
            0xc67178f2U};
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto* at = block_.data() + i * 4;
            words[i] = (static_cast<std::uint32_t>(at[0]) << 24) |
                       (static_cast<std::uint32_t>(at[1]) << 16) |
                       (static_cast<std::uint32_t>(at[2]) << 8) | at[3];
        }
        for (std::size_t i = 16; i < words.size(); ++i) {
            const auto a = rotate(words[i - 15], 7) ^ rotate(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const auto b = rotate(words[i - 2], 17) ^ rotate(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + a + words[i - 7] + b;
        }
        auto a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        auto e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto t1 = h + s1 + choose + constants[i] + words[i];
            const auto s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + majority;
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }
  public:
    void update(const unsigned char* bytes, std::size_t size) {
        total_ += size;
        for (std::size_t i = 0; i < size; ++i) {
            block_[used_++] = bytes[i];
            if (used_ == block_.size()) { transform(); used_ = 0; }
        }
    }
    std::array<unsigned char, 32> finish() {
        const std::uint64_t bits = total_ * 8;
        unsigned char padding[128]{};
        padding[0] = 0x80;
        update(padding, used_ < 56 ? 56 - used_ : 120 - used_);
        unsigned char length[8];
        for (int i = 0; i < 8; ++i) length[i] = static_cast<unsigned char>(bits >> (56 - i * 8));
        update(length, sizeof(length));
        std::array<unsigned char, 32> result{};
        for (std::size_t i = 0; i < state_.size(); ++i)
            for (int offset = 0; offset < 4; ++offset)
                result[i * 4 + offset] = static_cast<unsigned char>(state_[i] >> (24 - offset * 8));
        return result;
    }
};

Text join(const char* root, std::string_view child) {
    Text path(root ? root : "");
    if (!path.empty() && path.back() != '/') path.push_back('/');
    path.append(child.data(), child.size());
    return path;
}
PathProbe path_probe(const Text& path) {
    if (!path.good() || path.empty()) return {PathKind::other, EINVAL};
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesA(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        return {error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                    ? PathKind::missing : PathKind::other, static_cast<int>(error)};
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return {PathKind::other, 0};
    return {attributes & FILE_ATTRIBUTE_DIRECTORY ? PathKind::directory : PathKind::file, 0};
#else
    // This PS5 permits stat/open/fstat on these exact paths while absolute
    // lstat returns EPERM, including for valid files and directories. Bind a
    // no-follow descriptor to the stat result instead: stable symlinks fail
    // at open, missing names retain ENOENT, and replacement races fail the
    // held-identity comparison.
    struct stat named{};
    if (stat(path.c_str(), &named) != 0) {
        const int error = errno;
        return {error == ENOENT ? PathKind::missing : PathKind::other, error};
    }
    if (!S_ISDIR(named.st_mode) && !S_ISREG(named.st_mode)) return {PathKind::other, 0};
    int flags = O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC;
    if (S_ISDIR(named.st_mode)) flags |= O_DIRECTORY;
    const int fd = open(path.c_str(), flags);
    if (fd < 0) return {PathKind::other, errno};
    struct stat held{};
    if (fstat(fd, &held) != 0) {
        const int error = errno;
        close(fd);
        return {PathKind::other, error};
    }
    const bool same = named.st_dev == held.st_dev && named.st_ino == held.st_ino &&
                      named.st_uid == held.st_uid && named.st_gid == held.st_gid &&
                      named.st_mode == held.st_mode &&
                      ((S_ISDIR(named.st_mode) && S_ISDIR(held.st_mode)) ||
                       (S_ISREG(named.st_mode) && S_ISREG(held.st_mode)));
    if (close(fd) != 0) return {PathKind::other, errno};
    if (!same) return {PathKind::other, ESTALE};
    return {S_ISDIR(named.st_mode) ? PathKind::directory : PathKind::file, 0};
#endif
}
PathKind path_kind(const Text& path) { return path_probe(path).kind; }
bool directory_mode(const Text& path, unsigned mode) {
    if (path_kind(path) != PathKind::directory) return false;
#ifdef _WIN32
    (void)mode;
    return true;
#else
    struct stat info{};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
           (info.st_mode & 0777) == static_cast<mode_t>(mode);
#endif
}
bool set_directory_mode(const Text& path, unsigned mode) {
    if (path_kind(path) != PathKind::directory) return false;
#ifdef _WIN32
    (void)mode;
    return true;
#else
    return chmod(path.c_str(), static_cast<mode_t>(mode)) == 0 && directory_mode(path, mode);
#endif
}
bool mkdir_exact(const Text& path, unsigned mode = 0777) {
#ifdef _WIN32
    (void)mode;
    return _mkdir(path.c_str()) == 0;
#else
    if (mkdir(path.c_str(), static_cast<mode_t>(mode)) != 0) return false;
    if (set_directory_mode(path, mode)) return true;
    rmdir(path.c_str());
    return false;
#endif
}
bool remove_dir(const Text& path) {
#ifdef _WIN32
    return _rmdir(path.c_str()) == 0;
#else
    return rmdir(path.c_str()) == 0;
#endif
}
bool sync_file(FILE* file) {
    if (!file || std::fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}
bool set_file_mode(const Text& path, unsigned mode) {
    if (path_kind(path) != PathKind::file) return false;
#ifdef _WIN32
    (void)mode;
    return true;
#else
    if (chmod(path.c_str(), static_cast<mode_t>(mode)) != 0) return false;
    struct stat info{};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
           (info.st_mode & 0777) == static_cast<mode_t>(mode);
#endif
}
bool write_file(const Text& path, std::string_view contents) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    const bool okay = std::fwrite(contents.data(), 1, contents.size(), file) == contents.size() && sync_file(file);
    const bool closed = std::fclose(file) == 0;
    return okay && closed;
}
FILE* open_regular_file(const Text& path, bool bundle_source) {
    if (!path.good() || path.empty() || (!bundle_source && path_kind(path) != PathKind::file)) return nullptr;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return nullptr;
    if (bundle_source) {
        // /app0 can be a virtual mount whose lstat result does not describe
        // files that fopen can read. Verify the opened descriptor instead.
#ifdef _WIN32
        struct _stat info{};
        const bool regular = _fstat(_fileno(file), &info) == 0 && (info.st_mode & _S_IFMT) == _S_IFREG;
#else
        struct stat info{};
        const bool regular = fstat(fileno(file), &info) == 0 && S_ISREG(info.st_mode);
#endif
        if (!regular) { std::fclose(file); return nullptr; }
    }
    return file;
}
bool bundle_file_readable(const Text& path) {
    FILE* file = open_regular_file(path, true);
    return file && std::fclose(file) == 0;
}
bool read_file(const Text& path, Text& contents, std::size_t limit, bool bundle_source = false) {
    FILE* file = open_regular_file(path, bundle_source);
    if (!file) return false;
    contents.clear();
    char buffer[4096];
    bool okay = true;
    while (okay) {
        const std::size_t used = std::fread(buffer, 1, sizeof(buffer), file);
        if (used > limit - contents.size()) { okay = false; break; }
        contents.append(buffer, used);
        if (!contents.good()) { okay = false; break; }
        if (used < sizeof(buffer)) { okay = !std::ferror(file); break; }
    }
    const bool closed = std::fclose(file) == 0;
    return okay && closed;
}
#ifndef SHELF_PS5
bool small_files_equal(const Text& left, const Text& right, std::size_t limit) {
    Text left_bytes, right_bytes;
    return limit <= 8192 &&
           read_file(left, left_bytes, limit) &&
           read_file(right, right_bytes, limit) &&
           left_bytes.size() == right_bytes.size() &&
           left_bytes.size() > 0 &&
           std::memcmp(left_bytes.data(), right_bytes.data(), left_bytes.size()) == 0;
}
#endif
bool sha256_matches(Sha256& sha, const char* expected) {
    if (!expected || std::strlen(expected) != 64) return false;
    constexpr char hex[] = "0123456789abcdef";
    char actual[65];
    const auto digest = sha.finish();
    for (std::size_t i = 0; i < digest.size(); ++i) {
        actual[i * 2] = hex[digest[i] >> 4];
        actual[i * 2 + 1] = hex[digest[i] & 15];
    }
    actual[64] = 0;
    return std::strcmp(actual, expected) == 0;
}
bool sha256_text(const Text& source, const char* expected) {
    if (!source.good() || source.empty()) return false;
    Sha256 sha;
    sha.update(reinterpret_cast<const unsigned char*>(source.data()), source.size());
    return sha256_matches(sha, expected);
}
bool verified_bundle_text(const ConsoleTileConfig& config, const Text& source, const char* expected) {
#ifndef SHELF_PS5
    if (config.test_skip_integrity) return true;
#else
    (void)config;
#endif
    return sha256_text(source, expected);
}
bool sha256_file(const Text& path, const char* expected, std::size_t limit,
                 std::size_t* observed_size = nullptr, bool bundle_source = false) {
    if (!expected || std::strlen(expected) != 64) return false;
    FILE* file = open_regular_file(path, bundle_source);
    if (!file) return false;
    Sha256 sha;
    unsigned char buffer[8192];
    std::size_t total = 0;
    bool okay = true;
    while (okay) {
        const auto used = std::fread(buffer, 1, sizeof(buffer), file);
        if (used > limit - total) { okay = false; break; }
        sha.update(buffer, used);
        total += used;
        if (used < sizeof(buffer)) { okay = !std::ferror(file); break; }
    }
    const bool closed = std::fclose(file) == 0;
    okay = okay && closed;
    if (!okay || !total) return false;
    if (observed_size) *observed_size = total;
    return sha256_matches(sha, expected);
}
bool pinned_bundle(const ConsoleTileConfig& config, std::size_t system, Text* icon_digest = nullptr) {
    if (!config.bundle_root) return false;
#ifndef SHELF_PS5
    const bool skip_integrity = config.test_skip_integrity;
#else
    constexpr bool skip_integrity = false;
#endif
    for (const auto& asset : pinned_assets) {
        const Text path = join(config.bundle_root, asset.relative);
        if (!bundle_file_readable(path) ||
            (!skip_integrity && !sha256_file(path, asset.digest, asset.limit, nullptr, true)))
            return false;
    }
    if (system >= system_count) return true;
    const Text id(catalog[system].id);
    const Text icon_path = join(config.bundle_root, "icons/" + id + ".png");
    if (skip_integrity) return bundle_file_readable(icon_path);
    Text source;
    if (!read_file(join(config.bundle_root, "catalog.json"), source, 32768, true) ||
        !verified_bundle_text(config, source, bundle_catalog_hash)) return false;
    JsonDocument doc;
    if (!doc.parse(source.data(), source.size()) || !cJSON_IsObject(doc.root) ||
        std::strcmp(json_string(doc.root, "format"), "isshelf-console-tile-assets/1") != 0 ||
        std::strcmp(json_string(doc.root, "forwarderSource"), forwarder_source) != 0 ||
        std::strcmp(json_string(doc.root, "forwarderCommit"), forwarder_commit) != 0 ||
        std::strcmp(json_string(doc.root, "runtime"),
                    console_tile_forwarder_runtime) != 0)
        return false;
    const auto* systems = cJSON_GetObjectItemCaseSensitive(doc.root, "systems");
    if (!cJSON_IsArray(systems) || cJSON_GetArraySize(systems) != static_cast<int>(system_count)) return false;
    const auto* entry = cJSON_GetArrayItem(systems, static_cast<int>(system));
    const char* hash = json_string(entry, "iconSha256");
    const auto* bytes = cJSON_GetObjectItemCaseSensitive(entry, "iconBytes");
    std::size_t observed = 0;
    if (std::strcmp(json_string(entry, "id"), id.c_str()) != 0 || !cJSON_IsNumber(bytes) ||
        bytes->valuedouble < 1 || bytes->valuedouble > 2 * 1024 * 1024 ||
        !sha256_file(icon_path, hash, 2 * 1024 * 1024, &observed, true) ||
        observed != static_cast<std::size_t>(bytes->valuedouble))
        return false;
    if (icon_digest) *icon_digest = hash;
    return true;
}
bool resolve_pinned_bundle(const ConsoleTileConfig& config, std::size_t system,
                           Text& selected_root, Text* icon_digest = nullptr) {
    const char* roots[] = {config.bundle_root, config.bundle_fallback_root};
    for (std::size_t i = 0; i < 2; ++i) {
        const char* root = roots[i];
        if (!root || !root[0] ||
            (i == 1 && config.bundle_root && std::strcmp(root, config.bundle_root) == 0))
            continue;
        ConsoleTileConfig candidate = config;
        candidate.bundle_root = root;
        if (pinned_bundle(candidate, system, icon_digest)) {
            selected_root = root;
            return selected_root.good();
        }
    }
    return false;
}
bool copy_file(const Text& source, const Text& target, std::size_t limit, bool bundle_source = false) {
    if (path_kind(target) != PathKind::missing) return false;
    FILE* input = open_regular_file(source, bundle_source);
    if (!input) return false;
    FILE* output = std::fopen(target.c_str(), "wb");
    if (!output) { std::fclose(input); return false; }
    char buffer[8192];
    std::size_t total = 0;
    bool okay = true;
    while (okay) {
        const std::size_t used = std::fread(buffer, 1, sizeof(buffer), input);
        if (used > limit - total || std::fwrite(buffer, 1, used, output) != used) { okay = false; break; }
        total += used;
        if (used < sizeof(buffer)) { okay = !std::ferror(input); break; }
    }
    okay = okay && total > 0 && sync_file(output);
    const bool in_closed = std::fclose(input) == 0;
    const bool out_closed = std::fclose(output) == 0;
    return okay && in_closed && out_closed;
}
bool files_equal(const Text& source, const Text& target, std::size_t limit) {
    FILE* left = open_regular_file(source, true);
    FILE* right = open_regular_file(target, false);
    if (!left || !right) {
        if (left) std::fclose(left);
        if (right) std::fclose(right);
        return false;
    }
    char lhs[8192], rhs[8192];
    std::size_t total = 0;
    bool okay = true;
    while (okay) {
        const std::size_t left_used = std::fread(lhs, 1, sizeof(lhs), left);
        const std::size_t right_used = std::fread(rhs, 1, sizeof(rhs), right);
        if (left_used != right_used || left_used > limit - total ||
            (left_used && std::memcmp(lhs, rhs, left_used) != 0)) {
            okay = false;
            break;
        }
        total += left_used;
        if (left_used < sizeof(lhs)) {
            okay = !std::ferror(left) && !std::ferror(right);
            break;
        }
    }
    const bool left_closed = std::fclose(left) == 0;
    const bool right_closed = std::fclose(right) == 0;
    return okay && total > 0 && left_closed && right_closed;
}
bool make_executable(const Text& path) {
    // Matches a working homebrew tile on the owner's PS5, including the
    // runtime module. Host fixtures have no meaningful POSIX execute bit.
    return set_file_mode(path, 0777);
}
bool parse_json(const Text& source, JsonDocument& result) {
    return source.size() <= 8192 && result.parse(source.data(), source.size()) && cJSON_IsObject(result.root);
}
void set_message(ConsoleTileOutcome& outcome, const char* message) {
    std::snprintf(outcome.message, sizeof(outcome.message), "%s", message);
}
ConsoleTileOutcome outcome_for(std::size_t system, ConsoleTileStage stage, const char* message) {
    ConsoleTileOutcome result;
    result.stage = stage;
    result.intent.system = system;
    set_message(result, message);
    return result;
}
bool title_id_for(unsigned number, char (&output)[10]) {
    return number >= preferred_first && number <= last_number && number != reserved_number &&
           std::snprintf(output, sizeof(output), "PPSA%05u", number) == 9;
}
bool append_handoff_title(char* list, std::size_t capacity, const char* title) {
    if (!list || !capacity || !title || std::strlen(title) != 9)
        return false;
    std::string_view remaining(list);
    while (!remaining.empty()) {
        const auto comma = remaining.find(',');
        const auto length = comma == remaining.npos ? remaining.size() : comma;
        if (remaining.substr(0, length) == title)
            return true;
        if (comma == remaining.npos)
            break;
        remaining.remove_prefix(comma + 1);
    }
    const auto used = std::strlen(list);
    const auto separator = used ? 1u : 0u;
    if (used + separator + 9 >= capacity)
        return false;
    if (separator)
        list[used] = ',';
    std::memcpy(list + used + separator, title, 10);
    return true;
}
bool candidate_number(unsigned number) {
    return (number >= preferred_first && number < preferred_first + system_count) ||
           (number >= alternate_first && number <= last_number && number != reserved_number);
}
enum class MarkerOwnership { absent, other_system, owned, ambiguous };
MarkerOwnership marker_ownership(const Text& directory, const char* title_id, std::size_t system) {
    const auto marker = path_probe(join(directory.c_str(), "isshelf-console-tile.json"));
    if (marker.kind == PathKind::missing) return MarkerOwnership::absent;
    if (marker.kind != PathKind::file) return MarkerOwnership::ambiguous;
    Text source;
    if (!read_file(join(directory.c_str(), "isshelf-console-tile.json"), source, 2048))
        return MarkerOwnership::ambiguous;
    JsonDocument doc;
    if (!parse_json(source, doc) ||
        std::strcmp(json_string(doc.root, "format"), owner_format) != 0 ||
        std::strcmp(json_string(doc.root, "titleId"), title_id) != 0 ||
        std::strcmp(json_string(doc.root, "target"), shelf_title_id) != 0 ||
        std::strcmp(json_string(doc.root, "forwarderSource"), forwarder_source) != 0 ||
        std::strcmp(json_string(doc.root, "forwarderCommit"), forwarder_commit) != 0)
        return MarkerOwnership::ambiguous;
    const char* runtime = json_string(doc.root, "runtime");
    // A predecessor marker is still an isShelf ownership proof.  It must be
    // accepted here so the explicit update path can verify its pinned v2
    // executable and atomically migrate both the program and marker to v3.
    // Unknown runtimes remain ambiguous and fail closed.
    if (runtime[0] &&
        std::strcmp(runtime, console_tile_forwarder_runtime) != 0 &&
        std::strcmp(runtime, previous_console_tile_forwarder_runtime) != 0)
        return MarkerOwnership::ambiguous;
    const char* owner_system = json_string(doc.root, "system");
    for (std::size_t index = 0; index < system_count; ++index) {
        if (std::string_view(owner_system) == catalog[index].id)
            return index == system ? MarkerOwnership::owned : MarkerOwnership::other_system;
    }
    return MarkerOwnership::ambiguous;
}
bool owned_marker(const Text& directory, const char* title_id, std::size_t system) {
    return marker_ownership(directory, title_id, system) == MarkerOwnership::owned;
}
bool forwarder_intent(const Text& directory, std::size_t system, ConsoleTileIntent& intent) {
    Text source;
    if (!read_file(join(directory.c_str(), "forwarder.json"), source, 2048)) return false;
    JsonDocument doc;
    if (!parse_json(source, doc) || std::strcmp(json_string(doc.root, "format"), forwarder_format) != 0 ||
        std::strcmp(json_string(doc.root, "target"), shelf_title_id) != 0) return false;
    const auto* args = cJSON_GetObjectItemCaseSensitive(doc.root, "args");
    if (!cJSON_IsArray(args) || cJSON_GetArraySize(args) != 1) return false;
    const auto* arg = cJSON_GetArrayItem(args, 0);
    if (!cJSON_IsString(arg) || !arg->valuestring || !parse_console_tile_arg(arg->valuestring, intent)) return false;
    return intent.system == system;
}
bool param_file_identity(const Text& path, const char* title_id) {
    Text source;
    if (!read_file(path, source, 4096)) return false;
    JsonDocument doc;
    if (!parse_json(source, doc)) return false;
    char content_id[80];
    std::snprintf(content_id, sizeof(content_id), "UP9000-%s_00-PS5FORWARDER0000", title_id);
    return std::strcmp(json_string(doc.root, "titleId"), title_id) == 0 &&
           std::strcmp(json_string(doc.root, "contentId"), content_id) == 0;
}
bool param_identity(const Text& directory, const char* title_id) {
    return param_file_identity(join(directory.c_str(), "sce_sys/param.json"), title_id);
}
const char* required_folder_layout_issue(const Text& directory) {
    if (path_kind(directory) != PathKind::directory) return "layout-root";
    if (path_kind(join(directory.c_str(), "sce_module")) != PathKind::directory)
        return "layout-module";
    if (path_kind(join(directory.c_str(), "sce_sys")) != PathKind::directory)
        return "layout-sce-sys";
    struct RequiredFile { const char* path; const char* issue; };
    const RequiredFile files[] = {
        {"eboot.bin", "layout-eboot"}, {"sce_module/libc.prx", "layout-libc"},
        {"forwarder.json", "layout-forwarder"}, {"LICENSE-forwarder.txt", "layout-license"},
        {"sce_sys/param.json", "layout-param"}, {"sce_sys/icon0.png", "layout-icon"},
        {"isshelf-console-tile.json", "layout-owner"},
    };
    for (const auto& file : files)
        if (path_kind(join(directory.c_str(), file.path)) != PathKind::file) return file.issue;
    return nullptr;
}
const char* owned_folder_issue(const Text& directory, const char* title_id, std::size_t system,
                               ConsoleTileIntent& intent) {
    // Ownership requires every expected no-follow path and exact identities.
    // Extra entries are preserved when the whole folder is isolated. Cleanup
    // removes only known owned paths; a remaining entry prevents rmdir.
    if (const char* issue = required_folder_layout_issue(directory)) return issue;
    if (!owned_marker(directory, title_id, system)) return "owned-marker";
    if (!param_identity(directory, title_id)) return "owned-param";
    if (!forwarder_intent(directory, system, intent)) return "owned-forwarder";
    return nullptr;
}
bool owned_folder(const Text& directory, const char* title_id, std::size_t system,
                  ConsoleTileIntent& intent) {
    return owned_folder_issue(directory, title_id, system, intent) == nullptr;
}
int loopback_post(unsigned short port, const char* route, const char* body, char* response, std::size_t capacity) {
    if (!route || !body || !response || capacity < 2 || std::strlen(body) > 4096) return -1;
#ifdef _WIN32
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return -1;
    using Socket = SOCKET;
    constexpr Socket invalid = INVALID_SOCKET;
#else
    using Socket = int;
    constexpr Socket invalid = -1;
#endif
    const Socket fd = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (fd == invalid) { WSACleanup(); return -1; }
    const DWORD timeout = 3000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    if (fd == invalid) return -1;
    timeval timeout{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    sockaddr_in address{};
#ifdef SHELF_PS5
    address.sin_len = sizeof(address);
#endif
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int status = -1;
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        char header[256];
        const int header_size = std::snprintf(header, sizeof(header),
                                             "POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                                             "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                                             route, std::strlen(body));
        if (header_size > 0 && static_cast<std::size_t>(header_size) < sizeof(header)) {
            Text request(header, static_cast<std::size_t>(header_size));
            request += body;
            std::size_t sent = 0;
            while (request.good() && sent < request.size()) {
#ifdef MSG_NOSIGNAL
                constexpr int send_flags = MSG_NOSIGNAL;
#else
                constexpr int send_flags = 0;
#endif
                const int used = static_cast<int>(send(fd, request.data() + sent,
                                                       static_cast<int>(request.size() - sent), send_flags));
                if (used <= 0) break;
                sent += static_cast<std::size_t>(used);
            }
            if (request.good() && sent == request.size()) {
                Text raw;
                char buffer[4096];
                while (raw.size() <= capacity + 4096) {
                    const int used = static_cast<int>(recv(fd, buffer, sizeof(buffer), 0));
                    if (used < 0) { raw.clear(); break; }
                    if (used == 0) break;
                    raw.append(buffer, static_cast<std::size_t>(used));
                    if (!raw.good()) { raw.clear(); break; }
                }
                const auto divider = raw.find("\r\n\r\n");
                const auto end = raw.find("\r\n");
                if (divider != Text::npos && end != Text::npos && divider + 4 <= raw.size()) {
                    int code = 0;
                    if (std::sscanf(raw.substr(0, end).c_str(), "HTTP/%*u.%*u %d", &code) == 1 &&
                        raw.size() - divider - 4 < capacity) {
                        const auto size = raw.size() - divider - 4;
                        std::memcpy(response, raw.data() + divider + 4, size);
                        response[size] = 0;
                        status = code;
                    }
                }
            }
        }
    }
#ifdef _WIN32
    closesocket(fd);
    WSACleanup();
#else
    close(fd);
#endif
    return status;
}
int post(const ConsoleTileConfig& config, const char* route, const char* request,
         char* response, std::size_t capacity) {
    response[0] = 0;
    if (config.post) return config.post(config.post_context, route, request, response, capacity);
    return loopback_post(config.api_port, route, request, response, capacity);
}
ApiInfo api_info(const ConsoleTileConfig& config, const char* title_id, const Text& expected_path,
                 ApiProbe* evidence = nullptr) {
    if (evidence) { *evidence = {}; evidence->called = true; }
    char request[64], reply[8192];
    std::snprintf(request, sizeof(request), "{\"title_id\":\"%s\"}", title_id);
    const int code = post(config, "/api/v1/games/info", request, reply, sizeof(reply));
    if (evidence) evidence->http_status = code;
    auto finish = [evidence](ApiInfo result) {
        if (evidence) evidence->result = result;
        return result;
    };
    if (code == 404) {
        JsonDocument missing;
        if (!missing.parse(reply, std::strlen(reply)) || !cJSON_IsObject(missing.root))
            return finish(ApiInfo::failed);
        const auto* status = cJSON_GetObjectItemCaseSensitive(missing.root, "status");
        if (cJSON_IsNumber(status) && evidence) evidence->api_status = status->valueint;
        // ShadowMount reports an unknown title as HTTP 404 with errno ENOENT.
        // A different or malformed 404 is not evidence that the ID is free.
        return finish(cJSON_IsNumber(status) && status->valueint == ENOENT
                          ? ApiInfo::missing : ApiInfo::failed);
    }
    if (code != 200) return finish(ApiInfo::failed);
    JsonDocument doc;
    if (!doc.parse(reply, std::strlen(reply)) || !cJSON_IsObject(doc.root)) return finish(ApiInfo::failed);
    const auto* status = cJSON_GetObjectItemCaseSensitive(doc.root, "status");
    if (cJSON_IsNumber(status) && evidence) evidence->api_status = status->valueint;
    if (!cJSON_IsNumber(status) || status->valueint != 0) return finish(ApiInfo::failed);
    if (std::strcmp(json_string(doc.root, "title_id"), title_id) != 0) return finish(ApiInfo::failed);
    if (!expected_path.empty() && std::strcmp(json_string(doc.root, "path"), expected_path.c_str()) != 0)
        return finish(ApiInfo::conflict);
    const auto* installed = cJSON_GetObjectItemCaseSensitive(doc.root, "installed");
    if (!cJSON_IsTrue(installed) && !cJSON_IsFalse(installed)) return finish(ApiInfo::failed);
    if (cJSON_IsTrue(installed)) return finish(ApiInfo::registered);
    const auto* managed = cJSON_GetObjectItemCaseSensitive(doc.root, "managed");
    const auto* can_uninstall = cJSON_GetObjectItemCaseSensitive(doc.root, "can_uninstall");
    const auto* source_available = cJSON_GetObjectItemCaseSensitive(doc.root, "source_available");
    const auto exact_bool = [](const cJSON* value) {
        return cJSON_IsTrue(value) || cJSON_IsFalse(value);
    };
    if (!exact_bool(managed) || !exact_bool(can_uninstall) || !exact_bool(source_available))
        return finish(ApiInfo::failed);
    // A successful uninstall can remain addressable as an HTTP 200 tombstone.
    // It is absence evidence only for the removal path, after isShelf has
    // isolated its active source, and only when every lifecycle field is an
    // explicit negative. CREATE/inspection still treats the same title as
    // occupied until the ordinary 404/ENOENT record is observed.
    if (expected_path.empty() && cJSON_IsFalse(managed) && cJSON_IsFalse(can_uninstall) &&
        cJSON_IsFalse(source_available))
        return finish(ApiInfo::missing);
    return finish(ApiInfo::pending);
}
bool exact_json_number(const cJSON* item, int expected) {
    return cJSON_IsNumber(item) && item->valuedouble == static_cast<double>(expected);
}
bool bounded_api_path(const char* path) {
    if (!path || path[0] != '/') return false;
    const std::size_t length = std::strlen(path);
    if (length < 2 || length > 512 || path[length - 1] == '/') return false;
    for (std::size_t i = 0; i < length; ++i)
        if (static_cast<unsigned char>(path[i]) < 0x20) return false;
    return true;
}
bool title_basename(const char* path, const char* title_id) {
    const char* leaf = std::strrchr(path, '/');
    return leaf && std::strcmp(leaf + 1, title_id) == 0;
}
enum class ManualSourceState { absent, exact, conflict, failed };
bool bounded_manual_path(const char* path) {
#ifdef SHELF_PS5
    return bounded_api_path(path);
#else
    if (!path) return false;
    const std::size_t length = std::strlen(path);
    if (length < 2 || length > 512 || path[length - 1] == '/') return false;
    const bool rooted = path[0] == '/' ||
                        (length > 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
                         path[1] == ':' && path[2] == '/');
    if (!rooted) return false;
    for (std::size_t i = 0; i < length; ++i)
        if (static_cast<unsigned char>(path[i]) < 0x20 || path[i] == '"' || path[i] == '\\')
            return false;
    return true;
#endif
}
ManualSourceState manual_source_state(const ConsoleTileConfig& config, const char* title_id,
                                      const Text& expected_path) {
    if (!title_id || !bounded_manual_path(expected_path.c_str())) return ManualSourceState::failed;
    char reply[8192];
    if (post(config, "/api/v1/manual/list", "{}", reply, sizeof(reply)) != 200)
        return ManualSourceState::failed;
    JsonDocument doc;
    if (!doc.parse(reply, std::strlen(reply)) || !cJSON_IsObject(doc.root) ||
        !exact_json_number(cJSON_GetObjectItemCaseSensitive(doc.root, "status"), 0))
        return ManualSourceState::failed;
    const auto* paths = cJSON_GetObjectItemCaseSensitive(doc.root, "paths");
    if (!cJSON_IsArray(paths)) return ManualSourceState::failed;
    const int count = cJSON_GetArraySize(paths);
    if (count < 0 || count > 512 ||
        !exact_json_number(cJSON_GetObjectItemCaseSensitive(doc.root, "count"), count))
        return ManualSourceState::failed;
    std::array<const char*, 512> seen{};
    int used = 0, exact = 0, other_title = 0;
    for (const auto* item = paths->child; item; item = item->next) {
        if (used >= count || !cJSON_IsString(item) || !bounded_manual_path(item->valuestring))
            return ManualSourceState::failed;
        for (int earlier = 0; earlier < used; ++earlier)
            if (std::strcmp(item->valuestring, seen[static_cast<std::size_t>(earlier)]) == 0)
                return ManualSourceState::failed;
        seen[static_cast<std::size_t>(used++)] = item->valuestring;
        if (std::strcmp(item->valuestring, expected_path.c_str()) == 0)
            ++exact;
        else if (title_basename(item->valuestring, title_id))
            ++other_title;
    }
    if (used != count) return ManualSourceState::failed;
    if (exact > 1 || other_title) return ManualSourceState::conflict;
    return exact == 1 ? ManualSourceState::exact : ManualSourceState::absent;
}
bool json_path_safe(const Text& path) {
    if (!bounded_manual_path(path.c_str())) return false;
    for (const unsigned char* at = reinterpret_cast<const unsigned char*>(path.c_str()); *at; ++at) {
        bool safe = std::isalnum(*at) || *at == '/' || *at == '.' || *at == '_' || *at == '-';
#ifndef SHELF_PS5
        safe = safe || *at == ':';
#endif
        if (!safe) return false;
    }
    return true;
}
bool remove_manual_source(const ConsoleTileConfig& config, const char* title_id,
                          const Text& expected_path) {
    if (!json_path_safe(expected_path)) return false;
    char request[640], reply[2048];
    const int used = std::snprintf(request, sizeof(request), "{\"path\":\"%s\"}",
                                   expected_path.c_str());
    if (used <= 0 || static_cast<std::size_t>(used) >= sizeof(request) ||
        post(config, "/api/v1/manual/remove", request, reply, sizeof(reply)) != 200)
        return false;
    JsonDocument doc;
    if (!doc.parse(reply, std::strlen(reply)) || !cJSON_IsObject(doc.root) ||
        !exact_json_number(cJSON_GetObjectItemCaseSensitive(doc.root, "status"), 0) ||
        std::strcmp(json_string(doc.root, "path"), expected_path.c_str()) != 0 ||
        !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(doc.root, "present")) ||
        !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(doc.root, "changed")))
        return false;
    return manual_source_state(config, title_id, expected_path) == ManualSourceState::absent;
}
ConsoleTileStage queue_scan(const ConsoleTileConfig& config) {
    char reply[2048];
    if (post(config, "/api/v1/scan", "{}", reply, sizeof(reply)) != 200) return ConsoleTileStage::unavailable;
    JsonDocument doc;
    if (!doc.parse(reply, std::strlen(reply)) || !cJSON_IsObject(doc.root)) return ConsoleTileStage::failed;
    const auto* status = cJSON_GetObjectItemCaseSensitive(doc.root, "status");
    const auto* queued = cJSON_GetObjectItemCaseSensitive(doc.root, "scan_queued");
    const auto* deferred = cJSON_GetObjectItemCaseSensitive(doc.root, "scan_deferred");
    if (!cJSON_IsNumber(status) || status->valueint != 0 || !cJSON_IsTrue(queued)) return ConsoleTileStage::failed;
    return cJSON_IsTrue(deferred) ? ConsoleTileStage::deferred : ConsoleTileStage::pending;
}
bool make_forwarder_json(const ConsoleTileIntent& intent, Text& output) {
    char argument[console_tile_message_capacity + 1];
    if (!encode_console_tile_arg(intent, argument, sizeof(argument))) return false;
    output = "{\"format\":\"ps5-forwarder/1\",\"target\":\"PPSA99541\",\"args\":[\"";
    output += argument;
    output += "\"],\"creator\":\"isShelf\"}\n";
    return output.good() && output.size() < 512;
}
bool make_owner_json(const ConsoleTileIntent& intent, const char* title_id, Text& output) {
    const Text id(catalog[intent.system].id);
    output = "{\"format\":\"isshelf-console-tile/1\",\"system\":\"" + id +
             "\",\"titleId\":\"" + title_id + "\",\"target\":\"PPSA99541\",\"forwarderSource\":\"" +
             forwarder_source + "\",\"forwarderCommit\":\"" + forwarder_commit +
             "\",\"runtime\":\"" + console_tile_forwarder_runtime + "\"}\n";
    return output.good() && output.size() < 512;
}
bool make_param_json(const ConsoleTileConfig& config, const ConsoleTileIntent& intent,
                     const char* title_id, Text& output) {
    Text source;
    if (!read_file(join(config.bundle_root, "template/param.json"), source, 4096, true) ||
        !verified_bundle_text(config, source, template_param_hash)) return false;
    JsonDocument doc;
    if (!parse_json(source, doc) || std::strcmp(json_string(doc.root, "titleId"), "PPSA99100") != 0)
        return false;
    const auto* localized = cJSON_GetObjectItemCaseSensitive(doc.root, "localizedParameters");
    auto* english = cJSON_GetObjectItemCaseSensitive(localized, "en-US");
    if (!cJSON_IsObject(english)) return false;
    const Text name(catalog[intent.system].name);
    char content_id[80];
    std::snprintf(content_id, sizeof(content_id), "UP9000-%s_00-PS5FORWARDER0000", title_id);
    const bool okay =
        cJSON_ReplaceItemInObjectCaseSensitive(doc.root, "titleId", cJSON_CreateString(title_id)) &&
        cJSON_ReplaceItemInObjectCaseSensitive(doc.root, "conceptId", cJSON_CreateString(title_id + 4)) &&
        cJSON_ReplaceItemInObjectCaseSensitive(doc.root, "contentId", cJSON_CreateString(content_id)) &&
        cJSON_ReplaceItemInObjectCaseSensitive(english, "titleName", cJSON_CreateString(name.c_str()));
    if (!okay) return false;
    char* printed = cJSON_PrintUnformatted(doc.root);
    if (!printed) return false;
    output.assign(printed);
    cJSON_free(printed);
    output.push_back('\n');
    return output.good() && output.size() < 4096;
}
bool replace_file(const Text& target, std::string_view contents) {
    const auto temporary = target + ".isshelf-next";
    if (path_kind(temporary) != PathKind::missing || !write_file(temporary, contents)) return false;
    if (!set_file_mode(temporary, 0666)) { std::remove(temporary.c_str()); return false; }
#ifdef _WIN32
    const bool okay = MoveFileExA(temporary.c_str(), target.c_str(),
                                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    const bool okay = std::rename(temporary.c_str(), target.c_str()) == 0;
#endif
    if (!okay) std::remove(temporary.c_str());
    return okay;
}
bool current_owner_runtime(const Text& directory, const char* title_id,
                           std::size_t system) {
    Text source;
    if (!read_file(join(directory.c_str(), "isshelf-console-tile.json"),
                   source, 2048)) return false;
    JsonDocument doc;
    return parse_json(source, doc) &&
           std::strcmp(json_string(doc.root, "format"), owner_format) == 0 &&
           std::strcmp(json_string(doc.root, "titleId"), title_id) == 0 &&
           std::strcmp(json_string(doc.root, "target"), shelf_title_id) == 0 &&
           std::strcmp(json_string(doc.root, "forwarderSource"),
                       forwarder_source) == 0 &&
           std::strcmp(json_string(doc.root, "forwarderCommit"),
                       forwarder_commit) == 0 &&
           system < system_count &&
           std::string_view(json_string(doc.root, "system")) == catalog[system].id &&
           std::strcmp(json_string(doc.root, "runtime"),
                       console_tile_forwarder_runtime) == 0;
}
bool replace_bundle_binary(const Text& source, const Text& target,
                           std::size_t limit, const char* digest,
                           bool skip_integrity = false) {
    const auto temporary = target + ".isshelf-runtime-next";
    if (path_kind(temporary) != PathKind::missing ||
        !copy_file(source, temporary, limit, true)) return false;
    bool okay = (skip_integrity || sha256_file(temporary, digest, limit)) &&
                make_executable(temporary);
#ifdef _WIN32
    if (okay)
        okay = MoveFileExA(temporary.c_str(), target.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (okay) okay = std::rename(temporary.c_str(), target.c_str()) == 0;
#endif
    if (!okay) std::remove(temporary.c_str());
    return okay && (skip_integrity || sha256_file(target, digest, limit));
}
bool replace_bundle_asset(const Text& source, const Text& target,
                          std::size_t limit, const char* digest,
                          bool skip_integrity = false) {
    const auto temporary = target + ".isshelf-asset-next";
    if (path_kind(temporary) != PathKind::missing ||
        !copy_file(source, temporary, limit, true)) return false;
    bool okay = (skip_integrity || sha256_file(temporary, digest, limit)) &&
                set_file_mode(temporary, 0666);
#ifdef _WIN32
    if (okay)
        okay = MoveFileExA(temporary.c_str(), target.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (okay) okay = std::rename(temporary.c_str(), target.c_str()) == 0;
#endif
    if (!okay) std::remove(temporary.c_str());
    return okay && (skip_integrity ? files_equal(source, target, limit)
                                   : sha256_file(target, digest, limit));
}
bool upgrade_owned_runtime(const ConsoleTileConfig& config,
                           const ConsoleTileIntent& intent,
                           const char* title_id, const Text& directory,
                           bool& changed) {
    changed = false;
    if (!title_id || intent.system >= system_count) return false;
    Text verified_root;
    if (!resolve_pinned_bundle(config, intent.system, verified_root)) return false;
    const auto eboot = join(directory.c_str(), "eboot.bin");
    const auto libc = join(directory.c_str(), "sce_module/libc.prx");
#ifndef SHELF_PS5
    const bool skip_integrity = config.test_skip_integrity;
#else
    constexpr bool skip_integrity = false;
#endif
    if (!skip_integrity &&
        !sha256_file(libc, template_libc_hash, 2 * 1024 * 1024)) return false;
    const bool current = skip_integrity
        ? current_owner_runtime(directory, title_id, intent.system)
        : sha256_file(eboot, template_eboot_hash, 512 * 1024);
    const bool predecessor = skip_integrity ||
        sha256_file(eboot, previous_unmount_assisted_template_eboot_hash, 512 * 1024) ||
        sha256_file(eboot, previous_release_gated_template_eboot_hash, 512 * 1024) ||
        sha256_file(eboot, previous_retroarch_template_eboot_hash, 512 * 1024) ||
        sha256_file(eboot, previous_v3_template_eboot_hash, 512 * 1024) ||
        sha256_file(eboot, previous_template_eboot_hash, 512 * 1024) ||
        sha256_file(eboot, older_cooperative_eboot_hash, 512 * 1024) ||
        sha256_file(eboot, legacy_template_eboot_hash, 512 * 1024);
    if (!current && !predecessor) return false;
    if (!current) {
        if (!replace_bundle_binary(
                join(verified_root.c_str(), "template/eboot.bin"), eboot,
                512 * 1024, template_eboot_hash, skip_integrity)) return false;
        changed = true;
    }
    if (!current_owner_runtime(directory, title_id, intent.system)) {
        Text owner;
        if (!make_owner_json(intent, title_id, owner) ||
            !replace_file(join(directory.c_str(), "isshelf-console-tile.json"),
                          std::string_view(owner.data(), owner.size())))
            return false;
        changed = true;
    }
    return (skip_integrity ||
            sha256_file(eboot, template_eboot_hash, 512 * 1024)) &&
           (skip_integrity ||
            sha256_file(libc, template_libc_hash, 2 * 1024 * 1024)) &&
           current_owner_runtime(directory, title_id, intent.system);
}
bool refresh_owned_artwork(const ConsoleTileConfig& config,
                           const ConsoleTileIntent& intent,
                           const Text& directory, bool& changed) {
    changed = false;
    if (intent.system >= system_count) return false;
    Text verified_root, icon_digest;
    if (!resolve_pinned_bundle(config, intent.system, verified_root,
                               &icon_digest)) return false;
    const Text id(catalog[intent.system].id);
    const auto source = join(verified_root.c_str(), "icons/" + id + ".png");
    const auto target = join(directory.c_str(), "sce_sys/icon0.png");
#ifndef SHELF_PS5
    const bool skip_integrity = config.test_skip_integrity;
#else
    constexpr bool skip_integrity = false;
#endif
    const bool current = skip_integrity
        ? files_equal(source, target, 2 * 1024 * 1024)
        : icon_digest.good() && icon_digest.size() == 64 &&
              sha256_file(target, icon_digest.c_str(), 2 * 1024 * 1024);
    if (!current) {
        if (!replace_bundle_asset(source, target, 2 * 1024 * 1024,
                                  icon_digest.c_str(), skip_integrity)) return false;
        changed = true;
    }
    return skip_integrity
        ? files_equal(source, target, 2 * 1024 * 1024)
        : sha256_file(target, icon_digest.c_str(), 2 * 1024 * 1024);
}
bool registered_artwork_current(const ConsoleTileConfig& config,
                                const ConsoleTileIntent& intent,
                                const char* title_id) {
    if (!config.user_app_root || !config.user_appmeta_root || !title_id ||
        !title_id[0] || intent.system >= system_count) return false;
    Text verified_root, icon_digest;
    if (!resolve_pinned_bundle(config, intent.system, verified_root,
                               &icon_digest)) return false;
    const Text id(catalog[intent.system].id);
    const auto source = join(verified_root.c_str(), "icons/" + id + ".png");
    const auto app_icon = join(join(config.user_app_root, title_id).c_str(),
                               "sce_sys/icon0.png");
    const auto appmeta_icon = join(join(config.user_appmeta_root, title_id).c_str(),
                                   "icon0.png");
#ifndef SHELF_PS5
    if (config.test_skip_integrity)
        return files_equal(source, app_icon, 2 * 1024 * 1024) &&
               files_equal(source, appmeta_icon, 2 * 1024 * 1024);
#endif
    return icon_digest.good() && icon_digest.size() == 64 &&
           sha256_file(app_icon, icon_digest.c_str(), 2 * 1024 * 1024) &&
           sha256_file(appmeta_icon, icon_digest.c_str(), 2 * 1024 * 1024);
}
#ifndef SHELF_PS5
bool owned_registration_residue(const ConsoleTileConfig& config,
                                std::size_t system, const char* title_id) {
    if (!config.homebrew_root || !config.user_app_root ||
        !config.user_appmeta_root || !title_id || system >= system_count)
        return false;
    const auto active = join(config.homebrew_root, title_id);
    const auto app = join(config.user_app_root, title_id);
    const auto appmeta = join(config.user_appmeta_root, title_id);
    if (path_kind(active) != PathKind::missing ||
        path_kind(app) != PathKind::directory ||
        path_kind(appmeta) != PathKind::directory ||
        !param_identity(app, title_id) ||
        !param_file_identity(join(appmeta.c_str(), "param.json"), title_id) ||
        !small_files_equal(join(app.c_str(), "sce_sys/param.json"),
                           join(appmeta.c_str(), "param.json"), 4096))
        return false;
    ConsoleTileIntent intent;
    intent.system = system;
    if (!registered_artwork_current(config, intent, title_id))
        return false;
    Text verified_root, icon_digest;
    if (!resolve_pinned_bundle(config, system, verified_root, &icon_digest))
        return false;
    const Text id(catalog[system].id);
    const auto source_icon = join(verified_root.c_str(), "icons/" + id + ".png");
    const auto app_root_icon = join(app.c_str(), "icon0.png");
#ifndef SHELF_PS5
    if (config.test_skip_integrity) {
        if (!files_equal(source_icon, app_root_icon, 2 * 1024 * 1024))
            return false;
    } else
#endif
    if (!icon_digest.good() || icon_digest.size() != 64 ||
        !sha256_file(app_root_icon, icon_digest.c_str(), 2 * 1024 * 1024))
        return false;
    const auto info = api_info(config, title_id, {});
    return (info == ApiInfo::missing || info == ApiInfo::registered ||
            info == ApiInfo::pending) &&
           manual_source_state(config, title_id, active) == ManualSourceState::absent;
}
#endif
bool cleanup_folder(const Text& directory) {
    // Exact files from this writer only. An unexpected extra keeps the folder
    // in place; we never recursively delete a folder we did not fully inspect.
    if (path_kind(directory) != PathKind::directory) return false;
    for (const char* name : {"sce_module", "sce_sys"}) {
        const auto kind = path_kind(join(directory.c_str(), name));
        if (kind != PathKind::missing && kind != PathKind::directory) return false;
    }
    for (const char* file : {"eboot.bin", "sce_module/libc.prx", "forwarder.json", "LICENSE-forwarder.txt",
                             "sce_sys/param.json", "sce_sys/icon0.png", "isshelf-console-tile.json"}) {
        const auto kind = path_kind(join(directory.c_str(), file));
        if (kind != PathKind::missing && kind != PathKind::file) return false;
    }
    bool okay = true;
    for (const char* file : {"eboot.bin", "sce_module/libc.prx", "forwarder.json", "LICENSE-forwarder.txt",
                             "sce_sys/param.json", "sce_sys/icon0.png", "isshelf-console-tile.json"}) {
        const auto path = join(directory.c_str(), file);
        if (path_kind(path) != PathKind::missing) okay = std::remove(path.c_str()) == 0 && okay;
    }
    okay = remove_dir(join(directory.c_str(), "sce_module")) && okay;
    okay = remove_dir(join(directory.c_str(), "sce_sys")) && okay;
    return remove_dir(directory) && okay;
}
bool registration_paths_absent(const ConsoleTileConfig& config, const char* title_id) {
    if (!config.user_app_root || !config.user_appmeta_root || !title_id || !title_id[0]) return false;
    return path_kind(join(config.user_app_root, title_id)) == PathKind::missing &&
           path_kind(join(config.user_appmeta_root, title_id)) == PathKind::missing;
}
bool prepare_stage(const ConsoleTileConfig& config, const ConsoleTileIntent& intent,
                   const char* title_id, const Text& stage) {
    Text verified_root;
    Text icon_digest;
    if (!resolve_pinned_bundle(config, intent.system, verified_root, &icon_digest)) return false;
    ConsoleTileConfig source = config;
    source.bundle_root = verified_root.c_str();
    if (!mkdir_exact(stage, 0700)) return false;
    const auto module = join(stage.c_str(), "sce_module");
    const auto sce_sys = join(stage.c_str(), "sce_sys");
    bool okay = mkdir_exact(module, 0700) && mkdir_exact(sce_sys, 0700);
    const Text icon(catalog[intent.system].id);
    if (okay) okay = copy_file(join(source.bundle_root, "template/eboot.bin"),
                              join(stage.c_str(), "eboot.bin"), 512 * 1024, true);
    if (okay) okay = copy_file(join(source.bundle_root, "template/sce_module/libc.prx"),
                              join(module.c_str(), "libc.prx"), 2 * 1024 * 1024, true);
    if (okay) okay = make_executable(join(stage.c_str(), "eboot.bin")) &&
                     make_executable(join(module.c_str(), "libc.prx"));
    if (okay) okay = copy_file(join(source.bundle_root, "LICENSE-forwarder.txt"),
                              join(stage.c_str(), "LICENSE-forwarder.txt"), 128 * 1024, true);
    if (okay) okay = copy_file(join(source.bundle_root, "icons/" + icon + ".png"),
                              join(sce_sys.c_str(), "icon0.png"), 2 * 1024 * 1024, true);
    Text param, forwarder, owner;
    if (okay) okay = make_param_json(source, intent, title_id, param) &&
                     make_forwarder_json(intent, forwarder) && make_owner_json(intent, title_id, owner);
    if (okay) okay = write_file(join(sce_sys.c_str(), "param.json"), param) &&
                     write_file(join(stage.c_str(), "forwarder.json"), forwarder) &&
                     write_file(join(stage.c_str(), "isshelf-console-tile.json"), owner);
    if (okay) {
#ifndef SHELF_PS5
        if (!config.test_skip_integrity)
#endif
            okay = sha256_file(join(stage.c_str(), "eboot.bin"), template_eboot_hash, 512 * 1024) &&
                   sha256_file(join(stage.c_str(), "sce_module/libc.prx"), template_libc_hash, 2 * 1024 * 1024) &&
                   sha256_file(join(stage.c_str(), "LICENSE-forwarder.txt"), template_license_hash, 128 * 1024) &&
                   sha256_file(join(stage.c_str(), "sce_sys/icon0.png"), icon_digest.c_str(), 2 * 1024 * 1024);
    }
    if (okay) {
        for (const char* file : {"LICENSE-forwarder.txt", "sce_sys/icon0.png", "sce_sys/param.json",
                                 "forwarder.json", "isshelf-console-tile.json"})
            okay = set_file_mode(join(stage.c_str(), file), 0666) && okay;
        // The owner's working homebrew title and PS5SX3 tiles use 0777 for
        // title and metadata directories; set it explicitly after staging.
        okay = set_directory_mode(stage, 0777) && set_directory_mode(module, 0777) &&
               set_directory_mode(sce_sys, 0777) && okay;
    }
    if (!okay) cleanup_folder(stage);
    return okay;
}
struct OwnedLookup {
    int count = 0;
    bool corrupted = false;
    char title_id[10]{};
    char issue[24]{};
    char issue_title_id[10]{};
    int issue_error = 0;
    ConsoleTileIntent intent;
};
void record_lookup_issue(OwnedLookup& result, const char* issue,
                         const char* title_id = nullptr, int error = 0) {
    result.corrupted = true;
    if (result.issue[0]) return;
    std::snprintf(result.issue, sizeof(result.issue), "%s", issue ? issue : "unknown");
    if (title_id)
        std::snprintf(result.issue_title_id, sizeof(result.issue_title_id), "%s", title_id);
    result.issue_error = error;
}
OwnedLookup find_owned(std::size_t system, const char* root, bool allow_missing_root = false) {
    OwnedLookup result;
    if (!root || !root[0]) { record_lookup_issue(result, "root-config"); return result; }
    const auto root_probe = path_probe(Text(root));
    // The retired root is created on the first removal; its absence is normal.
    if (root_probe.kind == PathKind::missing && allow_missing_root) return result;
    if (root_probe.kind != PathKind::directory) {
        record_lookup_issue(result, "root-path", nullptr, root_probe.os_error);
        return result;
    }
    for (unsigned number = preferred_first; number <= last_number; ++number) {
        if (!candidate_number(number)) continue;
        char title_id[10];
        if (!title_id_for(number, title_id)) continue;
        const auto directory = join(root, title_id);
        const auto probe = path_probe(directory);
        if (probe.kind == PathKind::other) {
            record_lookup_issue(result, "candidate-path", title_id, probe.os_error);
            continue;
        }
        if (probe.kind != PathKind::directory) continue;
        const auto marker = marker_ownership(directory, title_id, system);
        if (marker == MarkerOwnership::ambiguous) {
            record_lookup_issue(result, "marker", title_id);
            continue;
        }
        if (marker != MarkerOwnership::owned) continue;
        ++result.count;
        if (result.count > 1) {
            record_lookup_issue(result, "duplicate", title_id);
            continue;
        }
        std::snprintf(result.title_id, sizeof(result.title_id), "%s", title_id);
        if (const char* issue = owned_folder_issue(directory, title_id, system, result.intent))
            record_lookup_issue(result, issue, title_id);
    }
    return result;
}
struct CandidateProbe {
    bool attempted = false;
    char title_id[10]{};
    PathProbe homebrew, user_app, user_appmeta;
    ApiProbe api;
};
const char* path_kind_name(PathKind kind) {
    switch (kind) {
    case PathKind::missing: return "missing";
    case PathKind::file: return "file";
    case PathKind::directory: return "directory";
    case PathKind::other: return "other";
    }
    return "unknown";
}
const char* api_info_name(ApiInfo result) {
    switch (result) {
    case ApiInfo::missing: return "missing";
    case ApiInfo::registered: return "registered";
    case ApiInfo::pending: return "pending";
    case ApiInfo::conflict: return "conflict";
    case ApiInfo::failed: return "failed";
    }
    return "unknown";
}
void append_candidate_probe(char* output, std::size_t capacity, const char* label,
                            const CandidateProbe& probe) {
    if (!output || !capacity) return;
    const std::size_t used = std::strlen(output);
    if (used >= capacity) return;
    if (!probe.attempted) {
        std::snprintf(output + used, capacity - used, "%s=not-probed", label);
        return;
    }
    const char* api = probe.api.called ? api_info_name(probe.api.result) : "not-called";
    std::snprintf(output + used, capacity - used,
                  "%s=%s hb=%s/%d app=%s/%d meta=%s/%d api=%s(http=%d,status=%d,diag=%d)",
                  label, probe.title_id,
                  path_kind_name(probe.homebrew.kind), probe.homebrew.os_error,
                  path_kind_name(probe.user_app.kind), probe.user_app.os_error,
                  path_kind_name(probe.user_appmeta.kind), probe.user_appmeta.os_error,
                  api, probe.api.http_status, probe.api.api_status, probe.api.diagnostic_only ? 1 : 0);
}
void probe_rejected_candidate_api(const ConsoleTileConfig& config, CandidateProbe& probe) {
    if (!probe.attempted || probe.api.called) return;
    api_info(config, probe.title_id, join(config.homebrew_root, probe.title_id), &probe.api);
    probe.api.diagnostic_only = true;
}
const char* mode_name(unsigned mode) {
#ifdef _WIN32
    if ((mode & _S_IFMT) == _S_IFREG) return "file";
    if ((mode & _S_IFMT) == _S_IFDIR) return "directory";
#else
    if (S_ISREG(mode)) return "file";
    if (S_ISDIR(mode)) return "directory";
    if (S_ISLNK(mode)) return "symlink";
#endif
    return "other";
}
void parent_entry_probe(const Text& path, char (&result)[40]) {
    const char* slash = std::strrchr(path.c_str(), '/');
    if (!path.good() || !slash || !slash[1]) {
        std::snprintf(result, sizeof(result), "invalid");
        return;
    }
    Text parent(path.c_str(), static_cast<std::size_t>(slash - path.c_str()));
    if (parent.empty()) parent = "/";
    const char* name = slash + 1;
#ifdef _WIN32
    const Text pattern = join(parent.c_str(), "*");
    WIN32_FIND_DATAA entry{};
    HANDLE handle = FindFirstFileA(pattern.c_str(), &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        const int error = static_cast<int>(GetLastError());
        const DWORD attributes = GetFileAttributesA(parent.c_str());
        if (error == ERROR_FILE_NOT_FOUND && attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY))
            std::snprintf(result, sizeof(result), "absent/0");
        else
            std::snprintf(result, sizeof(result), "open-error/%d", error);
        return;
    }
    bool found = false;
    const char* type = "other";
    do {
        if (std::strcmp(entry.cFileName, name) != 0) continue;
        found = true;
        type = (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? "symlink"
               : (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "directory" : "file";
        break;
    } while (FindNextFileA(handle, &entry));
    const int read_error = found || GetLastError() == ERROR_NO_MORE_FILES ? 0 : static_cast<int>(GetLastError());
    const bool closed = FindClose(handle) != 0;
    if (!closed) std::snprintf(result, sizeof(result), "close-error/%d", static_cast<int>(GetLastError()));
    else if (read_error) std::snprintf(result, sizeof(result), "read-error/%d", read_error);
    else std::snprintf(result, sizeof(result), "%s/0", found ? type : "absent");
#else
    DIR* handle = opendir(parent.c_str());
    if (!handle) {
        std::snprintf(result, sizeof(result), "open-error/%d", errno);
        return;
    }
    bool found = false;
    const char* type = "other";
    errno = 0;
    for (dirent* entry = readdir(handle); entry; entry = readdir(handle)) {
        if (std::strcmp(entry->d_name, name) != 0) continue;
        found = true;
        type = entry->d_type == DT_REG ? "file" : entry->d_type == DT_DIR ? "directory"
               : entry->d_type == DT_LNK ? "symlink" : "other";
        break;
    }
    const int read_error = found ? 0 : errno;
    const bool closed = closedir(handle) == 0;
    if (!closed) std::snprintf(result, sizeof(result), "close-error/%d", errno);
    else if (read_error) std::snprintf(result, sizeof(result), "read-error/%d", read_error);
    else std::snprintf(result, sizeof(result), "%s/0", found ? type : "absent");
#endif
}
bool append_path_capability(char* output, std::size_t capacity, const char* label, const Text& path) {
    char stat_result[40], list_result[40];
    struct stat info{};
    if (::stat(path.c_str(), &info) == 0)
        std::snprintf(stat_result, sizeof(stat_result), "%s/0", mode_name(info.st_mode));
    else
        std::snprintf(stat_result, sizeof(stat_result), "error/%d", errno);
    // Parent entry type does not follow a symlink; an unknown type remains
    // occupied. fstatat would add libkernel_sys.prx to this app for a
    // diagnostic only, so the production allocation decision still uses its
    // original fail-closed lstat path until hardware confirms enumeration.
    parent_entry_probe(path, list_result);
    const std::size_t used = std::strlen(output);
    if (used >= capacity) return false;
    const int printed = std::snprintf(output + used, capacity - used,
                                      "; %s stat=%s list=%s", label, stat_result, list_result);
    return printed >= 0 && static_cast<std::size_t>(printed) < capacity - used;
}
void append_allocation_capabilities(const ConsoleTileConfig& config,
                                    const CandidateProbe& preferred, const CandidateProbe& alternate,
                                    char* output, std::size_t capacity) {
    bool complete = true;
    struct NamedCandidate { const char* label; const CandidateProbe* probe; };
    struct NamedRoot { const char* label; const char* path; };
    const NamedCandidate candidates[] = {{"pref", &preferred}, {"alt", &alternate}};
    const NamedRoot roots[] = {{"hb", config.homebrew_root}, {"app", config.user_app_root},
                               {"meta", config.user_appmeta_root}};
    for (const auto& candidate : candidates) {
        if (!candidate.probe->attempted) continue;
        for (const auto& root : roots) {
            char label[24];
            std::snprintf(label, sizeof(label), "%s.%s", candidate.label, root.label);
            complete = append_path_capability(output, capacity, label,
                                              join(root.path, candidate.probe->title_id)) && complete;
        }
    }
    complete = append_path_capability(output, capacity, "known.hb-app",
                                      join(config.homebrew_root, shelf_title_id)) && complete;
    if (!complete && capacity > 25) {
        output[capacity - 25] = 0;
        std::snprintf(output + std::strlen(output), capacity - std::strlen(output), "; CAPABILITY-TRUNCATED");
    }
}
bool candidate_free(const ConsoleTileConfig& config, unsigned number, char (&title_id)[10], bool& api_error,
                    CandidateProbe* evidence = nullptr) {
    api_error = false;
    if (!title_id_for(number, title_id)) return false;
    if (evidence) {
        *evidence = {};
        evidence->attempted = true;
        std::snprintf(evidence->title_id, sizeof(evidence->title_id), "%s", title_id);
    }
    const auto directory = join(config.homebrew_root, title_id);
    const auto homebrew = path_probe(directory);
    const auto user_app = path_probe(join(config.user_app_root, title_id));
    const auto user_appmeta = path_probe(join(config.user_appmeta_root, title_id));
    if (evidence) {
        evidence->homebrew = homebrew;
        evidence->user_app = user_app;
        evidence->user_appmeta = user_appmeta;
    }
    if (homebrew.kind != PathKind::missing || user_app.kind != PathKind::missing ||
        user_appmeta.kind != PathKind::missing) {
        if ((homebrew.kind == PathKind::other && homebrew.os_error != 0) ||
            (user_app.kind == PathKind::other && user_app.os_error != 0) ||
            (user_appmeta.kind == PathKind::other && user_appmeta.os_error != 0))
            api_error = true;
        return false;
    }
    const auto status = api_info(config, title_id, directory, evidence ? &evidence->api : nullptr);
    if (status == ApiInfo::failed) api_error = true;
    return status == ApiInfo::missing;
}
bool choose_title(const ConsoleTileConfig& config, std::size_t system, char (&title_id)[10], bool& api_error,
                  CandidateProbe* preferred_probe = nullptr, CandidateProbe* alternate_probe = nullptr) {
    api_error = false;
    const unsigned preferred = console_tile_preferred_number(system);
    bool failed = false;
    if (candidate_free(config, preferred, title_id, failed, preferred_probe)) return true;
    if (failed) { api_error = true; return false; }
    for (unsigned number = alternate_first; number <= last_number; ++number) {
        if (number == reserved_number) continue;
        if (candidate_free(config, number, title_id, failed,
                           number == alternate_first ? alternate_probe : nullptr)) return true;
        if (failed) { api_error = true; return false; }
    }
    return false;
}
} // namespace

bool console_tile_title_id(unsigned number, char (&output)[10]) { return title_id_for(number, output); }
unsigned console_tile_preferred_number(std::size_t system) {
    return system < system_count ? preferred_first + static_cast<unsigned>(system) : 0;
}

ConsoleTileAvailability availability_result(ConsoleTileAvailabilityReason reason, const char* detail = "",
                                            int http_status = 0) {
    ConsoleTileAvailability result;
    result.reason = reason;
    result.http_status = http_status;
    std::snprintf(result.detail, sizeof(result.detail), "%s", detail);
    switch (reason) {
    case ConsoleTileAvailabilityReason::ready:
        std::snprintf(result.message, sizeof(result.message), "PS5 Home Screen shortcuts are ready.");
        break;
    case ConsoleTileAvailabilityReason::hardware_qualification:
        std::snprintf(result.message, sizeof(result.message),
                      "PS5 Home Screen shortcut writes are still being tested.");
        break;
    case ConsoleTileAvailabilityReason::invalid_config:
        std::snprintf(result.message, sizeof(result.message), "Shortcut configuration is missing: %s.", detail);
        break;
    case ConsoleTileAvailabilityReason::bundle_asset_missing_or_unsafe:
        std::snprintf(result.message, sizeof(result.message), "Shortcut bundle file is missing or unsafe: %s.", detail);
        break;
    case ConsoleTileAvailabilityReason::bundle_asset_integrity:
        std::snprintf(result.message, sizeof(result.message), "Shortcut bundle file failed SHA-256 verification: %s.", detail);
        break;
    case ConsoleTileAvailabilityReason::api_transport:
        std::snprintf(result.message, sizeof(result.message),
                      "ShadowMount loopback version request failed (connection or HTTP framing).");
        break;
    case ConsoleTileAvailabilityReason::api_http_status:
        std::snprintf(result.message, sizeof(result.message), "ShadowMount version returned HTTP %d.", http_status);
        break;
    case ConsoleTileAvailabilityReason::api_malformed_response:
        std::snprintf(result.message, sizeof(result.message), "ShadowMount version response is not valid JSON.");
        break;
    case ConsoleTileAvailabilityReason::api_status:
        std::snprintf(result.message, sizeof(result.message), "ShadowMount version status is missing or not zero.");
        break;
    case ConsoleTileAvailabilityReason::api_version:
        std::snprintf(result.message, sizeof(result.message), "ShadowMount API version is missing or unsupported.");
        break;
    case ConsoleTileAvailabilityReason::api_capabilities:
        std::snprintf(result.message, sizeof(result.message), "ShadowMount capabilities are missing or invalid.");
        break;
    case ConsoleTileAvailabilityReason::api_missing_capability:
        std::snprintf(result.message, sizeof(result.message), "ShadowMount capability is missing: %s.", detail);
        break;
    }
    return result;
}

ConsoleTileAvailability bundle_availability(const ConsoleTileConfig& config, const char* root,
                                            bool direct_fallback) {
#ifdef SHELF_PS5
    (void)config;
#endif
    for (const auto& asset : pinned_assets) {
        const Text path = join(root, asset.relative);
        if (!bundle_file_readable(path)) {
            auto result = availability_result(ConsoleTileAvailabilityReason::bundle_asset_missing_or_unsafe,
                                              asset.relative);
            std::snprintf(result.bundle_source, sizeof(result.bundle_source), "%s", root);
            if (direct_fallback)
                std::snprintf(result.message, sizeof(result.message),
                              "Direct installed bundle file is missing or unsafe: %s.", asset.relative);
            return result;
        }
#ifndef SHELF_PS5
        if (!config.test_skip_integrity && !sha256_file(path, asset.digest, asset.limit, nullptr, true))
#else
        if (!sha256_file(path, asset.digest, asset.limit, nullptr, true))
#endif
        {
            auto result = availability_result(ConsoleTileAvailabilityReason::bundle_asset_integrity,
                                              asset.relative);
            std::snprintf(result.bundle_source, sizeof(result.bundle_source), "%s", root);
            if (direct_fallback)
                std::snprintf(result.message, sizeof(result.message),
                              "Direct installed bundle file failed SHA-256 verification: %s.", asset.relative);
            return result;
        }
    }
    auto result = availability_result(ConsoleTileAvailabilityReason::ready);
    std::snprintf(result.bundle_source, sizeof(result.bundle_source), "%s", root);
    if (direct_fallback)
        std::snprintf(result.message, sizeof(result.message),
                      "PS5 Home Screen shortcuts are ready via the direct installed bundle.");
    return result;
}

ConsoleTileAvailability ConsoleTileManager::availability() const {
    if (!console_tile_writes_enabled)
        return availability_result(ConsoleTileAvailabilityReason::hardware_qualification);
    if (!config_.homebrew_root) return availability_result(ConsoleTileAvailabilityReason::invalid_config, "homebrew root");
    if (!config_.bundle_root) return availability_result(ConsoleTileAvailabilityReason::invalid_config, "bundle root");
    if (!config_.user_app_root) return availability_result(ConsoleTileAvailabilityReason::invalid_config, "user app root");
    if (!config_.user_appmeta_root) return availability_result(ConsoleTileAvailabilityReason::invalid_config, "user app metadata root");
    if (!config_.retired_root) return availability_result(ConsoleTileAvailabilityReason::invalid_config, "retired root");
    if (!config_.post && !config_.api_port)
        return availability_result(ConsoleTileAvailabilityReason::invalid_config, "ShadowMount port");
    const auto primary = bundle_availability(config_, config_.bundle_root, false);
    const bool has_fallback = config_.bundle_fallback_root && config_.bundle_fallback_root[0] &&
                              std::strcmp(config_.bundle_root, config_.bundle_fallback_root) != 0;
    const auto source = !primary.ready() && has_fallback
                            ? bundle_availability(config_, config_.bundle_fallback_root, true)
                            : primary;
    if (!source.ready()) return source;
    char reply[2048];
    const int http_status = post(config_, "/api/v1/version", "{}", reply, sizeof(reply));
    if (http_status < 0) return availability_result(ConsoleTileAvailabilityReason::api_transport);
    if (http_status != 200)
        return availability_result(ConsoleTileAvailabilityReason::api_http_status, "", http_status);
    JsonDocument doc;
    if (!doc.parse(reply, std::strlen(reply)) || !cJSON_IsObject(doc.root))
        return availability_result(ConsoleTileAvailabilityReason::api_malformed_response);
    const auto* status = cJSON_GetObjectItemCaseSensitive(doc.root, "status");
    const auto* version = cJSON_GetObjectItemCaseSensitive(doc.root, "api_version");
    const auto* capabilities = cJSON_GetObjectItemCaseSensitive(doc.root, "capabilities");
    if (!cJSON_IsNumber(status) || status->valueint != 0)
        return availability_result(ConsoleTileAvailabilityReason::api_status);
    if (!cJSON_IsNumber(version) || version->valueint != 1)
        return availability_result(ConsoleTileAvailabilityReason::api_version);
    if (!cJSON_IsArray(capabilities))
        return availability_result(ConsoleTileAvailabilityReason::api_capabilities);
    for (const char* required : {"game_info", "rescan", "uninstall_game",
                                 "list_manual_sources", "remove_manual_source"}) {
        bool found = false;
        for (const auto* item = capabilities->child; item; item = item->next)
            found = found || (cJSON_IsString(item) && item->valuestring &&
                              std::strcmp(item->valuestring, required) == 0);
        if (!found) return availability_result(ConsoleTileAvailabilityReason::api_missing_capability, required);
    }
    return source;
}

ConsoleTileAvailability ConsoleTileManager::catalog_read_availability() const {
    if (!config_.post && !config_.api_port)
        return availability_result(ConsoleTileAvailabilityReason::invalid_config, "ShadowMount port");
    char reply[2048];
    const int code = post(config_, "/api/v1/version", "{}", reply, sizeof(reply));
    if (code < 0) return availability_result(ConsoleTileAvailabilityReason::api_transport);
    if (code != 200)
        return availability_result(ConsoleTileAvailabilityReason::api_http_status, "", code);
    JsonDocument doc;
    if (!doc.parse(reply, std::strlen(reply)) || !cJSON_IsObject(doc.root))
        return availability_result(ConsoleTileAvailabilityReason::api_malformed_response);
    const auto* status = cJSON_GetObjectItemCaseSensitive(doc.root, "status");
    const auto* version = cJSON_GetObjectItemCaseSensitive(doc.root, "api_version");
    const auto* capabilities = cJSON_GetObjectItemCaseSensitive(doc.root, "capabilities");
    if (!exact_json_number(status, 0))
        return availability_result(ConsoleTileAvailabilityReason::api_status);
    if (!exact_json_number(version, 1))
        return availability_result(ConsoleTileAvailabilityReason::api_version);
    if (!cJSON_IsArray(capabilities))
        return availability_result(ConsoleTileAvailabilityReason::api_capabilities);
    for (const char* required : {"game_info", "list_manual_sources"}) {
        bool found = false;
        for (const auto* item = capabilities->child; item; item = item->next)
            found = found || (cJSON_IsString(item) && item->valuestring &&
                              std::strcmp(item->valuestring, required) == 0);
        if (!found)
            return availability_result(ConsoleTileAvailabilityReason::api_missing_capability, required);
    }
    auto result = availability_result(ConsoleTileAvailabilityReason::ready);
    std::snprintf(result.message, sizeof(result.message),
                  "ShadowMount shortcut status is readable; creation is still unavailable.");
    return result;
}

ConsoleTileOutcome ConsoleTileManager::inspect_catalog_title(std::size_t system) const {
    if (system >= system_count)
        return outcome_for(system, ConsoleTileStage::failed, "Unknown console identity.");
    const auto& pin = isshelf_badge::generic_tile::tiles[system];
    if (catalog[system].id != pin.system || !pin.title_id || std::strlen(pin.title_id) != 9)
        return outcome_for(system, ConsoleTileStage::failed, "Shortcut catalog identity differs.");
    auto result = outcome_for(system, ConsoleTileStage::unavailable,
                              "ShadowMount shortcut status cannot be checked.");
    std::snprintf(result.title_id, sizeof(result.title_id), "%s", pin.title_id);
    const auto available = catalog_read_availability();
    if (!available.ready()) {
        set_message(result, available.message);
        return result;
    }
    char request[64], reply[8192], game_path[513]{};
    std::snprintf(request, sizeof(request), "{\"title_id\":\"%s\"}", pin.title_id);
    const int info_code = post(config_, "/api/v1/games/info", request,
                               reply, sizeof(reply));
    if (info_code != 200 && info_code != 404) return result;
    JsonDocument info;
    if (!info.parse(reply, std::strlen(reply)) || !cJSON_IsObject(info.root))
        return result;
    const auto* info_status = cJSON_GetObjectItemCaseSensitive(info.root, "status");
    bool known = false, installed = false;
    if (info_code == 404) {
        if (!exact_json_number(info_status, ENOENT)) return result;
    } else {
        const auto* installed_item = cJSON_GetObjectItemCaseSensitive(info.root, "installed");
        const char* path = json_string(info.root, "path");
        if (!exact_json_number(info_status, 0) ||
            std::strcmp(json_string(info.root, "title_id"), pin.title_id) != 0 ||
            !bounded_api_path(path) || !cJSON_IsBool(installed_item)) return result;
        std::snprintf(game_path, sizeof(game_path), "%s", path);
        installed = cJSON_IsTrue(installed_item);
        known = true;
    }
    if (post(config_, "/api/v1/manual/list", "{}", reply,
             sizeof(reply)) != 200) return result;
    JsonDocument manual;
    if (!manual.parse(reply, std::strlen(reply)) ||
        !cJSON_IsObject(manual.root) ||
        !exact_json_number(cJSON_GetObjectItemCaseSensitive(manual.root, "status"), 0))
        return result;
    const auto* paths = cJSON_GetObjectItemCaseSensitive(manual.root, "paths");
    const int count = cJSON_GetArraySize(paths);
    if (!cJSON_IsArray(paths) || count < 0 || count > 512 ||
        !exact_json_number(cJSON_GetObjectItemCaseSensitive(manual.root, "count"), count))
        return result;
    std::array<const char*, 512> seen{};
    int used = 0, matching_title_paths = 0;
    bool exact_manual = false;
    for (const auto* item = paths->child; item; item = item->next) {
        if (used >= count || !cJSON_IsString(item) ||
            !bounded_api_path(item->valuestring)) return result;
        for (int earlier = 0; earlier < used; ++earlier)
            if (std::strcmp(item->valuestring, seen[static_cast<std::size_t>(earlier)]) == 0)
                return result;
        seen[static_cast<std::size_t>(used++)] = item->valuestring;
        if (title_basename(item->valuestring, pin.title_id)) ++matching_title_paths;
        if (known && std::strcmp(item->valuestring, game_path) == 0) exact_manual = true;
    }
    if (used != count) return result;
    if (matching_title_paths > 1 ||
        (known && matching_title_paths && !exact_manual)) {
        result.stage = ConsoleTileStage::conflict;
        set_message(result, "ShadowMount lists a conflicting source for this title; inspect it before changes.");
    } else if (!known && matching_title_paths) {
        result.stage = ConsoleTileStage::pending;
        set_message(result, "ShadowMount lists a manual source, but no installed title record.");
    } else if (!known) {
        set_message(result, "ShadowMount has no title record; source and Home checks remain required.");
    } else if (installed) {
        result.stage = ConsoleTileStage::registered;
        set_message(result, exact_manual
                           ? "ShadowMount reports this title installed with a manual source; verify ownership and Home."
                           : "ShadowMount reports this title installed; manual tracking is not shown.");
    } else {
        result.stage = ConsoleTileStage::pending;
        set_message(result, "ShadowMount knows this title, but does not report it installed.");
    }
    // An API title record never establishes isShelf ownership or safe CREATE.
    return result;
}

bool ConsoleTileManager::available() const { return availability().ready(); }

ConsoleTileOutcome ConsoleTileManager::inspect(std::size_t system) const {
    if (system >= system_count)
        return outcome_for(system, ConsoleTileStage::failed, "Unknown console identity.");
    if (!config_.homebrew_root || !config_.retired_root)
        return outcome_for(system, ConsoleTileStage::unavailable, "Shortcut paths are unavailable.");
    const auto active = find_owned(system, config_.homebrew_root);
    const auto retired = find_owned(system, config_.retired_root, true);
    if (active.count > 1 || retired.count > 1 || (active.count && retired.count) ||
        active.corrupted || retired.corrupted) {
        auto result = outcome_for(system, ConsoleTileStage::conflict,
                                  "Shortcut ownership or contents are ambiguous.");
        std::snprintf(result.diagnostic, sizeof(result.diagnostic),
                      "active count=%d issue=%s title=%s errno=%d; "
                      "retired count=%d issue=%s title=%s errno=%d",
                      active.count, active.issue[0] ? active.issue : "none",
                      active.issue_title_id[0] ? active.issue_title_id : "none", active.issue_error,
                      retired.count, retired.issue[0] ? retired.issue : "none",
                      retired.issue_title_id[0] ? retired.issue_title_id : "none", retired.issue_error);
        return result;
    }
    if (retired.count) {
        auto result = outcome_for(system, ConsoleTileStage::removed_pending,
                                  "Shortcut removal is waiting for a final PS5 registration check.");
        result.owned = true;
        result.intent = retired.intent;
        std::snprintf(result.title_id, sizeof(result.title_id), "%s", retired.title_id);
        const auto info = api_info(config_, retired.title_id, {});
        if (info == ApiInfo::missing)
            set_message(result, "PS5 registration is absent; finish removal to clean the recovery copy.");
        else if (info == ApiInfo::registered || info == ApiInfo::pending) {
            result.handoff_required = true;
            set_message(result, "Removal is queued. Close isShelf so ShadowMount can update PS5 Home.");
        } else
            set_message(result, "Cannot verify whether the PS5 removed the shortcut yet.");
        return result;
    }
    if (!active.count)
        return outcome_for(system, ConsoleTileStage::absent, "No shortcut for this console.");
    auto result = outcome_for(system, ConsoleTileStage::pending, "Shortcut source exists; registration is pending.");
    result.owned = true;
    result.intent = active.intent;
    std::snprintf(result.title_id, sizeof(result.title_id), "%s", active.title_id);
    const auto status = api_info(config_, active.title_id, join(config_.homebrew_root, active.title_id));
    if (status == ApiInfo::registered) {
        result.stage = ConsoleTileStage::registered;
        set_message(result, "ShadowMount reports the shortcut installed; verify it on the TV.");
    } else if (status == ApiInfo::conflict) {
        result.stage = ConsoleTileStage::conflict;
        set_message(result, "The title ID is registered to a different source.");
    } else if (status == ApiInfo::failed) {
        result.stage = ConsoleTileStage::unavailable;
        set_message(result, "Shortcut source exists; PS5 registration cannot be checked.");
    }
    return result;
}

ConsoleTileOutcome ConsoleTileManager::upsert(const ConsoleTileIntent& intent) const {
    if (!console_tile_writes_enabled)
        return outcome_for(intent.system, ConsoleTileStage::unavailable,
                           "PS5 Home Screen shortcut writes are still being tested.");
    if (!console_tile_creation_intent_valid(intent))
        return outcome_for(intent.system, ConsoleTileStage::failed, "Invalid shortcut destination or emulator.");
    auto current = inspect(intent.system);
    if (current.stage == ConsoleTileStage::conflict || current.stage == ConsoleTileStage::removed_pending ||
        current.stage == ConsoleTileStage::failed)
        return current;
    if (current.owned) {
        if (current.stage == ConsoleTileStage::unavailable)
            return current;
        Text forwarder;
        const auto directory = join(config_.homebrew_root, current.title_id);
        bool runtime_changed = false;
        bool artwork_changed = false;
        if (!upgrade_owned_runtime(config_, intent, current.title_id,
                                   directory, runtime_changed))
            return outcome_for(intent.system, ConsoleTileStage::failed,
                               "Could not safely upgrade the owned shortcut runtime.");
        if (!refresh_owned_artwork(config_, intent, directory, artwork_changed))
            return outcome_for(intent.system, ConsoleTileStage::failed,
                               "Could not safely refresh the owned shortcut artwork.");
        if (!make_forwarder_json(intent, forwarder) ||
            !replace_file(join(directory.c_str(), "forwarder.json"), forwarder))
            return outcome_for(intent.system, ConsoleTileStage::failed, "Could not update the owned shortcut.");
        ConsoleTileIntent readback;
        if (!forwarder_intent(directory, intent.system, readback) ||
            !console_tile_intents_equal(readback, intent))
            return outcome_for(intent.system, ConsoleTileStage::failed, "Shortcut destination readback did not match.");
        current.intent = intent;
        if (current.stage == ConsoleTileStage::pending || artwork_changed) {
            current.stage = queue_scan(config_);
            if (current.stage == ConsoleTileStage::deferred)
                set_message(current, artwork_changed
                                         ? "Shortcut artwork updated; close isShelf to refresh it on PS5 Home."
                                         : runtime_changed
                                               ? "Shortcut runtime and destination update queued; close isShelf for registration."
                                               : "Shortcut update queued; close isShelf for registration.");
            else if (current.stage == ConsoleTileStage::pending)
                set_message(current, artwork_changed
                                         ? "Shortcut artwork updated; PS5 Home refresh is pending."
                                         : runtime_changed
                                               ? "Shortcut runtime and destination updated; registration is pending."
                                               : "Shortcut update queued; registration is pending.");
            else
                set_message(current, "Shortcut updated, but registration retry could not be confirmed.");
            return current;
        }
        set_message(current, current.stage == ConsoleTileStage::registered
                                 ? (runtime_changed
                                        ? "Shortcut runtime and destination updated; open it to verify on the TV."
                                        : "Shortcut destination updated; open it to verify on the TV.")
                                 : "Shortcut destination updated; registration is still pending.");
        return current;
    }
    if (!available())
        return outcome_for(intent.system, ConsoleTileStage::unavailable,
                           "The shortcut bundle or ShadowMount API is unavailable.");
    char title_id[10];
    bool api_error = false;
    CandidateProbe preferred, alternate;
    if (!choose_title(config_, intent.system, title_id, api_error, &preferred, &alternate)) {
        // Two read-only API observations clarify a filesystem rejection, but
        // cannot turn a rejected candidate into an allocatable title ID.
        probe_rejected_candidate_api(config_, preferred);
        probe_rejected_candidate_api(config_, alternate);
        auto result = outcome_for(intent.system,
                                  api_error ? ConsoleTileStage::unavailable : ConsoleTileStage::conflict,
                                  api_error ? "Could not verify title-ID availability."
                                            : "No free shortcut title ID remains.");
        append_candidate_probe(result.diagnostic, sizeof(result.diagnostic), "pref", preferred);
        const std::size_t used = std::strlen(result.diagnostic);
        if (used + 2 < sizeof(result.diagnostic)) std::snprintf(result.diagnostic + used,
                                                                sizeof(result.diagnostic) - used, "; ");
        append_candidate_probe(result.diagnostic, sizeof(result.diagnostic), "alt", alternate);
        const std::size_t with_candidates = std::strlen(result.diagnostic);
        if (with_candidates < sizeof(result.diagnostic))
            std::snprintf(result.diagnostic + with_candidates,
                          sizeof(result.diagnostic) - with_candidates,
                          "; roots hb=%s app=%s meta=%s", config_.homebrew_root, config_.user_app_root,
                          config_.user_appmeta_root);
        append_allocation_capabilities(config_, preferred, alternate,
                                       result.diagnostic, sizeof(result.diagnostic));
        return result;
    }
    const auto target = join(config_.homebrew_root, title_id);
    const auto stage = join(config_.homebrew_root, Text(".isshelf-stage-") + title_id);
    if (path_kind(stage) != PathKind::missing || !prepare_stage(config_, intent, title_id, stage))
        return outcome_for(intent.system, ConsoleTileStage::failed,
                           "Could not prepare the shortcut in an isolated folder.");
    // Reserve this exact title directory exclusively. On PS5, rename over an
    // empty directory is atomic; a different writer cannot claim it meanwhile.
    if (!mkdir_exact(target)) {
        cleanup_folder(stage);
        return outcome_for(intent.system, ConsoleTileStage::conflict, "A title claimed the shortcut ID during creation.");
    }
#ifdef _WIN32
    // Host test filesystem semantics differ from PS5: Windows cannot replace
    // an empty directory with rename. Hardware uses the reserved directory.
    if (!remove_dir(target)) {
        cleanup_folder(stage);
        return outcome_for(intent.system, ConsoleTileStage::failed, "Could not complete shortcut reservation.");
    }
#endif
    if (std::rename(stage.c_str(), target.c_str()) != 0) {
        remove_dir(target);
        cleanup_folder(stage);
        return outcome_for(intent.system, ConsoleTileStage::failed, "Could not place the shortcut folder.");
    }
    if (!directory_mode(target, 0777) ||
        !directory_mode(join(target.c_str(), "sce_module"), 0777) ||
        !directory_mode(join(target.c_str(), "sce_sys"), 0777))
        return outcome_for(intent.system, ConsoleTileStage::conflict,
                           "Shortcut folder was placed but has unreadable directory permissions.");
    ConsoleTileIntent readback;
    if (!owned_folder(target, title_id, intent.system, readback) ||
        !console_tile_intents_equal(readback, intent))
        return outcome_for(intent.system, ConsoleTileStage::conflict,
                           "Shortcut files were placed but ownership readback failed.");
    auto result = outcome_for(intent.system, queue_scan(config_), "Shortcut source created; registration is pending.");
    result.owned = true;
    result.intent = intent;
    std::snprintf(result.title_id, sizeof(result.title_id), "%s", title_id);
    if (result.stage == ConsoleTileStage::deferred)
        set_message(result, "Shortcut is queued; close isShelf for the PS5 to finish registration.");
    else if (result.stage == ConsoleTileStage::unavailable || result.stage == ConsoleTileStage::failed)
        set_message(result, "Shortcut source exists, but the scan could not be confirmed.");
    return result;
}

ConsoleTileEmulatorBatchOutcome ConsoleTileManager::upsert_emulators(
    const std::array<ConsoleTileIntent, system_count>& intents,
    const std::array<bool, system_count>& selected) const {
    ConsoleTileEmulatorBatchOutcome batch;
    if (!console_tile_writes_enabled) {
        batch.failed_stage = ConsoleTileStage::unavailable;
        std::snprintf(batch.message, sizeof(batch.message),
                      "PS5 Home Screen shortcut writes are still being tested.");
        return batch;
    }

    std::array<bool, system_count> was_absent{};
    std::array<bool, system_count> finish_removal{};
    for (std::size_t system = 0; system < system_count; ++system) {
        if (!selected[system])
            continue;
        ++batch.selected;
        const auto& intent = intents[system];
        if (intent.system != system ||
            !console_tile_creation_intent_valid(intent)) {
            batch.failed_system = system;
            batch.failed_stage = ConsoleTileStage::failed;
            std::snprintf(batch.message, sizeof(batch.message),
                          "Nothing changed: %.*s has an invalid console shortcut route.",
                          static_cast<int>(catalog[system].name.size()),
                          catalog[system].name.data());
            return batch;
        }
        const auto current = inspect(system);
        const bool absent = current.stage == ConsoleTileStage::absent && !current.owned;
        const bool editable = current.owned &&
            (current.stage == ConsoleTileStage::registered ||
             current.stage == ConsoleTileStage::pending ||
             current.stage == ConsoleTileStage::deferred);
        const bool removable_recovery = current.stage == ConsoleTileStage::removed_pending &&
            current.owned && current.title_id[0] &&
            api_info(config_, current.title_id, {}) == ApiInfo::missing &&
            registration_paths_absent(config_, current.title_id);
        if (!absent && !editable && !removable_recovery) {
            batch.failed_system = system;
            batch.failed_stage = current.stage;
            std::snprintf(batch.message, sizeof(batch.message),
                          "Nothing changed: %.*s shortcut needs attention first.",
                          static_cast<int>(catalog[system].name.size()),
                          catalog[system].name.data());
            return batch;
        }
        was_absent[system] = absent || removable_recovery;
        finish_removal[system] = removable_recovery;
    }
    if (!batch.selected) {
        batch.complete = true;
        std::snprintf(batch.message, sizeof(batch.message),
                      "No supported installed emulator routes were found.");
        return batch;
    }

    for (std::size_t system = 0; system < system_count; ++system) {
        if (!selected[system])
            continue;
        if (finish_removal[system]) {
            const auto removed = confirm_removed(system);
            if (removed.stage != ConsoleTileStage::absent) {
                batch.failed_system = system;
                batch.failed_stage = removed.stage;
                std::snprintf(batch.message, sizeof(batch.message),
                              "Prepared %zu of %zu console tiles; could not finish %.*s's earlier removal. Reopen isShelf and try again.",
                              batch.committed, batch.selected,
                              static_cast<int>(catalog[system].name.size()),
                              catalog[system].name.data());
                return batch;
            }
        }
        const auto outcome = upsert(intents[system]);
        const bool exact_source = outcome.owned &&
            console_tile_intents_equal(outcome.intent, intents[system]);
        if (exact_source) {
            batch.committed_systems[system] = true;
            batch.intents[system] = intents[system];
            ++batch.committed;
            if (was_absent[system])
                ++batch.created;
            else
                ++batch.updated;
        }
        const bool accepted = exact_source &&
            (outcome.stage == ConsoleTileStage::registered ||
             outcome.stage == ConsoleTileStage::pending ||
             outcome.stage == ConsoleTileStage::deferred);
        if (!accepted) {
            batch.failed_system = system;
            batch.failed_stage = outcome.stage;
            std::snprintf(batch.message, sizeof(batch.message),
                          "Prepared %zu of %zu console tiles; stopped at %.*s. Reopen isShelf and run this action again.",
                          batch.committed, batch.selected,
                          static_cast<int>(catalog[system].name.size()),
                          catalog[system].name.data());
            return batch;
        }
        if (outcome.stage != ConsoleTileStage::registered)
            batch.close_required = true;
    }

    batch.complete = true;
    std::snprintf(batch.message, sizeof(batch.message),
                  "Added %zu and updated %zu console tiles. Standalone emulators open directly; shared RetroArch systems open their matching isShelf library.%s",
                  batch.created, batch.updated,
                  batch.close_required ? " Close isShelf so ShadowMount can update PS5 Home." : "");
    return batch;
}

ConsoleTileBatchOutcome ConsoleTileManager::refresh_installed() const {
    ConsoleTileBatchOutcome batch;
    if (!console_tile_writes_enabled) {
        std::snprintf(batch.message, sizeof(batch.message),
                      "PS5 Home Screen shortcut writes are still being tested.");
        batch.failed_stage = ConsoleTileStage::unavailable;
        return batch;
    }

    // Validate the entire owned set before changing its first member. This
    // catches an ambiguous source or incomplete removal without producing an
    // avoidable half-migration. The update loop remains prefix-durable and is
    // deliberately safe to run again if the filesystem changes afterward.
    struct InstalledSource {
        ConsoleTileIntent intent;
        char title_id[10]{};
        bool registered = false;
    };
    std::array<InstalledSource, system_count> installed{};
    for (std::size_t system = 0; system < system_count; ++system) {
        const auto current = inspect(system);
        if (current.stage == ConsoleTileStage::absent)
            continue;
        if (current.stage == ConsoleTileStage::removed_pending) {
            ++batch.removing;
            continue;
        }
        const bool refreshable =
            current.stage == ConsoleTileStage::registered ||
            current.stage == ConsoleTileStage::pending ||
            current.stage == ConsoleTileStage::deferred;
        if (!refreshable || !current.owned || !current.title_id[0] ||
            !console_tile_intent_valid(current.intent)) {
            batch.failed_system = system;
            batch.failed_stage = current.stage;
            std::snprintf(batch.message, sizeof(batch.message),
                          "Nothing changed: %.*s shortcut needs attention first.",
                          static_cast<int>(catalog[system].name.size()),
                          catalog[system].name.data());
            return batch;
        }
        auto& source = installed[batch.installed++];
        if (!normalize_console_tile_intent(current.intent, source.intent)) {
            batch.failed_system = system;
            batch.failed_stage = ConsoleTileStage::failed;
            std::snprintf(batch.message, sizeof(batch.message),
                          "Nothing changed: %.*s shortcut has no supported launch destination.",
                          static_cast<int>(catalog[system].name.size()),
                          catalog[system].name.data());
            return batch;
        }
        source.registered = current.stage == ConsoleTileStage::registered;
        std::snprintf(source.title_id, sizeof(source.title_id), "%s",
                      current.title_id);
    }

    if (!batch.installed) {
        batch.complete = true;
        if (batch.removing)
            std::snprintf(batch.message, sizeof(batch.message),
                          "No installed shortcuts to update; %zu removal is still pending.",
                          batch.removing);
        else
            std::snprintf(batch.message, sizeof(batch.message),
                          "No installed isShelf shortcuts need updating.");
        return batch;
    }

    for (std::size_t index = 0; index < batch.installed; ++index) {
        const auto& current = installed[index];
        const auto& intent = current.intent;
        const auto directory = join(config_.homebrew_root, current.title_id);
        bool runtime_changed = false, artwork_changed = false;
        Text forwarder;
        const bool refreshed =
            upgrade_owned_runtime(config_, intent, current.title_id,
                                  directory, runtime_changed) &&
            refresh_owned_artwork(config_, intent, directory,
                                  artwork_changed) &&
            make_forwarder_json(intent, forwarder) &&
            replace_file(join(directory.c_str(), "forwarder.json"), forwarder);
        ConsoleTileIntent readback;
        const bool exact = refreshed &&
            forwarder_intent(directory, intent.system, readback) &&
            console_tile_intents_equal(readback, intent);
        if (!exact) {
            batch.failed_system = intent.system;
            batch.failed_stage = ConsoleTileStage::failed;
            std::snprintf(batch.message, sizeof(batch.message),
                          "Updated %zu of %zu shortcuts; stopped at %.*s. Run Update all again after fixing it.",
                          batch.refreshed, batch.installed,
                          static_cast<int>(catalog[intent.system].name.size()),
                          catalog[intent.system].name.data());
            return batch;
        }
        ++batch.refreshed;
    }

    // A normal rescan does not replace already-registered icon0.png copies.
    // Leave every owned source in place and defer every stale registered copy
    // to the bounded post-close helper. That helper paces one exact title per
    // second and has a bounded retry for ShadowMount's reviewed `game_active`
    // and `scanner_busy` replies. Sending a foreground burst can otherwise
    // fill beta11's storage queue before the full owned set is refreshed.
    for (std::size_t index = 0; index < batch.installed; ++index) {
        const auto& current = installed[index];
        if (!current.registered ||
            registered_artwork_current(config_, current.intent,
                                       current.title_id))
            continue;
        if (!append_handoff_title(batch.handoff_titles,
                                  sizeof(batch.handoff_titles),
                                  current.title_id)) {
            batch.failed_system = current.intent.system;
            batch.failed_stage = ConsoleTileStage::failed;
            std::snprintf(batch.message, sizeof(batch.message),
                          "Updated %zu sources, but the bounded Home artwork handoff could not be prepared.",
                          batch.refreshed);
            return batch;
        }
        batch.handoff_required = true;
        ++batch.registration_refreshes;
    }

    if (batch.handoff_required) {
        batch.complete = true;
        std::snprintf(batch.message, sizeof(batch.message),
                      "Updated %zu shortcuts; closing isShelf to refresh %zu PS5 Home registrations. Every destination was kept.",
                      batch.refreshed, batch.registration_refreshes);
        return batch;
    }

    // One explicit maintenance action completes the title-only refreshes and
    // registers any already-pending owned sources. Repeating the action safely
    // retries this single scan without touching current registration artwork.
    const auto scan = queue_scan(config_);
    if (scan != ConsoleTileStage::pending && scan != ConsoleTileStage::deferred) {
        batch.failed_stage = scan;
        std::snprintf(batch.message, sizeof(batch.message),
                      "Updated %zu installed shortcuts, but the PS5 Home artwork refresh could not be queued. Run Update all again.",
                      batch.refreshed);
        return batch;
    }

    batch.complete = true;
    if (batch.removing)
        std::snprintf(batch.message, sizeof(batch.message),
                      "Updated %zu shortcuts and refreshed %zu Home registrations; kept every destination. Close isShelf to finish. %zu removal is pending.",
                      batch.refreshed, batch.registration_refreshes,
                      batch.removing);
    else
        std::snprintf(batch.message, sizeof(batch.message),
                      "Updated %zu shortcuts and refreshed %zu PS5 Home registrations. Every destination was kept; close isShelf to finish.",
                      batch.refreshed, batch.registration_refreshes);
    return batch;
}

ConsoleTileRemovalBatchOutcome ConsoleTileManager::remove_installed() const {
    ConsoleTileRemovalBatchOutcome batch;
    if (!console_tile_writes_enabled) {
        batch.failed_stage = ConsoleTileStage::unavailable;
        std::snprintf(batch.message, sizeof(batch.message),
                      "PS5 Home Screen shortcut writes are still being tested.");
        return batch;
    }

    struct OwnedSource {
        std::size_t system = system_count;
        bool already_queued = false;
        bool registration_residue = false;
        char title_id[10]{};
    };
    std::array<OwnedSource, system_count> sources{};
    for (std::size_t system = 0; system < system_count; ++system) {
        const auto current = inspect(system);
        if (current.stage == ConsoleTileStage::absent) {
#ifndef SHELF_PS5
            char title_id[10]{};
            if (!console_tile_title_id(console_tile_preferred_number(system), title_id) ||
                !owned_registration_residue(config_, system, title_id))
                continue;
            auto& source = sources[batch.found++];
            source.system = system;
            source.registration_residue = true;
            std::snprintf(source.title_id, sizeof(source.title_id), "%s", title_id);
#endif
            continue;
        }
        const bool removable = current.stage == ConsoleTileStage::registered ||
                               current.stage == ConsoleTileStage::pending ||
                               current.stage == ConsoleTileStage::deferred ||
                               current.stage == ConsoleTileStage::removed_pending;
        if (!removable || !current.owned || !current.title_id[0] ||
            !console_tile_intent_valid(current.intent)) {
            batch.failed_system = system;
            batch.failed_stage = current.stage;
            std::snprintf(batch.message, sizeof(batch.message),
                          "Nothing changed: %.*s shortcut needs attention first.",
                          static_cast<int>(catalog[system].name.size()),
                          catalog[system].name.data());
            return batch;
        }
        auto& source = sources[batch.found++];
        source.system = system;
        source.already_queued = current.stage == ConsoleTileStage::removed_pending;
        std::snprintf(source.title_id, sizeof(source.title_id), "%s", current.title_id);
    }

    if (!batch.found) {
#ifdef SHELF_PS5
        // The signed app's mount view can report these global registration
        // roots absent even while ShellCore still owns them. Never infer
        // ownership here. Queue only the canonical preferred list and let the
        // post-close helper preflight every metadata/icon pair before its
        // first uninstall request.
        for (std::size_t system = 0; system < system_count; ++system) {
            char title_id[10]{};
            if (!console_tile_title_id(console_tile_preferred_number(system), title_id) ||
                !append_handoff_title(batch.handoff_titles,
                                      sizeof(batch.handoff_titles), title_id)) {
                batch.failed_system = system;
                batch.failed_stage = ConsoleTileStage::failed;
                std::snprintf(batch.message, sizeof(batch.message),
                              "The protected residual-registration list could not be prepared.");
                return batch;
            }
            batch.queued_systems[system] = true;
            ++batch.found;
            ++batch.queued;
        }
        batch.complete = true;
        batch.handoff_required = true;
        batch.residual_sweep = true;
        std::snprintf(batch.message, sizeof(batch.message),
                      "Prepared a protected check of all 52 isShelf title slots. Close isShelf so exact residual registrations can be removed.");
#else
        batch.complete = true;
        std::snprintf(batch.message, sizeof(batch.message),
                      "No isShelf Home Screen tiles are installed.");
#endif
        return batch;
    }

    for (std::size_t index = 0; index < batch.found; ++index) {
        const auto& source = sources[index];
#ifndef SHELF_PS5
        if (source.registration_residue) {
            if (!owned_registration_residue(config_, source.system,
                                             source.title_id)) {
                batch.failed_system = source.system;
                batch.failed_stage = ConsoleTileStage::conflict;
                batch.handoff_required = batch.queued != 0;
                std::snprintf(batch.message, sizeof(batch.message),
                              "Queued %zu of %zu tiles; an exact residual registration changed.",
                              batch.queued, batch.found);
                return batch;
            }
            if (!append_handoff_title(batch.handoff_titles,
                                      sizeof(batch.handoff_titles),
                                      source.title_id)) {
                batch.failed_system = source.system;
                batch.failed_stage = ConsoleTileStage::failed;
                batch.handoff_required = batch.queued != 0;
                std::snprintf(batch.message, sizeof(batch.message),
                              "Queued %zu of %zu tiles; the exact residual list is full.",
                              batch.queued, batch.found);
                return batch;
            }
            batch.queued_systems[source.system] = true;
            ++batch.queued;
            continue;
        }
#endif
        auto outcome = source.already_queued ? reconcile(source.system)
                                             : remove(source.system);
        if (source.already_queued)
            ++batch.already_queued;
        if (outcome.stage == ConsoleTileStage::removed_pending &&
            !outcome.handoff_required) {
            const auto confirmed = confirm_removed(source.system);
            if (confirmed.stage == ConsoleTileStage::absent)
                continue;
            outcome = confirmed;
        }
        if (outcome.stage != ConsoleTileStage::removed_pending ||
            !outcome.owned || !outcome.handoff_required ||
            std::strcmp(outcome.title_id, source.title_id) != 0 ||
            !append_handoff_title(batch.handoff_titles,
                                  sizeof(batch.handoff_titles),
                                  outcome.title_id)) {
            batch.failed_system = source.system;
            batch.failed_stage = outcome.stage;
            batch.handoff_required = batch.queued != 0;
            std::snprintf(batch.message, sizeof(batch.message),
                          "Queued %zu of %zu tiles; stopped at %.*s. Apply the queued removals, then try again.",
                          batch.queued, batch.found,
                          static_cast<int>(catalog[source.system].name.size()),
                          catalog[source.system].name.data());
            return batch;
        }
        batch.queued_systems[source.system] = true;
        ++batch.queued;
    }

    batch.complete = true;
    batch.handoff_required = batch.queued != 0;
    if (batch.queued)
        std::snprintf(batch.message, sizeof(batch.message),
                      "Queued %zu isShelf tiles. Close isShelf once so ShadowMount can update PS5 Home.",
                      batch.queued);
    else
        std::snprintf(batch.message, sizeof(batch.message),
                      "All isShelf Home Screen tiles were already absent.");
    return batch;
}

ConsoleTileOutcome ConsoleTileManager::remove(std::size_t system) const {
    if (!console_tile_writes_enabled)
        return outcome_for(system, ConsoleTileStage::unavailable,
                           "PS5 Home Screen shortcut writes are still being tested.");
    auto current = inspect(system);
    if (current.stage == ConsoleTileStage::absent || current.stage == ConsoleTileStage::removed_pending ||
        current.stage == ConsoleTileStage::conflict || current.stage == ConsoleTileStage::failed)
        return current;
    if (!current.owned || !current.title_id[0])
        return outcome_for(system, ConsoleTileStage::failed, "No owned shortcut can be removed.");
    const auto active = join(config_.homebrew_root, current.title_id);
    const auto manual = manual_source_state(config_, current.title_id, active);
    if (manual == ManualSourceState::failed)
        return outcome_for(system, ConsoleTileStage::unavailable,
                           "Could not verify ShadowMount manual-source tracking; shortcut was not changed.");
    if (manual == ManualSourceState::conflict)
        return outcome_for(system, ConsoleTileStage::conflict,
                           "ShadowMount tracks a different source for this title; shortcut was not changed.");
    if (manual == ManualSourceState::exact &&
        !remove_manual_source(config_, current.title_id, active))
        return outcome_for(system, ConsoleTileStage::unavailable,
                           "ShadowMount manual-source removal was not verified; shortcut was not changed.");
    if (path_kind(config_.retired_root) == PathKind::missing && !mkdir_exact(config_.retired_root, 0700))
        return outcome_for(system, ConsoleTileStage::failed, "Could not make the shortcut recovery folder.");
    if (path_kind(config_.retired_root) != PathKind::directory)
        return outcome_for(system, ConsoleTileStage::conflict, "Shortcut recovery path is occupied.");
    const auto retired = join(config_.retired_root, current.title_id);
    if (path_kind(retired) != PathKind::missing)
        return outcome_for(system, ConsoleTileStage::conflict, "Shortcut recovery path already exists.");
    if (std::rename(active.c_str(), retired.c_str()) != 0)
        return outcome_for(system, ConsoleTileStage::failed, "Could not isolate the owned shortcut source.");
    ConsoleTileIntent readback;
    if (!owned_folder(retired, current.title_id, system, readback))
        return outcome_for(system, ConsoleTileStage::conflict,
                           "Shortcut was isolated but recovery readback failed.");
    current.stage = ConsoleTileStage::removed_pending;
    set_message(current, "Shortcut source isolated; PS5 Home removal is queued.");
    const auto info = api_info(config_, current.title_id, {});
    if (info == ApiInfo::missing) {
        set_message(current, "PS5 registration is absent; finish removal to clean the recovery copy.");
        return current;
    }
    if (info == ApiInfo::registered || info == ApiInfo::pending) {
        current.handoff_required = true;
        set_message(current, "Removal queued. Close isShelf so ShadowMount can update PS5 Home.");
    } else
        set_message(current, "Shortcut source isolated; registration status could not be checked.");
    return current;
}

ConsoleTileOutcome ConsoleTileManager::reconcile(std::size_t system) const {
    if (!console_tile_writes_enabled)
        return outcome_for(system, ConsoleTileStage::unavailable,
                           "PS5 Home Screen shortcut writes are still being tested.");
    if (system >= system_count)
        return outcome_for(system, ConsoleTileStage::failed, "Unknown console identity.");
    if (!config_.retired_root)
        return outcome_for(system, ConsoleTileStage::unavailable, "Shortcut recovery path is unavailable.");
    const auto retired = find_owned(system, config_.retired_root, true);
    if (!retired.count) return inspect(system);
    if (retired.count > 1 || retired.corrupted)
        return outcome_for(system, ConsoleTileStage::conflict, "Retired shortcut ownership is ambiguous.");
    auto result = outcome_for(system, ConsoleTileStage::removed_pending,
                              "Shortcut removal is waiting for a final PS5 registration check.");
    result.owned = true;
    result.intent = retired.intent;
    std::snprintf(result.title_id, sizeof(result.title_id), "%s", retired.title_id);
    const auto info = api_info(config_, retired.title_id, {});
    if (info == ApiInfo::missing) {
        set_message(result, "PS5 registration is absent; finish removal to clean the recovery copy.");
        return result;
    }
    if (info == ApiInfo::registered || info == ApiInfo::pending) {
        result.handoff_required = true;
        set_message(result, "Removal is queued. Close isShelf so ShadowMount can update PS5 Home.");
    } else
        set_message(result, "Cannot verify whether the PS5 removed the shortcut yet.");
    return result;
}

ConsoleTileOutcome ConsoleTileManager::confirm_removed(std::size_t system) const {
    if (!console_tile_writes_enabled)
        return outcome_for(system, ConsoleTileStage::unavailable,
                           "PS5 Home Screen shortcut writes are still being tested.");
    auto result = reconcile(system);
    if (result.stage != ConsoleTileStage::removed_pending || !result.owned || !result.title_id[0])
        return result;
    if (api_info(config_, result.title_id, {}) != ApiInfo::missing) {
        set_message(result, "ShadowMount still lists this shortcut; recovery copy retained.");
        return result;
    }
    if (!registration_paths_absent(config_, result.title_id)) {
        set_message(result, "PS5 registration files remain; recovery copy retained.");
        return result;
    }
    const auto folder = join(config_.retired_root, result.title_id);
    ConsoleTileIntent readback;
    if (!owned_folder(folder, result.title_id, system, readback) ||
        !cleanup_folder(folder)) {
        result.stage = ConsoleTileStage::failed;
        set_message(result, "Shortcut is absent; owned files were cleaned, but unknown recovery data was retained.");
        return result;
    }
    return outcome_for(system, ConsoleTileStage::absent, "Confirmed shortcut removal; recovery copy cleaned.");
}

} // namespace shelf
