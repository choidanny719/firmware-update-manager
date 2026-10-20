#include "flash.h"
#include "checksum.h"
#include "nor.h"
#include "unity.h"
#include <string.h>

static uint8_t memory[FLASH_SIZE];
static nor_model nor;
static flash_device flash;
static unsigned delays;

static void delay(void *context)
{
    TEST_ASSERT_EQUAL_PTR(&nor, context);
    ++delays;
}

void setUp(void)
{
    memset(memory, 0xff, sizeof(memory));
    nor_reset(&nor, memory);
    flash = (flash_device){nor_transfer, &nor, delay, 10};
    delays = 0;
}

void tearDown(void) {}

static void probe_matches_jedec_id(void)
{
    TEST_ASSERT_EQUAL(FW_OK, flash_probe(&flash));
    uint8_t tx[] = {0x9f, 0, 0, 0};
    uint8_t rx[4];
    TEST_ASSERT_EQUAL(FW_OK, nor_transfer(&nor, tx, rx, 4));
    uint8_t expected[] = {0xff, 0xef, 0x40, 0x17};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, rx, 4);
}

static void writes_split_at_pages_and_reads_cross_boundaries(void)
{
    uint8_t input[700], output[700];
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)i;
    TEST_ASSERT_EQUAL(FW_OK, flash_program(&flash, 250, input, sizeof(input)));
    TEST_ASSERT_EQUAL(4, nor.mutations);
    TEST_ASSERT_EQUAL(FW_OK, flash_read(&flash, 250, output, sizeof(output)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(input, output, sizeof(input));
    TEST_ASSERT_EQUAL_HEX8(0xff, memory[249]);
    TEST_ASSERT_EQUAL_HEX8(0xff, memory[950]);
    TEST_ASSERT_EQUAL(8, delays);
}

static void sector_erase_preserves_neighbors(void)
{
    memset(memory, 0, FLASH_SECTOR_SIZE * 3);
    TEST_ASSERT_EQUAL(FW_OK, flash_erase_sector(&flash, FLASH_SECTOR_SIZE));
    TEST_ASSERT_EQUAL_HEX8(0, memory[FLASH_SECTOR_SIZE - 1]);
    TEST_ASSERT_EACH_EQUAL_HEX8(0xff, memory + FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
    TEST_ASSERT_EQUAL_HEX8(0, memory[FLASH_SECTOR_SIZE * 2]);
}

static void programming_cannot_restore_zero_bits(void)
{
    uint8_t value = 0x55;
    TEST_ASSERT_EQUAL(FW_OK, flash_program(&flash, 0, &value, 1));
    value = 0xaa;
    TEST_ASSERT_EQUAL(FW_OK, flash_program(&flash, 0, &value, 1));
    TEST_ASSERT_EQUAL_HEX8(0, memory[0]);
    TEST_ASSERT_EQUAL(FW_OK, flash_erase_sector(&flash, 0));
    TEST_ASSERT_EQUAL(FW_OK, flash_program(&flash, 0, &value, 1));
    TEST_ASSERT_EQUAL_HEX8(0xaa, memory[0]);
}

static void model_enforces_write_enable_and_page_wrap(void)
{
    uint8_t command[] = {0x02, 0, 0, 255, 0x12, 0x34};
    TEST_ASSERT_EQUAL(FW_OK, nor_transfer(&nor, command, NULL, sizeof(command)));
    TEST_ASSERT_EQUAL_HEX8(0xff, memory[255]);
    uint8_t enable = 0x06;
    TEST_ASSERT_EQUAL(FW_OK, nor_transfer(&nor, &enable, NULL, 1));
    TEST_ASSERT_EQUAL(FW_OK, nor_transfer(&nor, command, NULL, sizeof(command)));
    TEST_ASSERT_EQUAL_HEX8(0x12, memory[255]);
    TEST_ASSERT_EQUAL_HEX8(0x34, memory[0]);
    TEST_ASSERT_FALSE(nor.write_enabled);
    TEST_ASSERT_EQUAL_HEX8(0xff, memory[256]);
}

static void busy_and_protected_devices_fail_without_mutation(void)
{
    uint8_t value = 1;
    nor.stuck_busy = true;
    TEST_ASSERT_EQUAL(FW_TIMEOUT, flash_probe(&flash));
    TEST_ASSERT_EQUAL(FW_TIMEOUT, flash_program(&flash, 0, &value, 1));
    TEST_ASSERT_EQUAL(FW_TIMEOUT, flash_erase_sector(&flash, 0));
    TEST_ASSERT_EQUAL(FW_TIMEOUT, flash_read(&flash, 0, &value, 1));
    TEST_ASSERT_EQUAL(40, delays);
    nor.stuck_busy = false;
    nor.protected = true;
    TEST_ASSERT_EQUAL(FW_PROTECTED, flash_program(&flash, 0, &value, 1));
    TEST_ASSERT_EQUAL(FW_PROTECTED, flash_erase_sector(&flash, 0));
    TEST_ASSERT_EQUAL(0, nor.mutations);
}

static void boundaries_and_bad_arguments_are_rejected(void)
{
    uint8_t value = 7;
    TEST_ASSERT_EQUAL(FW_OK, flash_program(&flash, FLASH_SIZE - 1, &value, 1));
    TEST_ASSERT_EQUAL(FW_OK, flash_read(&flash, FLASH_SIZE - 1, &value, 1));
    TEST_ASSERT_EQUAL(FW_INVALID, flash_read(&flash, FLASH_SIZE, &value, 1));
    TEST_ASSERT_EQUAL(FW_INVALID, flash_read(&flash, FLASH_SIZE - 1, &value, 2));
    TEST_ASSERT_EQUAL(FW_INVALID, flash_program(&flash, UINT32_MAX, &value, 1));
    TEST_ASSERT_EQUAL(FW_INVALID, flash_program(&flash, 0, &value, SIZE_MAX));
    TEST_ASSERT_EQUAL(FW_INVALID, flash_program(&flash, 0, NULL, 1));
    TEST_ASSERT_EQUAL(FW_INVALID, flash_read(&flash, 0, NULL, 1));
    TEST_ASSERT_EQUAL(FW_INVALID, flash_program(&flash, 0, &value, 0));
    TEST_ASSERT_EQUAL(FW_INVALID, flash_read(&flash, 0, &value, 0));
    TEST_ASSERT_EQUAL(FW_INVALID, flash_erase_sector(&flash, 1));
    TEST_ASSERT_EQUAL(FW_INVALID, flash_erase_sector(&flash, FLASH_SIZE));
    TEST_ASSERT_EQUAL(FW_INVALID, flash_probe(NULL));
    flash.poll_limit = 0;
    TEST_ASSERT_EQUAL(FW_INVALID, flash_probe(&flash));
    flash.poll_limit = 1;
    flash.transfer = NULL;
    TEST_ASSERT_EQUAL(FW_INVALID, flash_probe(&flash));
}

static void bus_errors_propagate(void)
{
    uint8_t value = 1;
    nor.io_failure = true;
    TEST_ASSERT_EQUAL(FW_IO, flash_probe(&flash));
    TEST_ASSERT_EQUAL(FW_IO, flash_read(&flash, 0, &value, 1));
    TEST_ASSERT_EQUAL(FW_IO, flash_program(&flash, 0, &value, 1));
    TEST_ASSERT_EQUAL(FW_IO, flash_erase_sector(&flash, 0));
}

static void power_loss_can_tear_program_and_erase_operations(void)
{
    uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    nor.cut_after = 1;
    nor.tear_bytes = 3;
    TEST_ASSERT_EQUAL(FW_POWER_LOSS, flash_program(&flash, 0, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, memory, 3);
    TEST_ASSERT_EQUAL_HEX8(0xff, memory[3]);
    TEST_ASSERT_EQUAL(FW_POWER_LOSS, flash_probe(&flash));
    nor_reset(&nor, memory);
    nor.cut_after = 1;
    nor.tear_bytes = 2;
    TEST_ASSERT_EQUAL(FW_POWER_LOSS, flash_erase_sector(&flash, 0));
    TEST_ASSERT_EACH_EQUAL_HEX8(0xff, memory, 2);
    TEST_ASSERT_EQUAL_HEX8(3, memory[2]);
}

static void crc_matches_standard_vector_and_incremental_updates(void)
{
    TEST_ASSERT_EQUAL_HEX32(0, crc32_update(0, NULL, 0));
    TEST_ASSERT_EQUAL_HEX32(0xcbf43926, crc32_update(0, "123456789", 9));
    uint32_t crc = crc32_update(0, "1234", 4);
    TEST_ASSERT_EQUAL_HEX32(0xcbf43926, crc32_update(crc, "56789", 5));
    uint8_t bytes[4];
    write_u32(bytes, 0x12345678);
    uint8_t expected[] = {0x78, 0x56, 0x34, 0x12};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, bytes, 4);
    TEST_ASSERT_EQUAL_HEX32(0x12345678, read_u32(bytes));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(probe_matches_jedec_id);
    RUN_TEST(writes_split_at_pages_and_reads_cross_boundaries);
    RUN_TEST(sector_erase_preserves_neighbors);
    RUN_TEST(programming_cannot_restore_zero_bits);
    RUN_TEST(model_enforces_write_enable_and_page_wrap);
    RUN_TEST(busy_and_protected_devices_fail_without_mutation);
    RUN_TEST(boundaries_and_bad_arguments_are_rejected);
    RUN_TEST(bus_errors_propagate);
    RUN_TEST(power_loss_can_tear_program_and_erase_operations);
    RUN_TEST(crc_matches_standard_vector_and_incremental_updates);
    return UNITY_END();
}
