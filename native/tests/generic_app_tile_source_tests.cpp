// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/generic_app_tile_source.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace shelf;
using namespace shelf::generic_app_tile_source;

static std::string read_file(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    assert(file);
    std::string bytes;
    char block[65536];
    for (std::size_t count; (count = std::fread(block, 1, sizeof(block), file)) != 0;)
        bytes.append(block, count);
    assert(!std::ferror(file) && std::fclose(file) == 0);
    return bytes;
}

static std::string source_path(const char* root, const char* nonce,
                               const char* title) {
    return std::string(root) + "/" + folder_prefix + nonce + "/" + title;
}

static std::string sha256_hex(const std::string& input) {
    isshelf_badge::Sha256 sha;
    sha.update(reinterpret_cast<const unsigned char*>(input.data()), input.size());
    const auto bytes = sha.finish();
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned char byte : bytes) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

static void verify_source(const char* root, const char* nonce,
                          const ConsoleTileIntent& intent, const Result& result) {
    const auto planned = plan(intent);
    assert(planned.valid && result.attempted && result.complete && result.receipt_bytes);
    const std::string title = source_path(root, nonce, planned.title_id);
    const auto& pin = isshelf_badge::generic_tile::tiles[intent.system];
    const std::string report = std::string(root) + "/" + receipt_prefix + nonce + ".txt";
    const auto receipt = read_file(report);
    assert(receipt.size() == result.receipt_bytes);
    assert(app_tile_source_probe::fnv(reinterpret_cast<const unsigned char*>(receipt.data()),
                                      receipt.size()) == result.receipt_fnv);
    assert(receipt.find(std::string("title=") + planned.title_id + "\n") != std::string::npos);
    assert(receipt.find(std::string("system=") + planned.system_id + "\n") != std::string::npos);
    assert(receipt.find("state=source-only\n") != std::string::npos);
    assert(receipt.find("files=7\n") != std::string::npos);
    assert(receipt.find(std::string("pack_sha256=") +
        isshelf_badge::generic_tile::pack_sha256 + "\n") != std::string::npos);
    assert(receipt.find(std::string("forwarder_sha256=") +
        sha256_hex(std::string(planned.forwarder, planned.forwarder_bytes)) + "\n") !=
        std::string::npos);
    assert(receipt.find(std::string("destination=") +
        std::string(console_tile_destination_name(intent.destination)) + "\n") != std::string::npos);
    for (std::size_t index = 0; index < file_count; ++index) {
        const std::string path = title + "/" + pin.files[index].path;
        const auto file = read_file(path);
        if (index == 4) {
            assert(file == std::string(planned.forwarder, planned.forwarder_bytes));
            if (intent.destination == ConsoleTileDestination::home) {
                char fixed_home[console_tile_message_capacity + 1]{};
                assert(encode_console_tile_fixed_home_arg(intent.system,
                    fixed_home, sizeof(fixed_home)));
                assert(file.find(std::string("\"args\":[\"") + planned.launch_arg +
                    "\",\"" + fixed_home + "\"]") != std::string::npos);
            } else
                assert(file.find(std::string("\"args\":[\"") + planned.launch_arg + "\"]") !=
                       std::string::npos);
        } else {
            assert(file.size() == pin.files[index].bytes);
            assert(hex_digest(reinterpret_cast<const unsigned char*>(file.data()),
                              file.size(), pin.files[index].sha256));
        }
        struct stat info{};
        assert(::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
               (info.st_mode & 07777) == (index < 2 ? 0755 : 0644));
    }
    for (const auto& directory : {std::string(root) + "/" + folder_prefix + nonce,
                                  title, title + "/sce_module", title + "/sce_sys"}) {
        struct stat info{};
        assert(::lstat(directory.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
               (info.st_mode & 07777) == 0755);
    }
}

int main(int argc, char** argv) {
    assert(argc == 2);
    static_assert(isshelf_badge::generic_tile::tile_count == system_count);
    constexpr char nonce[] = "0123456789abcdef0123456789abcdef";
    constexpr char second[] = "11111111111111111111111111111111";
    constexpr char third[] = "22222222222222222222222222222222";
    constexpr char fourth[] = "33333333333333333333333333333333";
    char root[] = "/tmp/isshelf-generic-source-XXXXXX";
    assert(::mkdtemp(root));

    std::set<std::string> titles;
    std::size_t eligible = 0;
    for (std::size_t system = 0; system < system_count; ++system) {
        const auto& pin = isshelf_badge::generic_tile::tiles[system];
        assert(titles.insert(pin.title_id).second);
        assert(tile_job::catalog_pin_matches(system));
        ConsoleTileIntent intent{system, ConsoleTileDestination::library, {}};
        const auto candidate = plan(intent);
        if (reserved_existing_title(system)) {
            assert(!candidate.valid && std::strcmp(candidate.failure, "existing-title") == 0);
            continue;
        }
        ++eligible;
        assert(candidate.valid && std::strcmp(candidate.title_id, pin.title_id) == 0);
        assert(std::strstr(candidate.forwarder, candidate.launch_arg));
        intent.destination = ConsoleTileDestination::home;
        const auto home = plan(intent);
        assert(home.valid && std::strcmp(home.title_id, candidate.title_id) == 0);
        assert(std::strstr(home.forwarder, ",home,-"));
        assert(std::strstr(home.forwarder, "--isshelf-home-direct-v2="));
        for (const auto& menu : isshelf_badge::generic_tile::menus) {
            if (menu.system != system) continue;
            intent.destination = ConsoleTileDestination::emulator;
            std::strcpy(intent.emulator_title, menu.title_id);
            const auto emulator = plan(intent);
            if (console_tile_emulator_menu_selects_system(menu.title_id))
                assert(emulator.valid && std::strstr(emulator.forwarder, menu.title_id));
            else
                assert(!emulator.valid);
        }
    }
    assert(eligible == 48 && titles.size() == system_count);
    ConsoleTileIntent invalid{};
    assert(!plan(invalid).valid);
    ConsoleTileIntent ps5{0, ConsoleTileDestination::home, {}};
    ConsoleTileIntent switch_library{7, ConsoleTileDestination::library, {}};
    ConsoleTileIntent wiiu_emulator{8, ConsoleTileDestination::emulator, {}};
    std::strcpy(wiiu_emulator.emulator_title, "PPSA99360");
    assert(plan(wiiu_emulator).valid);
    ConsoleTileIntent wrong_emulator = wiiu_emulator;
    std::strcpy(wrong_emulator.emulator_title, "PPSA99064");
    assert(!plan(wrong_emulator).valid);
    ConsoleTileIntent legacy_retroarch{5, ConsoleTileDestination::emulator, {}};
    std::strcpy(legacy_retroarch.emulator_title, "PPSA99169");
    assert(!plan(legacy_retroarch).valid);
    const auto no_nonce = run_once(root, argv[1], "invalid", ps5);
    assert(!no_nonce.attempted && std::strcmp(no_nonce.failure, "input") == 0);
    const auto adopted = run_once(root, argv[1], nonce,
        ConsoleTileIntent{4, ConsoleTileDestination::library, {}});
    assert(!adopted.attempted && std::strcmp(adopted.failure, "existing-title") == 0);
    for (std::size_t system : {2u, 3u, 11u}) {
        const auto published = run_once(root, argv[1], nonce,
            ConsoleTileIntent{system, ConsoleTileDestination::library, {}});
        assert(!published.attempted && std::strcmp(published.failure, "existing-title") == 0);
    }
    const auto first = run_once(root, argv[1], nonce, ps5);
    verify_source(root, nonce, ps5, first);
    const auto repeat = run_once(root, argv[1], nonce, ps5);
    assert(!repeat.attempted && std::strcmp(repeat.failure, "collision") == 0);
    const auto library = run_once(root, argv[1], second, switch_library);
    verify_source(root, second, switch_library, library);
    const auto emulator = run_once(root, argv[1], third, wiiu_emulator);
    verify_source(root, third, wiiu_emulator, emulator);
    const std::string alias = std::string(root) + "-alias";
    assert(::symlink(root, alias.c_str()) == 0);
    const auto redirected = run_once(alias.c_str(), argv[1], fourth, ps5);
    assert(!redirected.attempted && std::strcmp(redirected.failure, "root-open") == 0);
    const std::string changed = std::string(root) + "/changed-pack.bin";
    {
        FILE* file = std::fopen(changed.c_str(), "wb");
        assert(file && std::fwrite("changed", 1, 7, file) == 7 && std::fclose(file) == 0);
    }
    const auto rejected = run_once(root, changed.c_str(), fourth, ps5);
    assert(!rejected.attempted && std::strcmp(rejected.failure, "pack-check") == 0);

    for (const auto& item : {std::pair{nonce, ps5}, std::pair{second, switch_library},
                             std::pair{third, wiiu_emulator}}) {
        const auto planned = plan(item.second);
        const auto& pin = isshelf_badge::generic_tile::tiles[item.second.system];
        const auto title = source_path(root, item.first, planned.title_id);
        for (std::size_t index = 0; index < file_count; ++index)
            assert(::unlink((title + "/" + pin.files[index].path).c_str()) == 0);
        assert(::rmdir((title + "/sce_module").c_str()) == 0);
        assert(::rmdir((title + "/sce_sys").c_str()) == 0);
        assert(::rmdir(title.c_str()) == 0);
        assert(::rmdir((std::string(root) + "/" + folder_prefix + item.first).c_str()) == 0);
        assert(::unlink((std::string(root) + "/" + receipt_prefix + item.first + ".txt").c_str()) == 0);
    }
    assert(::unlink(alias.c_str()) == 0 && ::unlink(changed.c_str()) == 0 &&
           ::rmdir(root) == 0);
    std::puts("PASS: 48 unused titles planned; PS5 Home, Switch Library, Wii U Emulator seven-file sources exact; replay and redirect rejected");
}
