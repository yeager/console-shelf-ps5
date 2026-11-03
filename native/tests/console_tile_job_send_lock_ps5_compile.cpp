// SPDX-License-Identifier: GPL-3.0-only
// Target ABI check only. This object is never loaded on the console.
#ifndef SHELF_PS5
#error "The sender lock compile probe requires the PS5 target"
#endif
#include "shelf/console_tile_job_posix.hpp"

extern "C" bool isshelf_sender_lock_ps5_compile() {
    shelf::tile_job::PosixJobStore store;
    return store.open_root(false) && store.acquire_send_lock() &&
           store.identity_fresh();
}
