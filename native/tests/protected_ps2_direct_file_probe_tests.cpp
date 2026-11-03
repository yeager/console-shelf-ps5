// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/protected_ps2_direct_file_probe.hpp"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace shelf::protected_ps2_direct_file_probe;

static constexpr char param_body[] =
    R"JSON({"ageLevel":{"default":0},"applicationCategoryType":0,"applicationDrmType":"free","attribute":0,"attribute2":0,"attribute3":0,"conceptId":"99703","contentBadgeType":1,"contentId":"UP9000-PPSA99703_00-PS5FORWARDER0000","contentVersion":"01.000.000","downloadDataSize":256,"gameIntent":{"permittedIntents":[{"intentType":"launchActivity"}]},"localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"PlayStation 2"}},"masterVersion":"01.00","pubtools":{"creationDate":"2026-08-22 00:00:00","loudnessSnd0":"-28.00","toolVersion":"2.00"},"requiredSystemSoftwareVersion":"0x0000000000000000","sdkVersion":"0x0000000000000000","titleId":"PPSA99703","versionFileUri":""})JSON"
    "\n";

static void make_dir(const std::string& path) { assert(::mkdir(path.c_str(), 0755) == 0); }

static void write_file(const std::string& path, const char* bytes, std::size_t size) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    assert(fd >= 0);
    assert(::write(fd, bytes, size) == static_cast<ssize_t>(size));
    assert(::close(fd) == 0);
}

int main() {
    static_assert(sizeof(param_body) - 1 == expected_bytes);
    assert(shelf::tile_job::detail::exact_helper_sha256(
        reinterpret_cast<const unsigned char*>(param_body), expected_bytes,
        expected_bytes, expected_sha256));
    char root_chars[] = "/tmp/isshelf-ps2-direct-XXXXXX";
    assert(::mkdtemp(root_chars) != nullptr);
    const std::string root(root_chars);
    const std::string system = root + "/system_data";
    const std::string tiles = system + "/isshelf-tiles";
    const std::string stage = tiles + "/.ps2-pilot-745991606ec7";
    const std::string title = stage + "/PPSA99703";
    const std::string sys = title + "/sce_sys";
    const std::string target = sys + "/param.json";
    make_dir(system);
    make_dir(tiles);
    make_dir(stage);
    make_dir(title);
    make_dir(sys);

    const auto missing = inspect(root.c_str());
    assert(!missing.complete && !missing.target_opened &&
           std::strcmp(missing.failure, "open-target") == 0 && missing.system_error == ENOENT);
    write_file(target, param_body, expected_bytes);
    const auto alias = inspect(root.c_str());
    assert(!alias.complete && alias.target_opened && alias.content_matched &&
           alias.bytes_read == expected_bytes && !alias.mount_distinct &&
           std::strcmp(alias.failure, "mount-alias") == 0);

    assert(::unlink(target.c_str()) == 0);
    const std::string linked = target + ".saved";
    write_file(linked, param_body, expected_bytes);
    assert(::symlink(linked.c_str(), target.c_str()) == 0);
    const auto symlink = inspect(root.c_str());
    assert(!symlink.complete && !symlink.target_opened &&
           std::strcmp(symlink.failure, "open-target") == 0 && symlink.system_error == ELOOP);
    assert(::unlink(target.c_str()) == 0);
    assert(::unlink(linked.c_str()) == 0);

    write_file(target, "bad", 3);
    const auto short_file = inspect(root.c_str());
    assert(!short_file.complete && short_file.target_opened &&
           std::strcmp(short_file.failure, "file-bound") == 0 && short_file.bytes_read == 0);
    assert(::unlink(target.c_str()) == 0);
    char altered[expected_bytes];
    std::memcpy(altered, param_body, expected_bytes);
    altered[50] ^= 1;
    write_file(target, altered, sizeof(altered));
    const auto mismatch = inspect(root.c_str());
    assert(!mismatch.complete && mismatch.target_opened && !mismatch.content_matched &&
           mismatch.bytes_read == expected_bytes &&
           std::strcmp(mismatch.failure, "content-mismatch") == 0);

    assert(::unlink(target.c_str()) == 0);
    assert(::rmdir(sys.c_str()) == 0);
    assert(::rmdir(title.c_str()) == 0);
    assert(::rmdir(stage.c_str()) == 0);
    assert(::rmdir(tiles.c_str()) == 0);
    assert(::rmdir(system.c_str()) == 0);
    assert(::rmdir(root.c_str()) == 0);
}
