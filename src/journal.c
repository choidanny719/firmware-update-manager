#include "journal.h"
#include "checksum.h"
#include <string.h>

#define JOURNAL_MAGIC 0x31555746u
#define COMMIT_MARKER 0x54494d43u

uint32_t image_address(uint32_t slot)
{
    return IMAGE_BASE + slot * IMAGE_CAPACITY;
}

static bool valid_image(image_descriptor image)
{
    if (image.slot == NO_SLOT) return image.version == 0 && image.size == 0 && image.checksum == 0;
    return image.slot < 2 && image.version > 0 && image.size > 0 && image.size <= IMAGE_CAPACITY;
}

static bool valid_record(boot_record record)
{
    if (!valid_image(record.confirmed) || !valid_image(record.candidate)) return false;
    if (record.phase == BOOT_STABLE) return record.candidate.slot == NO_SLOT;
    return (record.phase == BOOT_READY || record.phase == BOOT_TRIAL) &&
           record.candidate.slot != NO_SLOT && record.candidate.slot != record.confirmed.slot &&
           record.candidate.version > record.confirmed.version;
}

static void encode_image(uint8_t *bytes, image_descriptor image)
{
    write_u32(bytes, image.slot);
    write_u32(bytes + 4, image.version);
    write_u32(bytes + 8, image.size);
    write_u32(bytes + 12, image.checksum);
}

static image_descriptor decode_image(const uint8_t *bytes)
{
    return (image_descriptor){read_u32(bytes), read_u32(bytes + 4), read_u32(bytes + 8), read_u32(bytes + 12)};
}

static void encode(uint8_t *bytes, boot_record record)
{
    write_u32(bytes, JOURNAL_MAGIC);
    write_u32(bytes + 4, record.generation);
    encode_image(bytes + 8, record.confirmed);
    encode_image(bytes + 24, record.candidate);
    write_u32(bytes + 40, (uint32_t)record.phase);
    write_u32(bytes + 44, crc32_update(0, bytes, 44));
    write_u32(bytes + 48, COMMIT_MARKER);
}

static bool decode(const uint8_t *bytes, boot_record *record)
{
    if (read_u32(bytes) != JOURNAL_MAGIC || read_u32(bytes + 48) != COMMIT_MARKER ||
        read_u32(bytes + 44) != crc32_update(0, bytes, 44) || read_u32(bytes + 40) > BOOT_TRIAL) {
        return false;
    }
    *record = (boot_record){read_u32(bytes + 4), decode_image(bytes + 8),
                           decode_image(bytes + 24), (boot_phase)read_u32(bytes + 40)};
    return valid_record(*record);
}

fw_result journal_load(journal *log, const flash_device *flash)
{
    if (!log || !flash) return FW_INVALID;
    *log = (journal){.flash = flash, .bank = -1, .record = {
        .confirmed = {.slot = NO_SLOT}, .candidate = {.slot = NO_SLOT}
    }};
    uint8_t bytes[2][JOURNAL_RECORD_SIZE];
    boot_record records[2];
    bool valid[2];
    bool blank = true;
    for (unsigned bank = 0; bank < 2; ++bank) {
        fw_result result = flash_read(flash, bank * FLASH_SECTOR_SIZE, bytes[bank], JOURNAL_RECORD_SIZE);
        if (result != FW_OK) return result;
        valid[bank] = decode(bytes[bank], &records[bank]);
        for (size_t i = 0; i < JOURNAL_RECORD_SIZE; ++i) blank = blank && bytes[bank][i] == 0xff;
    }
    if (!valid[0] && !valid[1]) {
        log->usable = blank;
        return blank ? FW_OK : FW_CORRUPT;
    }
    unsigned selected = valid[0] ? 0 : 1;
    if (valid[0] && valid[1]) {
        uint32_t distance = records[1].generation - records[0].generation;
        if (distance == 0x80000000u ||
            (distance == 0 && memcmp(bytes[0], bytes[1], JOURNAL_RECORD_SIZE) != 0)) return FW_CORRUPT;
        if (distance && distance < 0x80000000u) selected = 1;
    }
    log->bank = (int)selected;
    log->record = records[selected];
    log->usable = true;
    return FW_OK;
}

fw_result journal_store(journal *log, boot_record next)
{
    if (!log || !log->usable) return FW_STATE;
    if (!valid_record(next)) return FW_INVALID;
    next.generation = log->record.generation + 1;
    uint8_t bytes[JOURNAL_RECORD_SIZE], actual[JOURNAL_RECORD_SIZE];
    encode(bytes, next);
    int bank = log->bank == 0 ? 1 : 0;
    uint32_t address = (uint32_t)bank * FLASH_SECTOR_SIZE;
    fw_result result = flash_erase_sector(log->flash, address);
    if (result == FW_OK) result = flash_program(log->flash, address, bytes, 48);
    if (result == FW_OK) result = flash_program(log->flash, address + 48, bytes + 48, 4);
    if (result == FW_OK) result = flash_read(log->flash, address, actual, sizeof(actual));
    if (result == FW_OK && memcmp(bytes, actual, sizeof(bytes)) != 0) result = FW_CORRUPT;
    if (result != FW_OK) {
        log->usable = false;
        return result;
    }
    log->bank = bank;
    log->record = next;
    return FW_OK;
}
