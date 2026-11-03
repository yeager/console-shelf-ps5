// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/update_helper.hpp"
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    shelf::LaunchRequest request;
    if (argc != 3 || std::strlen(argv[1]) >= sizeof(request.rom) ||
        std::strlen(argv[2]) != 64) {
        std::fprintf(stderr, "usage: update-helper-test TAG MANIFEST_SHA256\n");
        return 2;
    }
    request.mode = shelf::LaunchMode::update_recover;
    std::snprintf(request.title, sizeof(request.title), "%s", shelf::shelf_title_id);
    std::snprintf(request.rom, sizeof(request.rom), "%s", argv[1]);
    std::snprintf(request.core, sizeof(request.core), "%s", argv[2]);
    const int result = shelf::apply_update_transaction(request, stderr);
    std::printf("%d\n", result);
    return 0;
}
