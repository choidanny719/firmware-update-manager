#include "flash.h"
#include <string.h>

static int valid(const flash_device *flash)
{
    return flash && flash->transfer && flash->poll_limit > 0;
}

static fw_result read_status(const flash_device *flash, uint8_t *status)
{
    uint8_t tx[2] = {0x05, 0};
    uint8_t rx[2];
    fw_result result = flash->transfer(flash->context, tx, rx, sizeof(tx));
    if (result == FW_OK) *status = rx[1];
    return result;
}

static fw_result wait_ready(const flash_device *flash)
{
    for (unsigned i = 0; i < flash->poll_limit; ++i) {
        uint8_t status;
        fw_result result = read_status(flash, &status);
        if (result != FW_OK) return result;
        if (!(status & 1)) return FW_OK;
        if (flash->poll_delay) flash->poll_delay(flash->context);
    }
    return FW_TIMEOUT;
}

static fw_result enable_write(const flash_device *flash)
{
    fw_result result = wait_ready(flash);
    if (result != FW_OK) return result;
    uint8_t command = 0x06;
    result = flash->transfer(flash->context, &command, NULL, 1);
    if (result != FW_OK) return result;
    uint8_t status;
    result = read_status(flash, &status);
    if (result != FW_OK) return result;
    return (status & 2) ? FW_OK : FW_PROTECTED;
}

static void address_bytes(uint8_t *tx, uint8_t command, uint32_t address)
{
    tx[0] = command;
    tx[1] = (uint8_t)(address >> 16);
    tx[2] = (uint8_t)(address >> 8);
    tx[3] = (uint8_t)address;
}

fw_result flash_probe(const flash_device *flash)
{
    if (!valid(flash)) return FW_INVALID;
    fw_result result = wait_ready(flash);
    if (result != FW_OK) return result;
    uint8_t tx[4] = {0x9f, 0, 0, 0};
    uint8_t rx[4];
    result = flash->transfer(flash->context, tx, rx, sizeof(tx));
    if (result != FW_OK) return result;
    return rx[1] == 0xef && rx[2] == 0x40 && rx[3] == 0x17 ? FW_OK : FW_INVALID;
}

fw_result flash_read(const flash_device *flash, uint32_t address, void *data, size_t length)
{
    if (!valid(flash) || !data || !length || address >= FLASH_SIZE || length > FLASH_SIZE - address) {
        return FW_INVALID;
    }
    fw_result result = wait_ready(flash);
    if (result != FW_OK) return result;
    uint8_t *output = data;
    while (length) {
        size_t count = length > FLASH_PAGE_SIZE ? FLASH_PAGE_SIZE : length;
        uint8_t tx[SPI_TRANSFER_MAX] = {0};
        uint8_t rx[SPI_TRANSFER_MAX];
        address_bytes(tx, 0x03, address);
        result = flash->transfer(flash->context, tx, rx, count + 4);
        if (result != FW_OK) return result;
        memcpy(output, rx + 4, count);
        output += count;
        address += (uint32_t)count;
        length -= count;
    }
    return FW_OK;
}

fw_result flash_program(const flash_device *flash, uint32_t address, const void *data, size_t length)
{
    if (!valid(flash) || !data || !length || address >= FLASH_SIZE || length > FLASH_SIZE - address) {
        return FW_INVALID;
    }
    const uint8_t *input = data;
    while (length) {
        size_t count = FLASH_PAGE_SIZE - address % FLASH_PAGE_SIZE;
        if (count > length) count = length;
        fw_result result = enable_write(flash);
        if (result != FW_OK) return result;
        uint8_t tx[SPI_TRANSFER_MAX];
        address_bytes(tx, 0x02, address);
        memcpy(tx + 4, input, count);
        result = flash->transfer(flash->context, tx, NULL, count + 4);
        if (result != FW_OK) return result;
        result = wait_ready(flash);
        if (result != FW_OK) return result;
        input += count;
        address += (uint32_t)count;
        length -= count;
    }
    return FW_OK;
}

fw_result flash_erase_sector(const flash_device *flash, uint32_t address)
{
    if (!valid(flash) || address >= FLASH_SIZE || address % FLASH_SECTOR_SIZE != 0) return FW_INVALID;
    fw_result result = enable_write(flash);
    if (result != FW_OK) return result;
    uint8_t tx[4];
    address_bytes(tx, 0x20, address);
    result = flash->transfer(flash->context, tx, NULL, sizeof(tx));
    return result == FW_OK ? wait_ready(flash) : result;
}
