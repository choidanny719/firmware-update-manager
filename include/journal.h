#ifndef JOURNAL_H
#define JOURNAL_H

#include "flash.h"
#include <stdbool.h>

#define IMAGE_CAPACITY (256u * 1024u)
#define IMAGE_BASE (2u * FLASH_SECTOR_SIZE)
#define NO_SLOT UINT32_MAX
#define JOURNAL_RECORD_SIZE 52u

typedef struct {
    uint32_t slot;
    uint32_t version;
    uint32_t size;
    uint32_t checksum;
} image_descriptor;

typedef enum { BOOT_STABLE, BOOT_READY, BOOT_TRIAL } boot_phase;

typedef struct {
    uint32_t generation;
    image_descriptor confirmed;
    image_descriptor candidate;
    boot_phase phase;
} boot_record;

typedef struct {
    const flash_device *flash;
    boot_record record;
    int bank;
    bool usable;
} journal;

fw_result journal_load(journal *log, const flash_device *flash);
fw_result journal_store(journal *log, boot_record next);
uint32_t image_address(uint32_t slot);

#endif
