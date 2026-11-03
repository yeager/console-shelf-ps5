// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/update_helper.hpp"
#include <cstdio>
#include <cstring>

namespace {
struct NoticeAudit {
    const char* tag = nullptr;
    unsigned seen = 0;
    bool invalid = false;
};

void notice(void* raw, shelf::UpdateTransactionStage stage, const char* tag) {
    auto& audit = *static_cast<NoticeAudit*>(raw);
    const auto bit = 1u << static_cast<unsigned>(stage);
    audit.invalid = audit.invalid || !tag || std::strcmp(tag, audit.tag) != 0;
    audit.seen |= bit;
}
} // namespace

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
    NoticeAudit audit{request.rom};
    const int result = shelf::apply_update_transaction(request, stderr, notice, &audit);
    const unsigned verifying = 1u << static_cast<unsigned>(shelf::UpdateTransactionStage::verifying);
    const unsigned finishing = 1u << static_cast<unsigned>(shelf::UpdateTransactionStage::finishing);
    if (audit.invalid || (audit.seen & verifying) == 0 || (result >= 0 && (audit.seen & finishing) == 0)) {
        std::fprintf(stderr, "invalid updater notification sequence: result=%d stages=0x%x\n",
                     result, audit.seen);
        return 3;
    }
    std::printf("%d\n", result);
    return 0;
}
