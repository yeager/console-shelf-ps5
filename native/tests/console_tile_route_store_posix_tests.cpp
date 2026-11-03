// SPDX-License-Identifier: GPL-3.0-only
// Run on Linux with ISSHELF_PS1_WRITER_HOST_TEST so checked_at uses libc.
#include "shelf/console_tile_route_store_posix.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if !defined(_WIN32)
namespace {
void check(bool condition, const char* reason) {
    if (!condition) { std::fprintf(stderr, "FAIL POSIX route store: %s\n", reason); std::exit(1); }
}
} // namespace
int main() {
    using namespace shelf;
    using namespace shelf::tile_route;
    char parent[] = "/tmp/isshelf-route-store-XXXXXX";
    check(mkdtemp(parent) != nullptr, "test parent created");
    const Source source{system_for("ps1")};
    const Preference library{source.system, ConsoleTileDestination::library, {}};
    const Preference home{source.system, ConsoleTileDestination::home, {}};
    const std::string root = std::string(parent) + "/" + route_directory;
    const std::string target = root + "/route-ps1.txt";
    const std::string temp = target + ".33333333333333333333333333333333.tmp";
    {
        PosixStore store(parent, geteuid(), 0700);
        Preference observed;
        check(store.open_root(false) == Root::missing,
              "missing route root is distinct from unsafe and not created by read");
        check(store.open_root(true) == Root::present && store.identity_fresh(),
              "same instance can create and check 0700 root after a missing read");
        check(store.read(source, observed) == Input::absent, "missing preference is distinct");
        check(store.write(source, library, "11111111111111111111111111111111") == Save::saved,
              "first preference saved atomically");
        check(store.read(source, observed) == Input::valid &&
              observed.destination == ConsoleTileDestination::library,
              "first preference read back");
        check(store.write(source, home, "22222222222222222222222222222222") == Save::saved,
              "one source changes route without changing the source marker");
        check(store.read(source, observed) == Input::valid &&
              observed.destination == ConsoleTileDestination::home,
              "changed preference read back");
        check(store.write(source, home, "not-a-nonce") == Save::refused,
              "malformed nonce refused");
        FILE* partial = std::fopen(temp.c_str(), "wb");
        check(partial && std::fputs("partial", partial) >= 0 && std::fclose(partial) == 0,
              "test duplicate nonce temp created");
        check(store.write(source, library, "33333333333333333333333333333333") == Save::refused,
              "duplicate nonce is not reused or deleted");
        check(store.read(source, observed) == Input::valid &&
              observed.destination == ConsoleTileDestination::home,
              "duplicate nonce leaves committed route unchanged");
    }
    check(chmod(parent, 0777) == 0, "test parent mode changed");
    {
        PosixStore store(parent, geteuid(), 0700);
        check(store.open_root(false) == Root::unsafe,
              "wrong parent mode is unsafe rather than a missing preference");
    }
    check(chmod(parent, 0700) == 0, "test parent mode restored");
    check(chmod(target.c_str(), 0644) == 0, "test broad file mode installed");
    {
        PosixStore store(parent, geteuid(), 0700);
        Preference observed;
        check(store.open_root(false) == Root::present && store.read(source, observed) == Input::invalid,
              "a 0644 preference is not read");
        check(store.write(source, home, "44444444444444444444444444444444") == Save::refused,
              "unsafe existing target is not replaced");
    }
    check(chmod(target.c_str(), 0600) == 0, "test file mode restored");
    const std::string detached = std::string(parent) + "/detached";
    {
        PosixStore store(parent, geteuid(), 0700);
        check(store.open_root(false) == Root::present, "held root reopened");
        check(std::rename(root.c_str(), detached.c_str()) == 0, "test root detached");
        check(symlink(detached.c_str(), root.c_str()) == 0, "test symlink substituted");
        PosixStore competing(parent, geteuid(), 0700);
        check(competing.open_root(false) == Root::unsafe,
              "substituted root is unsafe rather than missing");
        Preference observed;
        check(!store.identity_fresh() && store.read(source, observed) == Input::invalid,
              "named/held root mismatch refuses a read");
        check(store.write(source, library, "55555555555555555555555555555555") == Save::refused,
              "named/held root mismatch refuses a write");
    }
    check(unlink(root.c_str()) == 0 && unlink(target.c_str()) != 0,
          "test symlink removed without following it");
    const std::string detached_target = detached + "/route-ps1.txt";
    const std::string detached_temp = detached + "/route-ps1.txt.33333333333333333333333333333333.tmp";
    check(unlink(detached_target.c_str()) == 0 && unlink(detached_temp.c_str()) == 0 &&
          rmdir(detached.c_str()) == 0 && rmdir(parent) == 0,
          "test tree removed by exact paths");
    std::puts("PASS: route root tri-state, same-instance create, parent mode, atomic change, duplicate nonce, unsafe file, and directory substitution.");
}
#endif
