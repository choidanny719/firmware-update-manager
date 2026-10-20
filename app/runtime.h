#ifndef RUNTIME_H
#define RUNTIME_H

#include "nor.h"
#include "packet.h"
#include "update.h"

typedef struct {
    uint32_t receiver_packets;
    uint32_t controller_packets;
    uint32_t spi_commands;
    uint32_t queue_high_water;
    update_manager manager;
    fw_result result;
} runtime_report;

fw_result runtime_upload(nor_model *nor, const uint8_t *input, size_t length, runtime_report *report);

#endif
