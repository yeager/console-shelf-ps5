// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "launch_protocol.hpp"
#include <cstdio>

namespace shelf {
enum class UpdateTransactionStage {
    verifying,
    recovering,
    installing,
    restoring,
    finishing,
};
using UpdateTransactionNotifier = void (*)(void*, UpdateTransactionStage, const char* tag);

// Runs only in the already-bundled post-close helper. The result is written to
// the app-owned update folder so the relaunched app can explain whether the
// exact transaction committed, rolled back, or still needs attention.
int apply_update_transaction(const LaunchRequest& request, FILE* log,
                             UpdateTransactionNotifier notifier = nullptr,
                             void* notifier_context = nullptr);
} // namespace shelf
