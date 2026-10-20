/* NASBridge - the one place stb_image's code is compiled.
 * Copyright (C) 2026 NASBridge contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * stb_image.h itself (public domain or MIT, see LICENSE beside it) comes from
 * the toolchain image at a pinned commit; the build copies it here. Only the
 * PNG decoder is built: game covers are sce_sys/icon0.png. Memory goes through
 * the app's own malloc, and nothing reads files: the bytes come from the NAS.
 */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 4096
#include "third_party/stb_image/stb_image.h"
