#ifndef UPDATE_H
#define UPDATE_H

#include "journal.h"

typedef struct {
    journal log;
    image_descriptor incoming;
    uint32_t received;
    bool receiving;
    bool usable;
} update_manager;

typedef struct {
    image_descriptor image;
    bool trial;
} boot_choice;

fw_result update_open(update_manager *manager, const flash_device *flash);
fw_result update_begin(update_manager *manager, uint32_t version, uint32_t size, uint32_t checksum);
fw_result update_write(update_manager *manager, uint32_t offset, const void *data, size_t length);
fw_result update_finish(update_manager *manager);
fw_result update_boot(update_manager *manager, boot_choice *choice);
fw_result update_confirm(update_manager *manager, uint32_t version);

#endif
