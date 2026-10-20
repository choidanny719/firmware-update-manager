#ifndef CHECKSUM_H
#define CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

uint32_t crc32_update(uint32_t checksum, const void *data, size_t length);
uint32_t read_u32(const uint8_t *bytes);
void write_u32(uint8_t *bytes, uint32_t value);

#endif
