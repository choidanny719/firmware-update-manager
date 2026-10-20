#include "nor.h"
#include <string.h>

void nor_reset(nor_model *nor, uint8_t *bytes)
{
    *nor = (nor_model){.bytes = bytes};
}

static fw_result persist(nor_model *nor, uint32_t address, size_t length)
{
    return nor->persist && length ? nor->persist(nor->persist_context, address, length) : FW_OK;
}

fw_result nor_transfer(void *context, const uint8_t *tx, uint8_t *rx, size_t length)
{
    nor_model *nor = context;
    if (!nor || !nor->bytes || !tx || !length || length > SPI_TRANSFER_MAX) return FW_INVALID;
    if (nor->powered_off) return FW_POWER_LOSS;
    if (nor->io_failure) return FW_IO;
    if (rx) memset(rx, 0xff, length);
    if (tx[0] == 0x05 && length == 2 && rx) {
        bool busy = nor->stuck_busy || nor->busy_reads != 0;
        rx[1] = (uint8_t)((busy ? 1 : 0) | (nor->write_enabled ? 2 : 0));
        if (nor->busy_reads) --nor->busy_reads;
        return FW_OK;
    }
    if (nor->stuck_busy || nor->busy_reads) return FW_OK;
    if (tx[0] == 0x9f && length == 4 && rx) {
        rx[1] = 0xef;
        rx[2] = 0x40;
        rx[3] = 0x17;
        return FW_OK;
    }
    if (tx[0] == 0x06 && length == 1) {
        nor->write_enabled = !nor->protected;
        return FW_OK;
    }
    if (tx[0] == 0x04 && length == 1) {
        nor->write_enabled = false;
        return FW_OK;
    }
    if (length < 4) return FW_INVALID;
    uint32_t address = (uint32_t)tx[1] << 16 | (uint32_t)tx[2] << 8 | tx[3];
    if (address >= FLASH_SIZE) return FW_INVALID;
    if (tx[0] == 0x03 && length > 4 && rx && length - 4 <= FLASH_SIZE - address) {
        memcpy(rx + 4, nor->bytes + address, length - 4);
        return FW_OK;
    }
    bool program = tx[0] == 0x02 && length > 4;
    bool erase = tx[0] == 0x20 && length == 4;
    if (!program && !erase) return FW_INVALID;
    if (!nor->write_enabled || nor->protected) return FW_OK;
    nor->write_enabled = false;
    ++nor->mutations;
    bool cut = nor->cut_after && nor->mutations == nor->cut_after;
    size_t count = program ? length - 4 : FLASH_SECTOR_SIZE;
    if (cut && nor->tear_bytes < count) count = nor->tear_bytes;
    fw_result result;
    if (program) {
        uint32_t page = address - address % FLASH_PAGE_SIZE;
        for (size_t i = 0; i < count; ++i) {
            uint32_t target = page + (uint32_t)((address % FLASH_PAGE_SIZE + i) % FLASH_PAGE_SIZE);
            nor->bytes[target] &= tx[i + 4];
        }
        result = count ? persist(nor, page, FLASH_PAGE_SIZE) : FW_OK;
    } else {
        address -= address % FLASH_SECTOR_SIZE;
        memset(nor->bytes + address, 0xff, count);
        result = persist(nor, address, count);
    }
    if (cut) {
        nor->powered_off = true;
        return result == FW_OK ? FW_POWER_LOSS : result;
    }
    nor->busy_reads = 2;
    return result;
}
