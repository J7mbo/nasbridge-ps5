// NASBridge - Decoding game covers (sce_sys/icon0.png).
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/png.hpp"

#include "third_party/stb_image/stb_image.h"

#include <cstring>

namespace hui::concepts
{

bool decode_png(const std::uint8_t *data, std::size_t size, Image *image, std::string *error,
                int max_side)
{
    *image = Image{};
    if (size == 0 || size > 0x7fffffff)
    {
        *error = "empty or oversized file";
        return false;
    }
    const int length = static_cast<int>(size);
    int width = 0;
    int height = 0;
    int channels = 0;
    // The size first, so an absurd picture is refused before any memory is used.
    if (stbi_info_from_memory(data, length, &width, &height, &channels) == 0)
    {
        const char *reason = stbi_failure_reason();
        *error = reason != nullptr ? reason : "not a PNG";
        return false;
    }
    if (width <= 0 || height <= 0 || width > max_side || height > max_side)
    {
        *error = "picture size out of range";
        return false;
    }
    stbi_uc *pixels = stbi_load_from_memory(data, length, &width, &height, &channels, 4);
    if (pixels == nullptr)
    {
        const char *reason = stbi_failure_reason();
        *error = reason != nullptr ? reason : "couldn't decode";
        return false;
    }
    image->width = width;
    image->height = height;
    image->rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return true;
}

void shrink_to(Image *image, int max_side)
{
    while (image->width > max_side || image->height > max_side)
    {
        const int w = image->width / 2 > 0 ? image->width / 2 : 1;
        const int h = image->height / 2 > 0 ? image->height / 2 : 1;
        std::vector<std::uint8_t> smaller(static_cast<std::size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c)
                {
                    const auto at = [&](int sx, int sy) -> int
                    {
                        sx = sx < image->width ? sx : image->width - 1;
                        sy = sy < image->height ? sy : image->height - 1;
                        return image->rgba[(static_cast<std::size_t>(sy) * image->width + sx) * 4 + c];
                    };
                    const int sum = at(x * 2, y * 2) + at(x * 2 + 1, y * 2) + at(x * 2, y * 2 + 1) +
                                    at(x * 2 + 1, y * 2 + 1);
                    smaller[(static_cast<std::size_t>(y) * w + x) * 4 + c] =
                        static_cast<std::uint8_t>((sum + 2) / 4);
                }
        image->width = w;
        image->height = h;
        image->rgba = std::move(smaller);
    }
}

} // namespace hui::concepts
