#ifndef NOR_H
#define NOR_H

#include "flash.h"
#include <stdbool.h>

typedef struct {
    uint8_t *bytes;
    fw_result (*persist)(void *context, uint32_t address, size_t length);
    void *persist_context;
    uint32_t mutations;
    uint32_t cut_after;
    uint32_t tear_bytes;
    unsigned busy_reads;
    bool write_enabled;
    bool powered_off;
    bool stuck_busy;
    bool protected;
    bool io_failure;
} nor_model;

void nor_reset(nor_model *nor, uint8_t *bytes);
fw_result nor_transfer(void *context, const uint8_t *tx, uint8_t *rx, size_t length);

#endif
