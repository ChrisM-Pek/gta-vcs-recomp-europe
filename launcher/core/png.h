// A PNG writer for screenshots (--screenshot): RGBA8, stored (uncompressed) deflate blocks. No library.
#pragma once

#include <cstdint>
#include <filesystem>

namespace core
{
    bool writePng(const std::filesystem::path &path, int width, int height, const uint8_t *rgba);
}
