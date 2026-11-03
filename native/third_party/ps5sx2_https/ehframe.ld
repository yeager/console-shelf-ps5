/* SPDX-License-Identifier: GPL-3.0-or-later
 * Adapted from Swordpdf/PS5SX2's PS5 linker augmentation. The pinned PS5
 * runtime script omits these bounds, while the linked libc++/libunwind
 * runtime requires them even though isShelf disables C++ exceptions. */
PROVIDE(__eh_frame_start = ADDR(.eh_frame));
PROVIDE(__eh_frame_end = ADDR(.eh_frame) + SIZEOF(.eh_frame));
PROVIDE(__eh_frame_hdr_start = ADDR(.eh_frame_hdr));
PROVIDE(__eh_frame_hdr_end = ADDR(.eh_frame_hdr) + SIZEOF(.eh_frame_hdr));
