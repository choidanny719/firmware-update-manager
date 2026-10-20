#ifndef FLASH_H
#define FLASH_H

#include "result.h"
#include <stddef.h>
#include <stdint.h>

#define FLASH_SIZE (8u * 1024u * 1024u)
#define FLASH_SECTOR_SIZE 4096u
#define FLASH_PAGE_SIZE 256u
#define SPI_TRANSFER_MAX (FLASH_PAGE_SIZE + 4u)

typedef fw_result (*spi_transfer)(void *context, const uint8_t *tx, uint8_t *rx, size_t length);

typedef struct {
    spi_transfer transfer;
    void *context;
    void (*poll_delay)(void *context);
    unsigned poll_limit;
} flash_device;

fw_result flash_probe(const flash_device *flash);
fw_result flash_read(const flash_device *flash, uint32_t address, void *data, size_t length);
fw_result flash_program(const flash_device *flash, uint32_t address, const void *data, size_t length);
fw_result flash_erase_sector(const flash_device *flash, uint32_t address);

#endif
