// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/ui_test_remote_arm.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace {
constexpr char good_nonce[] = "95b4d7ac0f3182e6d9c5b7234a1e8f60";

shelf::UiTestRemoteArmInput parse(std::initializer_list<const char*> values,
                                  shelf::ConsoleTileInput tile = shelf::ConsoleTileInput::absent,
                                  bool marker = false) {
    std::vector<std::string> owned(values.begin(), values.end());
    std::vector<char*> args;
    for (auto& value : owned)
        args.push_back(value.data());
    char nonce[33]{};
    const auto outcome = shelf::parse_ui_test_remote_arm(
        static_cast<int>(args.size()), args.data(), tile, marker, nonce);
    assert((outcome == shelf::UiTestRemoteArmInput::valid) ==
           (std::string(nonce) == good_nonce));
    return outcome;
}

bool title_file(const char* raw) {
    const auto path = std::filesystem::temp_directory_path() /
        ("isshelf-ui-arm-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".json");
    {
        std::ofstream file(path, std::ios::binary);
        assert(file);
        file << raw;
        assert(file);
    }
    const bool okay = shelf::ui_test_main_title_param(path.string().c_str());
    assert(std::filesystem::remove(path));
    return okay;
}
} // namespace

int main(int argc, char** argv) {
    assert(argc == 1 || argc == 2);
    if (argc == 2)
        assert(shelf::ui_test_main_title_param(argv[1]));
    using shelf::ConsoleTileInput;
    using shelf::UiTestRemoteArmInput;
    assert(parse({}) == UiTestRemoteArmInput::absent);
    assert(parse({"eboot.bin"}) == UiTestRemoteArmInput::absent);
    assert(parse({"--isshelf-ui-test-arm=95b4d7ac0f3182e6d9c5b7234a1e8f60"}) ==
           UiTestRemoteArmInput::valid); // PS5 CRT may put the first argument at argv[0].
    assert(parse({"eboot.bin", "--isshelf-ui-test-arm=95b4d7ac0f3182e6d9c5b7234a1e8f60"}) ==
           UiTestRemoteArmInput::valid);
    assert(parse({"/app0/eboot.bin", "--isshelf-ui-test-arm=95b4d7ac0f3182e6d9c5b7234a1e8f60"}) ==
           UiTestRemoteArmInput::valid);
    assert(parse({"", "--isshelf-ui-test-arm=95b4d7ac0f3182e6d9c5b7234a1e8f60"}) ==
           UiTestRemoteArmInput::valid); // Observed PS5 argv shape on UI46.
    assert(parse({"", "--isshelf-ui-test-arm=95b4d7ac0f3182e6d9c5b7234a1e8f60", "extra"}) ==
           UiTestRemoteArmInput::invalid);
    assert(parse({"--other", "--isshelf-ui-test-arm=95b4d7ac0f3182e6d9c5b7234a1e8f60"}) ==
           UiTestRemoteArmInput::invalid);
    assert(parse({"--isshelf-ui-test-arm=95b4d7ac0f3182e6d9c5b7234a1e8f60", "extra"}) ==
           UiTestRemoteArmInput::invalid);
    assert(parse({"eboot.bin", "--isshelf-ui-test-arm=95b4d7ac0f3182e6d9c5b7234a1e8f60", "extra"}) ==
           UiTestRemoteArmInput::invalid);
    std::string another = "--isshelf-ui-test-arm=11111111111111111111111111111111";
    char* another_argv[] = {another.data()};
    char another_nonce[33]{};
    assert(shelf::parse_ui_test_remote_arm(1, another_argv, ConsoleTileInput::absent,
                                           false, another_nonce) == UiTestRemoteArmInput::valid);
    assert(std::string(another_nonce) == "11111111111111111111111111111111");
    assert(parse({"--isshelf-ui-test-arm=95b4d7ac0f3182e6d9c5b7234a1e8f60"},
                 ConsoleTileInput::valid) == UiTestRemoteArmInput::invalid);
    constexpr char fixed_ps2[] = "--isshelf-console-tile=ps2,home,-";
    constexpr char arm[] = "--isshelf-ui-test-arm=95b4d7ac0f3182e6d9c5b7234a1e8f60";
    assert(parse({arm, fixed_ps2}, ConsoleTileInput::valid) == UiTestRemoteArmInput::valid);
    assert(parse({"", arm, fixed_ps2}, ConsoleTileInput::valid) == UiTestRemoteArmInput::valid);
    assert(parse({"eboot.bin", arm, fixed_ps2}, ConsoleTileInput::valid) == UiTestRemoteArmInput::valid);
    assert(parse({fixed_ps2, arm}, ConsoleTileInput::valid) == UiTestRemoteArmInput::invalid);
    assert(parse({arm, fixed_ps2, fixed_ps2}, ConsoleTileInput::valid) == UiTestRemoteArmInput::invalid);
    assert(parse({"", arm, fixed_ps2, "extra"}, ConsoleTileInput::valid) == UiTestRemoteArmInput::invalid);
    assert(parse({arm, "--isshelf-console-tile=ps1,home,-"}, ConsoleTileInput::valid) ==
           UiTestRemoteArmInput::invalid);
    assert(parse({arm, "--isshelf-console-tile=ps2,library,-"}, ConsoleTileInput::valid) ==
           UiTestRemoteArmInput::invalid);
    assert(parse({arm, fixed_ps2}, ConsoleTileInput::absent) == UiTestRemoteArmInput::invalid);
    assert(parse({arm, fixed_ps2}, ConsoleTileInput::valid, true) == UiTestRemoteArmInput::invalid);
    assert(parse({"--isshelf-ui-test-arm=95b4d7ac0f3182e6d9c5b7234a1e8f60"},
                 ConsoleTileInput::absent, true) == UiTestRemoteArmInput::invalid);
    assert(parse({"--isshelf-ui-test-arm=95b4d7ac0f3182e6d9c5b7234a1e8f60",
                  "--isshelf-ui-test-arm=95b4d7ac0f3182e6d9c5b7234a1e8f60"}) ==
           UiTestRemoteArmInput::invalid);
    assert(parse({"--isshelf-ui-test-arm="}) == UiTestRemoteArmInput::invalid);
    assert(parse({"--isshelf-ui-test-arm=95b4d7ac0f3182e6d9c5b7234a1e8f6"}) ==
           UiTestRemoteArmInput::invalid);
    assert(parse({"--isshelf-ui-test-arm=95b4d7ac0f3182e6d9c5b7234a1e8f600"}) ==
           UiTestRemoteArmInput::invalid);
    assert(parse({"--isshelf-ui-test-arm=95B4d7ac0f3182e6d9c5b7234a1e8f60"}) ==
           UiTestRemoteArmInput::invalid);
    assert(parse({"--isshelf-ui-test-arm=95b4d7ac0f3182e6d9c5b7234a1e8f6g"}) ==
           UiTestRemoteArmInput::invalid);
    assert(parse({"--isshelf-ui-test-arm=00000000000000000000000000000000"}) ==
           UiTestRemoteArmInput::invalid);
    assert(parse({"--isshelf-ui-test-arm:95b4d7ac0f3182e6d9c5b7234a1e8f60"}) ==
           UiTestRemoteArmInput::invalid);
    char nonce[33]{};
    assert(shelf::parse_ui_test_remote_arm(1, nullptr, ConsoleTileInput::absent, false, nonce) ==
           UiTestRemoteArmInput::invalid);

    constexpr char main_param[] =
        "{\"titleId\":\"PPSA99541\",\"contentId\":\"UP9000-PPSA99541_00-CONSOLESHELF0001\"}\n";
    assert(title_file(main_param));
    assert(!title_file("{\"titleId\":\"PPSA99703\",\"contentId\":\"UP9000-PPSA99541_00-CONSOLESHELF0001\"}"));
    assert(!title_file("{\"titleId\":\"PPSA99541\",\"contentId\":\"WRONG\"}"));
    assert(!title_file("{\"titleId\":\"PPSA99541\",\"titleId\":\"PPSA99541\","
                       "\"contentId\":\"UP9000-PPSA99541_00-CONSOLESHELF0001\"}"));
    assert(!title_file("{\"titleId\":\"PPSA99541\","));
    assert(!shelf::ui_test_main_title_param("/definitely-absent-isShelf-remote-arm-param.json"));
    const std::string too_large(8193, 'x');
    assert(!title_file(too_large.c_str()));
}
