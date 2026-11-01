#include "png.h"

#include <algorithm>
#include <fstream>
#include <vector>

namespace core
{
    namespace
    {
        uint32_t crc32(const uint8_t *data, size_t size, uint32_t crc = 0)
        {
            static uint32_t table[256];
            if (!table[1])
                for (uint32_t i = 0; i < 256; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    table[i] = c;
                }
            crc = ~crc;
            for (size_t i = 0; i < size; ++i)
                crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }

        void put32(std::vector<uint8_t> &out, uint32_t v)
        {
            out.push_back(uint8_t(v >> 24));
            out.push_back(uint8_t(v >> 16));
            out.push_back(uint8_t(v >> 8));
            out.push_back(uint8_t(v));
        }

        void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
        {
            put32(out, uint32_t(data.size()));
            const size_t start = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), data.begin(), data.end());
            put32(out, crc32(out.data() + start, out.size() - start));
        }
    }

    bool writePng(const std::filesystem::path &path, int width, int height, const uint8_t *rgba)
    {
        std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        std::vector<uint8_t> header;
        put32(header, uint32_t(width));
        put32(header, uint32_t(height));
        header.insert(header.end(), {8, 6, 0, 0, 0}); // 8 bits, RGBA, deflate, no filter method, no interlace
        chunk(png, "IHDR", header);

        // scanlines with filter byte 0, in a zlib stream of stored blocks
        const size_t row = size_t(width) * 4;
        std::vector<uint8_t> raw;
        raw.reserve((row + 1) * size_t(height));
        for (int y = 0; y < height; ++y)
        {
            raw.push_back(0);
            raw.insert(raw.end(), rgba + row * size_t(y), rgba + row * size_t(y + 1));
        }
        std::vector<uint8_t> z = {0x78, 0x01};
        for (size_t at = 0; at < raw.size() || raw.empty();)
        {
            const size_t n = std::min<size_t>(raw.size() - at, 65535);
            const bool last = at + n == raw.size();
            z.push_back(last ? 1 : 0);
            z.push_back(uint8_t(n));
            z.push_back(uint8_t(n >> 8));
            z.push_back(uint8_t(~n));
            z.push_back(uint8_t(~n >> 8));
            z.insert(z.end(), raw.begin() + ptrdiff_t(at), raw.begin() + ptrdiff_t(at + n));
            at += n;
            if (last)
                break;
        }
        uint32_t a = 1, b = 0; // Adler-32
        for (uint8_t c : raw)
        {
            a = (a + c) % 65521;
            b = (b + a) % 65521;
        }
        put32(z, (b << 16) | a);
        chunk(png, "IDAT", z);
        chunk(png, "IEND", {});

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(png.data()), std::streamsize(png.size()));
        return bool(out);
    }
}
