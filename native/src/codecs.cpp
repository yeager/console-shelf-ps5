// SPDX-License-Identifier: GPL-3.0-only
// Bundled, pinned stb sources retain their embedded MIT/Unlicense notices.
#include "shelf/memory.hpp"
#ifdef SHELF_PS5
#define STBI_NO_THREAD_LOCALS
#define STBI_MALLOC(size) gs_malloc(size)
#define STBI_REALLOC(ptr, size) gs_realloc(ptr, size)
#define STBI_FREE(ptr) gs_free(ptr)
#define STBTT_malloc(size, user) ((void)(user), gs_malloc(size))
#define STBTT_free(ptr, user) ((void)(user), gs_free(ptr))
#define STBIW_MALLOC(size) gs_malloc(size)
#define STBIW_REALLOC(ptr, size) gs_realloc(ptr, size)
#define STBIW_FREE(ptr) gs_free(ptr)
#define PLM_MALLOC(size) gs_malloc(size)
#define PLM_REALLOC(ptr, size) gs_realloc(ptr, size)
#define PLM_FREE(ptr) gs_free(ptr)
#endif
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg/pl_mpeg.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
