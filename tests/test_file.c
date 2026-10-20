#include "flash_file.h"
#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char directory[64];
static char path[128];
static flash_file file;

void setUp(void)
{
    strcpy(directory, "/tmp/fw-file-test-XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/device.flash", directory);
    file = (flash_file){.descriptor = -1};
}

void tearDown(void)
{
    flash_file_close(&file);
    unlink(path);
    rmdir(directory);
}

static flash_device device(void)
{
    return (flash_device){nor_transfer, &file.nor, NULL, 10};
}

static void program_and_erase_survive_close_and_reopen(void)
{
    TEST_ASSERT_EQUAL(FW_OK, flash_file_create(path));
    TEST_ASSERT_EQUAL(FW_OK, flash_file_open(&file, path));
    TEST_ASSERT_EACH_EQUAL_HEX8(0xff, file.nor.bytes, FLASH_SIZE);
    flash_device flash = device();
    uint8_t value = 0x42;
    TEST_ASSERT_EQUAL(FW_OK, flash_program(&flash, FLASH_SIZE - 1, &value, 1));
    flash_file_close(&file);
    TEST_ASSERT_EQUAL(FW_OK, flash_file_open(&file, path));
    TEST_ASSERT_EQUAL_HEX8(0x42, file.nor.bytes[FLASH_SIZE - 1]);
    TEST_ASSERT_EQUAL(FW_OK, flash_erase_sector(&flash, FLASH_SIZE - FLASH_SECTOR_SIZE));
    flash_file_close(&file);
    TEST_ASSERT_EQUAL(FW_OK, flash_file_open(&file, path));
    TEST_ASSERT_EQUAL_HEX8(0xff, file.nor.bytes[FLASH_SIZE - 1]);
}

static void initialization_never_overwrites_existing_files(void)
{
    FILE *existing = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(existing);
    fputs("keep", existing);
    fclose(existing);
    TEST_ASSERT_EQUAL(FW_IO, flash_file_create(path));
    existing = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(existing);
    char content[5] = {0};
    TEST_ASSERT_EQUAL(4, fread(content, 1, 4, existing));
    fclose(existing);
    TEST_ASSERT_EQUAL_STRING("keep", content);
    TEST_ASSERT_EQUAL(FW_IO, flash_file_open(&file, path));
}

static void locked_flash_cannot_be_opened_by_another_owner(void)
{
    TEST_ASSERT_EQUAL(FW_OK, flash_file_create(path));
    TEST_ASSERT_EQUAL(FW_OK, flash_file_open(&file, path));
    flash_file other;
    TEST_ASSERT_EQUAL(FW_IO, flash_file_open(&other, path));
    flash_file_close(&other);
    flash_file_close(&file);
    TEST_ASSERT_EQUAL(FW_OK, flash_file_open(&file, path));
}

static void torn_writes_are_persisted_before_simulated_power_loss(void)
{
    TEST_ASSERT_EQUAL(FW_OK, flash_file_create(path));
    TEST_ASSERT_EQUAL(FW_OK, flash_file_open(&file, path));
    flash_device flash = device();
    file.nor.cut_after = 1;
    file.nor.tear_bytes = 3;
    uint8_t bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    TEST_ASSERT_EQUAL(FW_POWER_LOSS, flash_program(&flash, 0, bytes, sizeof(bytes)));
    flash_file_close(&file);
    TEST_ASSERT_EQUAL(FW_OK, flash_file_open(&file, path));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(bytes, file.nor.bytes, 3);
    TEST_ASSERT_EQUAL_HEX8(0xff, file.nor.bytes[3]);
}

static void storage_write_errors_are_reported(void)
{
    TEST_ASSERT_EQUAL(FW_OK, flash_file_create(path));
    TEST_ASSERT_EQUAL(FW_OK, flash_file_open(&file, path));
    close(file.descriptor);
    file.descriptor = -1;
    flash_device flash = device();
    uint8_t byte = 0;
    TEST_ASSERT_EQUAL(FW_IO, flash_program(&flash, 0, &byte, 1));
    flash_file_close(&file);
    TEST_ASSERT_EQUAL(FW_OK, flash_file_open(&file, path));
    TEST_ASSERT_EQUAL_HEX8(0xff, file.nor.bytes[0]);
}

static void missing_files_directories_and_null_arguments_are_rejected(void)
{
    TEST_ASSERT_EQUAL(FW_IO, flash_file_open(&file, path));
    TEST_ASSERT_EQUAL(FW_IO, flash_file_open(&file, directory));
    TEST_ASSERT_EQUAL(FW_INVALID, flash_file_open(NULL, path));
    TEST_ASSERT_EQUAL(FW_INVALID, flash_file_open(&file, NULL));
    TEST_ASSERT_EQUAL(FW_INVALID, flash_file_create(NULL));
    TEST_ASSERT_EQUAL(FW_IO, flash_file_create(directory));
    flash_file_close(NULL);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(program_and_erase_survive_close_and_reopen);
    RUN_TEST(initialization_never_overwrites_existing_files);
    RUN_TEST(locked_flash_cannot_be_opened_by_another_owner);
    RUN_TEST(torn_writes_are_persisted_before_simulated_power_loss);
    RUN_TEST(storage_write_errors_are_reported);
    RUN_TEST(missing_files_directories_and_null_arguments_are_rejected);
    return UNITY_END();
}
