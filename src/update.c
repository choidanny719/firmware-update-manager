#include "update.h"
#include "checksum.h"

static fw_result verify(const flash_device *flash, image_descriptor image)
{
    if (image.slot == NO_SLOT) return FW_NO_IMAGE;
    uint32_t checksum = 0;
    uint8_t bytes[FLASH_PAGE_SIZE];
    for (uint32_t offset = 0; offset < image.size;) {
        size_t length = image.size - offset;
        if (length > sizeof(bytes)) length = sizeof(bytes);
        fw_result result = flash_read(flash, image_address(image.slot) + offset, bytes, length);
        if (result != FW_OK) return result;
        checksum = crc32_update(checksum, bytes, length);
        offset += (uint32_t)length;
    }
    return checksum == image.checksum ? FW_OK : FW_CORRUPT;
}

static fw_result remember_result(update_manager *manager, fw_result result)
{
    if (result != FW_OK) manager->usable = false;
    return result;
}

fw_result update_open(update_manager *manager, const flash_device *flash)
{
    if (!manager || !flash) return FW_INVALID;
    *manager = (update_manager){0};
    fw_result result = flash_probe(flash);
    if (result == FW_OK) result = journal_load(&manager->log, flash);
    manager->usable = result == FW_OK;
    return result;
}

fw_result update_begin(update_manager *manager, uint32_t version, uint32_t size, uint32_t checksum)
{
    if (!manager || !manager->usable || manager->receiving || manager->log.record.phase != BOOT_STABLE) return FW_STATE;
    if (!version || version <= manager->log.record.confirmed.version || !size || size > IMAGE_CAPACITY) return FW_INVALID;
    manager->incoming = (image_descriptor){manager->log.record.confirmed.slot == 0 ? 1 : 0, version, size, checksum};
    for (uint32_t offset = 0; offset < size; offset += FLASH_SECTOR_SIZE) {
        fw_result result = flash_erase_sector(manager->log.flash, image_address(manager->incoming.slot) + offset);
        if (result != FW_OK) return remember_result(manager, result);
    }
    manager->received = 0;
    manager->receiving = true;
    return FW_OK;
}

fw_result update_write(update_manager *manager, uint32_t offset, const void *data, size_t length)
{
    if (!manager || !manager->usable || !manager->receiving) return FW_STATE;
    if (offset != manager->received) return FW_SEQUENCE;
    if (!data || !length || length > manager->incoming.size - manager->received) return FW_INVALID;
    fw_result result = flash_program(manager->log.flash, image_address(manager->incoming.slot) + offset, data, length);
    if (result != FW_OK) return remember_result(manager, result);
    manager->received += (uint32_t)length;
    return FW_OK;
}

fw_result update_finish(update_manager *manager)
{
    if (!manager || !manager->usable || !manager->receiving) return FW_STATE;
    if (manager->received != manager->incoming.size) return FW_INCOMPLETE;
    fw_result result = verify(manager->log.flash, manager->incoming);
    if (result != FW_OK) return remember_result(manager, result);
    boot_record next = manager->log.record;
    next.candidate = manager->incoming;
    next.phase = BOOT_READY;
    result = journal_store(&manager->log, next);
    if (result != FW_OK) return remember_result(manager, result);
    manager->receiving = false;
    return FW_OK;
}

static fw_result discard_candidate(update_manager *manager)
{
    boot_record next = manager->log.record;
    next.phase = BOOT_STABLE;
    next.candidate = (image_descriptor){.slot = NO_SLOT};
    return remember_result(manager, journal_store(&manager->log, next));
}

fw_result update_boot(update_manager *manager, boot_choice *choice)
{
    if (!manager || !manager->usable || manager->receiving) return FW_STATE;
    if (!choice) return FW_INVALID;
    if (manager->log.record.phase == BOOT_READY) {
        fw_result result = verify(manager->log.flash, manager->log.record.candidate);
        if (result == FW_OK) {
            boot_record next = manager->log.record;
            next.phase = BOOT_TRIAL;
            result = journal_store(&manager->log, next);
            if (result != FW_OK) return remember_result(manager, result);
            *choice = (boot_choice){next.candidate, true};
            return FW_OK;
        }
        if (result != FW_CORRUPT) return remember_result(manager, result);
        result = discard_candidate(manager);
        if (result != FW_OK) return result;
    } else if (manager->log.record.phase == BOOT_TRIAL) {
        fw_result result = discard_candidate(manager);
        if (result != FW_OK) return result;
    }
    fw_result result = verify(manager->log.flash, manager->log.record.confirmed);
    if (result == FW_OK) *choice = (boot_choice){manager->log.record.confirmed, false};
    return result;
}

fw_result update_confirm(update_manager *manager, uint32_t version)
{
    if (!manager || !manager->usable || manager->receiving || manager->log.record.phase != BOOT_TRIAL ||
        manager->log.record.candidate.version != version) return FW_STATE;
    fw_result result = verify(manager->log.flash, manager->log.record.candidate);
    if (result != FW_OK) return remember_result(manager, result);
    boot_record next = manager->log.record;
    next.confirmed = next.candidate;
    next.candidate = (image_descriptor){.slot = NO_SLOT};
    next.phase = BOOT_STABLE;
    return remember_result(manager, journal_store(&manager->log, next));
}
