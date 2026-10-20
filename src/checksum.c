#include "checksum.h"

uint32_t crc32_update(uint32_t checksum, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t value = ~checksum;
    for (size_t i = 0; i < length; ++i) {
        value ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0);
        }
    }
    return ~value;
}

uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

void write_u32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (i * 8));
}
