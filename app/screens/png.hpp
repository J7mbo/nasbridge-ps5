// NASBridge - Decoding game covers (sce_sys/icon0.png).
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A thin wrapper over stb_image (see src/third_party/stb_image). Output is
// RGBA, 8 bits per channel, top row first.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hui::concepts
{

struct Image
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; // width * height * 4
};

// Decodes `data`; on failure returns false and says why in *error.
// Images larger than max_side on either side are refused.
bool decode_png(const std::uint8_t *data, std::size_t size, Image *image, std::string *error,
                int max_side = 4096);

// Shrinks an image by whole steps (box filter) until neither side is over
// max_side. Covers are 512 x 512; nothing larger is needed on screen.
void shrink_to(Image *image, int max_side);

} // namespace hui::concepts
