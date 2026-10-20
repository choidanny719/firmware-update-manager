#include "packet.h"
#include "checksum.h"
#include "unity.h"
#include <string.h>

static packet_parser parser;
static packet output;
static uint8_t wire[PACKET_MAX];

void setUp(void)
{
    parser = (packet_parser){0};
    output = (packet){0};
    memset(wire, 0, sizeof(wire));
}

void tearDown(void) {}

static size_t frame(unsigned type, uint32_t sequence, unsigned length)
{
    wire[0] = 0x55;
    wire[1] = 0xaa;
    wire[2] = (uint8_t)type;
    write_u32(wire + 3, sequence);
    wire[7] = (uint8_t)length;
    wire[8] = (uint8_t)(length >> 8);
    for (unsigned i = 0; i < length; ++i) wire[9 + i] = (uint8_t)i;
    write_u32(wire + 9 + length, crc32_update(0, wire, 9 + length));
    return PACKET_OVERHEAD + length;
}

static fw_result feed(size_t count, bool *complete)
{
    fw_result result = FW_OK;
    for (size_t i = 0; result == FW_OK && i < count; ++i) result = packet_feed(&parser, wire[i], &output, complete);
    return result;
}

static void all_payload_lengths_parse_one_byte_at_a_time(void)
{
    for (unsigned length = 1; length <= PACKET_PAYLOAD_MAX; ++length) {
        bool complete;
        size_t size = frame(PACKET_DATA, UINT32_MAX, length);
        for (size_t i = 0; i < size; ++i) {
            TEST_ASSERT_EQUAL(FW_OK, packet_feed(&parser, wire[i], &output, &complete));
            TEST_ASSERT_EQUAL(i == size - 1, complete);
        }
        TEST_ASSERT_EQUAL(length, output.length);
        TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, output.sequence);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(wire + 9, output.data, length);
    }
}

static void start_and_end_frames_can_be_concatenated(void)
{
    bool complete;
    TEST_ASSERT_EQUAL(FW_OK, feed(frame(PACKET_START, 0, 12), &complete));
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL(PACKET_START, output.type);
    TEST_ASSERT_EQUAL(FW_OK, feed(frame(PACKET_END, 1, 0), &complete));
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL(PACKET_END, output.type);
    TEST_ASSERT_EQUAL(FW_OK, packet_finish(&parser));
}

static void every_truncation_is_detected(void)
{
    size_t size = frame(PACKET_DATA, 1, 256);
    for (size_t length = 1; length < size; ++length) {
        parser = (packet_parser){0};
        bool complete;
        TEST_ASSERT_EQUAL(FW_OK, feed(length, &complete));
        TEST_ASSERT_FALSE(complete);
        TEST_ASSERT_EQUAL(FW_INCOMPLETE, packet_finish(&parser));
    }
}

static void corruption_in_each_byte_is_rejected(void)
{
    size_t size = frame(PACKET_DATA, 1, 100);
    for (size_t index = 0; index < size; ++index) {
        parser = (packet_parser){0};
        wire[index] ^= 0x80;
        bool complete;
        fw_result result = feed(size, &complete);
        if (result == FW_OK) result = packet_finish(&parser);
        TEST_ASSERT_NOT_EQUAL(FW_OK, result);
        TEST_ASSERT_FALSE(complete);
        wire[index] ^= 0x80;
    }
}

static void unknown_types_and_invalid_lengths_fail_at_the_header(void)
{
    const unsigned cases[][2] = {{0, 1}, {4, 1}, {1, 0}, {1, 11}, {1, 13}, {2, 0}, {2, 257}, {2, 65535}, {3, 1}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        parser = (packet_parser){0};
        frame(PACKET_END, 0, 0);
        wire[2] = (uint8_t)cases[i][0];
        wire[7] = (uint8_t)cases[i][1];
        wire[8] = (uint8_t)(cases[i][1] >> 8);
        bool complete;
        TEST_ASSERT_EQUAL(FW_INVALID, feed(9, &complete));
        TEST_ASSERT_EQUAL(FW_STATE, packet_feed(&parser, 0, &output, &complete));
        TEST_ASSERT_EQUAL(FW_STATE, packet_finish(&parser));
    }
}

static void bad_magic_and_null_arguments_are_rejected(void)
{
    bool complete;
    TEST_ASSERT_EQUAL(FW_INVALID, packet_feed(&parser, 0, &output, &complete));
    parser = (packet_parser){0};
    TEST_ASSERT_EQUAL(FW_OK, packet_feed(&parser, 0x55, &output, &complete));
    TEST_ASSERT_EQUAL(FW_INVALID, packet_feed(&parser, 0, &output, &complete));
    TEST_ASSERT_EQUAL(FW_INVALID, packet_feed(NULL, 0, &output, &complete));
    TEST_ASSERT_EQUAL(FW_INVALID, packet_feed(&parser, 0, NULL, &complete));
    TEST_ASSERT_EQUAL(FW_INVALID, packet_feed(&parser, 0, &output, NULL));
    TEST_ASSERT_EQUAL(FW_INVALID, packet_finish(NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(all_payload_lengths_parse_one_byte_at_a_time);
    RUN_TEST(start_and_end_frames_can_be_concatenated);
    RUN_TEST(every_truncation_is_detected);
    RUN_TEST(corruption_in_each_byte_is_rejected);
    RUN_TEST(unknown_types_and_invalid_lengths_fail_at_the_header);
    RUN_TEST(bad_magic_and_null_arguments_are_rejected);
    return UNITY_END();
}
