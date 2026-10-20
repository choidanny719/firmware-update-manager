#ifndef PACKET_H
#define PACKET_H

#include "result.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PACKET_PAYLOAD_MAX 256u
#define PACKET_OVERHEAD 13u
#define PACKET_MAX (PACKET_OVERHEAD + PACKET_PAYLOAD_MAX)
#define UPLOAD_STREAM_MAX (4u * 1024u * 1024u)

typedef enum { PACKET_START = 1, PACKET_DATA, PACKET_END } packet_type;

typedef struct {
    packet_type type;
    uint32_t sequence;
    uint16_t length;
    uint8_t data[PACKET_PAYLOAD_MAX];
} packet;

typedef struct {
    uint8_t bytes[PACKET_MAX];
    size_t used;
    size_t expected;
    bool failed;
} packet_parser;

fw_result packet_feed(packet_parser *parser, uint8_t byte, packet *output, bool *complete);
fw_result packet_finish(const packet_parser *parser);

#endif
