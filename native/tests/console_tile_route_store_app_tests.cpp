// SPDX-License-Identifier: GPL-3.0-only
// Host-only test of imported-call route storage. It never touches a PS5.
#define ISSHELF_APP_ROUTE_STORE_HOST_TEST 1
#include "shelf/console_tile_route_store_app.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if !defined(_WIN32)
namespace {
void check(bool value, const char* reason) {
    if (!value) { std::fprintf(stderr, "FAIL app route store: %s\n", reason); std::exit(1); }
}
}
int main() {
    using namespace shelf;
    using namespace shelf::tile_route;
    char parent[] = "/tmp/isshelf-app-route-XXXXXX";
    check(mkdtemp(parent) != nullptr, "private test parent created");
    const Source ps2{system_for("ps2")};
    const Source ps1{system_for("ps1")};
    const Preference library{ps2.system, ConsoleTileDestination::library, {}};
    const Preference home{ps2.system, ConsoleTileDestination::home, {}};
    const std::string root = std::string(parent) + "/" + app_route_directory;
    const std::string target = root + "/route-ps2.txt";
    const std::string duplicate_temp = target + ".33333333333333333333333333333333.tmp";
    {
        AppStore store(parent, geteuid(), 0700);
        Preference observed;
        check(store.open_root(false) == AppRoot::missing, "missing root is not unsafe");
        check(store.open_root(true) == AppRoot::present && store.identity_fresh(),
              "same instance creates private root");
        check(store.read(ps2, observed) == Input::absent, "missing record is absent");
        check(store.write(ps2, library, "11111111111111111111111111111111") == AppSave::saved,
              "library route saved");
        check(store.read(ps2, observed) == Input::valid &&
              observed.destination == ConsoleTileDestination::library,
              "library route read back");
        check(store.write(ps2, home, "22222222222222222222222222222222") == AppSave::saved,
              "same source changed to Home route");
        check(store.read(ps2, observed) == Input::valid &&
              observed.destination == ConsoleTileDestination::home,
              "Home route read back");
        check(store.read(ps1, observed) == Input::absent,
              "another console's route remains absent");
        check(store.write(ps2, home, "bad-nonce") == AppSave::refused,
              "malformed nonce refused");
        FILE* partial = std::fopen(duplicate_temp.c_str(), "wb");
        check(partial && std::fputs("partial", partial) >= 0 && std::fclose(partial) == 0,
              "duplicate temp fixture created");
        check(store.write(ps2, library, "33333333333333333333333333333333") == AppSave::refused,
              "duplicate nonce refused without retry");
        check(store.read(ps2, observed) == Input::valid &&
              observed.destination == ConsoleTileDestination::home,
              "duplicate nonce leaves committed route intact");
        store.test_fail_post_rename_sync_once();
        check(store.write(ps2, home, "66666666666666666666666666666666") == AppSave::unknown,
              "post-rename sync fault is unknown, not saved");
        check(store.write(ps2, library, "77777777777777777777777777777777") == AppSave::refused,
              "uncertain write latches refusal for the rest of the instance");
        check(store.read(ps2, observed) == Input::valid &&
              observed.destination == ConsoleTileDestination::home,
              "unknown result does not cause another route write");
    }
    char uncertain_parent[] = "/tmp/isshelf-app-route-ambiguous-XXXXXX";
    check(mkdtemp(uncertain_parent) != nullptr, "ambiguous-create test parent created");
    const std::string uncertain_root = std::string(uncertain_parent) + "/" + app_route_directory;
    {
        AppStore store(uncertain_parent, geteuid(), 0700);
        Preference observed;
        check(store.open_root(false) == AppRoot::missing, "ambiguous-create root starts missing");
        store.test_fail_post_mkdir_sync_once();
        check(store.open_root(true) == AppRoot::unsafe,
              "post-mkdir sync fault is unsafe, not present");
        check(store.open_root(false) == AppRoot::unsafe &&
              store.open_root(true) == AppRoot::unsafe,
              "unsafe create is terminal for this store instance");
        check(store.read(ps2, observed) == Input::invalid &&
              store.write(ps2, library, "88888888888888888888888888888888") == AppSave::refused,
              "ambiguous create cannot read or write through the same instance");
    }
    check(rmdir(uncertain_root.c_str()) == 0 && rmdir(uncertain_parent) == 0,
          "ambiguous-create exact test directories removed");
    check(chmod(parent, 0777) == 0, "parent mode changed");
    {
        AppStore store(parent, geteuid(), 0700);
        check(store.open_root(false) == AppRoot::unsafe,
              "wrong parent mode is unsafe, not missing");
    }
    check(chmod(parent, 0700) == 0, "parent mode restored");
    check(chmod(parent, 0777) == 0, "observed writable-parent mode installed");
    {
        AppStore store(parent, geteuid(), 0777);
        Preference observed;
        check(store.open_root(false) == AppRoot::present &&
              store.read(ps2, observed) == Input::valid &&
              observed.destination == ConsoleTileDestination::home,
              "route is readable under the observed writable-parent mode");
    }
    check(chmod(parent, 0700) == 0, "private test-parent mode restored");
    check(chmod(target.c_str(), 0644) == 0, "broad target mode installed");
    {
        AppStore store(parent, geteuid(), 0700);
        Preference observed;
        check(store.open_root(false) == AppRoot::present &&
              store.read(ps2, observed) == Input::invalid,
              "broad target mode rejected on read");
        check(store.write(ps2, home, "44444444444444444444444444444444") == AppSave::refused,
              "broad target mode refused on write");
    }
    check(chmod(target.c_str(), 0600) == 0, "target mode restored");
    {
        FILE* corrupt = std::fopen(target.c_str(), "wb");
        check(corrupt && std::fputs("invalid route record\n", corrupt) >= 0 &&
              std::fclose(corrupt) == 0, "corrupt record installed");
        AppStore store(parent, geteuid(), 0700);
        Preference observed;
        check(store.open_root(false) == AppRoot::present &&
              store.read(ps2, observed) == Input::invalid,
              "corrupt record is invalid, never absent");
    }
    const std::string detached = std::string(parent) + "/detached";
    {
        AppStore store(parent, geteuid(), 0700);
        check(store.open_root(false) == AppRoot::present, "held root opened");
        check(std::rename(root.c_str(), detached.c_str()) == 0, "root detached");
        check(symlink(detached.c_str(), root.c_str()) == 0, "root name replaced by symlink");
        AppStore other(parent, geteuid(), 0700);
        check(other.open_root(false) == AppRoot::unsafe,
              "symlinked root is unsafe");
        Preference observed;
        check(!store.identity_fresh() && store.read(ps2, observed) == Input::invalid,
              "detached held root cannot read");
        check(store.write(ps2, library, "55555555555555555555555555555555") == AppSave::refused,
              "detached held root cannot write");
    }
    check(unlink(root.c_str()) == 0, "test symlink removed without following");
    check(unlink((detached + "/route-ps2.txt").c_str()) == 0,
          "test route file removed");
    check(unlink((detached + "/route-ps2.txt.33333333333333333333333333333333.tmp").c_str()) == 0,
          "test duplicate temp removed");
    check(rmdir(detached.c_str()) == 0 && rmdir(parent) == 0,
          "exact test directories removed");
    std::puts("PASS: app route store tri-state, same-instance create, atomic route save, duplicate nonce, unsafe modes, and substitution.");
}
#endif
