#include "packet.h"
#include "checksum.h"
#include <string.h>

fw_result packet_feed(packet_parser *parser, uint8_t byte, packet *output, bool *complete)
{
    if (!parser || !output || !complete) return FW_INVALID;
    *complete = false;
    if (parser->failed) return FW_STATE;
    parser->bytes[parser->used++] = byte;
    if ((parser->used == 1 && byte != 0x55) || (parser->used == 2 && byte != 0xaa)) {
        parser->failed = true;
        return FW_INVALID;
    }
    if (parser->used == 9) {
        unsigned length = (unsigned)parser->bytes[7] | (unsigned)parser->bytes[8] << 8;
        uint8_t type = parser->bytes[2];
        bool valid = (type == PACKET_START && length == 12) ||
                     (type == PACKET_DATA && length > 0 && length <= PACKET_PAYLOAD_MAX) ||
                     (type == PACKET_END && length == 0);
        if (!valid) {
            parser->failed = true;
            return FW_INVALID;
        }
        parser->expected = PACKET_OVERHEAD + length;
    }
    if (parser->expected && parser->used == parser->expected) {
        size_t checked = parser->used - 4;
        if (read_u32(parser->bytes + checked) != crc32_update(0, parser->bytes, checked)) {
            parser->failed = true;
            return FW_CORRUPT;
        }
        output->type = (packet_type)parser->bytes[2];
        output->sequence = read_u32(parser->bytes + 3);
        output->length = (uint16_t)(parser->used - PACKET_OVERHEAD);
        memcpy(output->data, parser->bytes + 9, output->length);
        parser->used = 0;
        parser->expected = 0;
        *complete = true;
    }
    return FW_OK;
}

fw_result packet_finish(const packet_parser *parser)
{
    if (!parser) return FW_INVALID;
    if (parser->failed) return FW_STATE;
    return parser->used ? FW_INCOMPLETE : FW_OK;
}
