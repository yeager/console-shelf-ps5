// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "launch_protocol.hpp"
#include <cstdio>

namespace shelf {
// Runs only in the already-bundled post-close helper. The result is written to
// the app-owned update folder so the relaunched app can explain whether the
// exact transaction committed, rolled back, or still needs attention.
int apply_update_transaction(const LaunchRequest& request, FILE* log);
} // namespace shelf
