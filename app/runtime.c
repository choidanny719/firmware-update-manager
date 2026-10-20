#include "runtime.h"
#include "checksum.h"
#include "FreeRTOS.h"
#include "event_groups.h"
#include "queue.h"
#include "task.h"
#include <string.h>

enum { STOP = 1, RECEIVER_DONE = 2, STORAGE_DONE = 4, PACKET_QUEUE_CAPACITY = 4 };

typedef struct {
    packet value;
    fw_result result;
    bool end_of_input;
} received_message;

typedef struct {
    size_t length;
    uint8_t bytes[SPI_TRANSFER_MAX];
    bool read;
} spi_request;

typedef struct {
    fw_result result;
    uint8_t bytes[SPI_TRANSFER_MAX];
} spi_response;

typedef struct {
    nor_model *nor;
    const uint8_t *input;
    size_t length;
    flash_device flash;
    QueueHandle_t packets;
    QueueHandle_t requests;
    QueueHandle_t responses;
    EventGroupHandle_t events;
    TaskHandle_t controller;
    runtime_report report;
    bool finished;
} application;

static application app;

static bool send_received(const received_message *message)
{
    while (!(xEventGroupGetBits(app.events) & STOP)) {
        if (xQueueSend(app.packets, message, pdMS_TO_TICKS(20)) == pdTRUE) {
            uint32_t depth = (uint32_t)uxQueueMessagesWaiting(app.packets);
            if (depth > app.report.queue_high_water) app.report.queue_high_water = depth;
            return true;
        }
    }
    return false;
}

static void receiver_task(void *argument)
{
    (void)argument;
    packet_parser parser = {0};
    received_message message = {0};
    fw_result result = FW_OK;
    for (size_t i = 0; i < app.length && !(xEventGroupGetBits(app.events) & STOP); ++i) {
        bool complete;
        result = packet_feed(&parser, app.input[i], &message.value, &complete);
        if (result != FW_OK) break;
        if (complete) {
            ++app.report.receiver_packets;
            if (!send_received(&message)) break;
        }
    }
    message.end_of_input = true;
    message.result = result == FW_OK ? packet_finish(&parser) : result;
    send_received(&message);
    xEventGroupSetBits(app.events, RECEIVER_DONE);
    vTaskDelete(NULL);
}

static void storage_task(void *argument)
{
    (void)argument;
    for (;;) {
        spi_request request;
        configASSERT(xQueueReceive(app.requests, &request, portMAX_DELAY) == pdTRUE);
        if (request.length == 0) break;
        spi_response response = {0};
        response.result = nor_transfer(app.nor, request.bytes, request.read ? response.bytes : NULL, request.length);
        ++app.report.spi_commands;
        configASSERT(xQueueSend(app.responses, &response, portMAX_DELAY) == pdTRUE);
    }
    xEventGroupSetBits(app.events, STORAGE_DONE);
    vTaskDelete(NULL);
}

static fw_result queued_transfer(void *context, const uint8_t *tx, uint8_t *rx, size_t length)
{
    (void)context;
    configASSERT(length > 0 && length <= SPI_TRANSFER_MAX);
    spi_request request = {.length = length, .read = rx != NULL};
    memcpy(request.bytes, tx, length);
    spi_response response;
    configASSERT(xQueueSend(app.requests, &request, portMAX_DELAY) == pdTRUE);
    configASSERT(xQueueReceive(app.responses, &response, portMAX_DELAY) == pdTRUE);
    if (response.result == FW_OK && rx) memcpy(rx, response.bytes, length);
    return response.result;
}

static void poll_delay(void *context)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(1));
}

static fw_result process_packet(const packet *value, uint32_t *sequence, bool *ended)
{
    if (*ended) return FW_STATE;
    if (value->sequence != *sequence) return FW_SEQUENCE;
    fw_result result;
    if (*sequence == 0) {
        if (value->type != PACKET_START) return FW_STATE;
        result = update_begin(&app.report.manager, read_u32(value->data),
                              read_u32(value->data + 4), read_u32(value->data + 8));
    } else if (value->type == PACKET_DATA) {
        result = update_write(&app.report.manager, app.report.manager.received, value->data, value->length);
    } else if (value->type == PACKET_END) {
        *ended = true;
        result = FW_OK;
    } else {
        result = FW_STATE;
    }
    if (result == FW_OK) ++*sequence;
    return result;
}

static void controller_task(void *argument)
{
    (void)argument;
    fw_result result = update_open(&app.report.manager, &app.flash);
    uint32_t sequence = 0;
    bool ended = false;
    while (result == FW_OK) {
        received_message message;
        configASSERT(xQueueReceive(app.packets, &message, portMAX_DELAY) == pdTRUE);
        if (message.end_of_input) {
            result = message.result;
            if (result == FW_OK) result = ended ? update_finish(&app.report.manager) : FW_INCOMPLETE;
            break;
        }
        ++app.report.controller_packets;
        result = process_packet(&message.value, &sequence, &ended);
    }
    app.report.result = result;
    xEventGroupSetBits(app.events, STOP);
    spi_request stop = {0};
    configASSERT(xQueueSend(app.requests, &stop, portMAX_DELAY) == pdTRUE);
    xEventGroupWaitBits(app.events, RECEIVER_DONE | STORAGE_DONE, pdFALSE, pdTRUE, portMAX_DELAY);
    app.finished = true;
    vTaskEndScheduler();
}

void vApplicationMallocFailedHook(void)
{
    abort();
}

fw_result runtime_upload(nor_model *nor, const uint8_t *input, size_t length, runtime_report *report)
{
    if (!nor || !input || !report || length > UPLOAD_STREAM_MAX) return FW_INVALID;
    app = (application){.nor = nor, .input = input, .length = length};
    app.flash = (flash_device){queued_transfer, NULL, poll_delay, 100};
    app.packets = xQueueCreate(PACKET_QUEUE_CAPACITY, sizeof(received_message));
    app.requests = xQueueCreate(1, sizeof(spi_request));
    app.responses = xQueueCreate(1, sizeof(spi_response));
    app.events = xEventGroupCreate();
    configASSERT(app.packets && app.requests && app.responses && app.events);
    configASSERT(xTaskCreate(receiver_task, "receiver", 2048, NULL, 1, NULL) == pdPASS);
    configASSERT(xTaskCreate(storage_task, "storage", 2048, NULL, 3, NULL) == pdPASS);
    configASSERT(xTaskCreate(controller_task, "controller", 2048, NULL, 2, &app.controller) == pdPASS);
    vTaskStartScheduler();
    if (!app.finished) return FW_IO;
    vTaskDelete(app.controller);
    *report = app.report;
    report->manager.log.flash = NULL;
    report->manager.log.usable = false;
    report->manager.usable = false;
    vQueueDelete(app.packets);
    vQueueDelete(app.requests);
    vQueueDelete(app.responses);
    vEventGroupDelete(app.events);
    return report->result;
}
