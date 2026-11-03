// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/sha256.hpp"
#include "shelf/update_protocol.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
void update_check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL updater: %s\n", message);
        std::exit(1);
    }
}
} // namespace

void test_update() {
    using namespace shelf;
    unsigned parts[3]{};
    update_check(update_semver("v0.1.0", parts) && parts[0] == 0 && parts[1] == 1 && parts[2] == 0 &&
                     compare_update_versions("v0.1.0", "v0.2.0") < 0 &&
                     compare_update_versions("v2.0.0", "v1.99.99") > 0 &&
                     compare_update_versions("v1.2.3", "v1.2.3") == 0 &&
                     !update_semver("1.2.3", parts) && !update_semver("v01.2.3", parts) &&
                     !update_semver("v1.2.3-beta", parts),
                 "only canonical vMAJOR.MINOR.PATCH production tags compare");

    constexpr char digest[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char* release_json =
        "{\"tag_name\":\"v0.2.0\",\"name\":\"isShelf v0.2.0\",\"draft\":false,"
        "\"prerelease\":false,\"assets\":[{\"name\":\"PPSA99541.zip\",\"state\":\"uploaded\","
        "\"size\":150000000,\"digest\":\"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"browser_download_url\":\"https://github.com/Iswebro/console-shelf-ps5/releases/download/v0.2.0/PPSA99541.zip\"}]}";
    UpdateRelease release;
    char error[192]{};
    update_check(parse_update_release(release_json, "v0.1.0", release, error, sizeof(error)) ==
                         UpdateDecision::available &&
                     std::strcmp(release.tag, "v0.2.0") == 0 && release.bytes == 150000000 &&
                     std::strcmp(release.sha256, digest) == 0,
                 "the exact official production ZIP with GitHub's SHA-256 is accepted");
    update_check(parse_update_release(release_json, "v0.2.0", release, error, sizeof(error)) ==
                     UpdateDecision::current,
                 "the same release is current");
    update_check(parse_update_release(release_json, "v0.3.0", release, error, sizeof(error)) ==
                     UpdateDecision::newer,
                 "a local test build is not downgraded");
    const char* wrong_url =
        "{\"tag_name\":\"v0.2.0\",\"name\":\"isShelf\",\"draft\":false,\"prerelease\":false,"
        "\"assets\":[{\"name\":\"PPSA99541.zip\",\"state\":\"uploaded\",\"size\":150000000,"
        "\"digest\":\"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"browser_download_url\":\"https://example.com/PPSA99541.zip\"}]}";
    update_check(parse_update_release(wrong_url, "v0.1.0", release, error, sizeof(error)) ==
                     UpdateDecision::invalid,
                 "an asset outside the fixed repository is rejected");
    const char* duplicate_key =
        "{\"tag_name\":\"v0.2.0\",\"tag_name\":\"v9.0.0\",\"draft\":false,"
        "\"prerelease\":false,\"assets\":[]}";
    update_check(parse_update_release(duplicate_key, "v0.1.0", release, error, sizeof(error)) ==
                     UpdateDecision::invalid,
                 "duplicate JSON members cannot replace the reviewed release identity");

    UpdateManifest manifest;
    UpdateManifestEntry entries[4]{};
    const std::string_view manifest_text =
        "isshelf-update 1\n"
        "tag=v0.2.0\n"
        "count=3\n"
        "file=assets/icon.png\tbytes=12\tsha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
        "file=eboot.bin\tbytes=1234\tsha256=1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
        "file=sce_sys/param.json\tbytes=456\tsha256=2123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
    update_check(parse_update_manifest(manifest_text, manifest, entries, 4) && manifest.count == 3 &&
                     manifest.total_bytes == 1702 && std::strcmp(entries[1].path, "eboot.bin") == 0,
                 "the bounded staged-file manifest is accepted");
    auto unsafe = std::string(manifest_text);
    unsafe.replace(unsafe.find("assets/icon.png"), std::strlen("assets/icon.png"), "assets/../x.png");
    update_check(!parse_update_manifest(unsafe, manifest, entries, 4),
                 "manifest traversal is rejected before any program file can change");
    for (const char* personal : {"settings.ini", "emulators/PS5SX2/config.ini",
                                 "games/title.iso", "roms/title.bin", "bios/firmware.bin",
                                 "saves/card.bin", "artwork/custom.png", "keys/account.key",
                                 "userdata/profile.json"}) {
        auto protected_manifest = std::string(manifest_text);
        protected_manifest.replace(protected_manifest.find("assets/icon.png"),
                                   std::strlen("assets/icon.png"), personal);
        update_check(!parse_update_manifest(protected_manifest, manifest, entries, 4),
                     "user settings, emulators, games and saves cannot become update targets");
    }
    update_check(update_program_path("assets/3d-icons/ps2.png") &&
                     update_program_path("native/assets/fonts/Poppins-Regular.ttf") &&
                     update_program_path("sce_module/libc.prx") &&
                     !update_program_path("assets") && !update_program_path("games/title.iso"),
                 "only exact packaged program roots are writable by the updater");

    Sha256 hash;
    hash.update("abc", 3);
    char sha[65];
    sha256_hex(hash.finish(), sha);
    update_check(std::strcmp(sha, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
                 "the updater/helper SHA-256 implementation matches the standard vector");
    std::puts("PASS: strict production-release identity, semantic versioning, staged manifest and SHA-256.");
}
