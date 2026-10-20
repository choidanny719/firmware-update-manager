#include "update.h"
#include "checksum.h"
#include "nor.h"
#include "unity.h"
#include <string.h>

static uint8_t memory[FLASH_SIZE];
static uint8_t backup[IMAGE_BASE + 2 * IMAGE_CAPACITY];
static uint8_t image[600];
static uint8_t baseline_image[600];
static nor_model nor;
static flash_device flash;
static update_manager manager;

static void reopen(void)
{
    nor_reset(&nor, memory);
    TEST_ASSERT_EQUAL(FW_OK, update_open(&manager, &flash));
}

void setUp(void)
{
    memset(memory, 0xff, sizeof(memory));
    for (size_t i = 0; i < sizeof(image); ++i) image[i] = (uint8_t)(i * 17);
    flash = (flash_device){nor_transfer, &nor, NULL, 10};
    reopen();
}

void tearDown(void) {}

static fw_result upload(uint32_t version)
{
    fw_result result = update_begin(&manager, version, sizeof(image), crc32_update(0, image, sizeof(image)));
    for (uint32_t offset = 0; result == FW_OK && offset < sizeof(image);) {
        size_t length = sizeof(image) - offset;
        if (length > 200) length = 200;
        result = update_write(&manager, offset, image + offset, length);
        offset += (uint32_t)length;
    }
    return result == FW_OK ? update_finish(&manager) : result;
}

static void confirmed_baseline(void)
{
    memcpy(baseline_image, image, sizeof(image));
    boot_choice choice;
    TEST_ASSERT_EQUAL(FW_OK, upload(1));
    TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
    TEST_ASSERT_EQUAL(FW_OK, update_confirm(&manager, 1));
    reopen();
    for (size_t i = 0; i < sizeof(image); ++i) image[i] ^= 0xa5;
}

static void empty_flash_has_no_bootable_image(void)
{
    boot_choice choice;
    TEST_ASSERT_EQUAL(FW_NO_IMAGE, update_boot(&manager, &choice));
    TEST_ASSERT_EQUAL(FW_STATE, update_confirm(&manager, 1));
    TEST_ASSERT_EQUAL(0, nor.mutations);
}

static void initial_image_is_trial_then_confirmed_across_restarts(void)
{
    TEST_ASSERT_EQUAL(FW_OK, upload(1));
    reopen();
    TEST_ASSERT_EQUAL(BOOT_READY, manager.log.record.phase);
    boot_choice choice;
    TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
    TEST_ASSERT_TRUE(choice.trial);
    TEST_ASSERT_EQUAL(1, choice.image.version);
    TEST_ASSERT_EQUAL(FW_STATE, update_confirm(&manager, 2));
    TEST_ASSERT_EQUAL(FW_OK, update_confirm(&manager, 1));
    reopen();
    TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
    TEST_ASSERT_FALSE(choice.trial);
    TEST_ASSERT_EQUAL(1, choice.image.version);
}

static void unconfirmed_upgrade_rolls_back_without_touching_old_bytes(void)
{
    confirmed_baseline();
    uint32_t old_slot = manager.log.record.confirmed.slot;
    TEST_ASSERT_EQUAL(FW_OK, upload(2));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(baseline_image, memory + image_address(old_slot), sizeof(image));
    boot_choice choice;
    TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
    TEST_ASSERT_TRUE(choice.trial);
    TEST_ASSERT_EQUAL(2, choice.image.version);
    reopen();
    TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
    TEST_ASSERT_FALSE(choice.trial);
    TEST_ASSERT_EQUAL(1, choice.image.version);
    TEST_ASSERT_EQUAL(BOOT_STABLE, manager.log.record.phase);
}

static void confirmed_upgrades_alternate_slots(void)
{
    confirmed_baseline();
    boot_choice choice;
    for (uint32_t version = 2; version <= 5; ++version) {
        uint32_t previous = manager.log.record.confirmed.slot;
        TEST_ASSERT_EQUAL(FW_OK, upload(version));
        TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
        TEST_ASSERT_NOT_EQUAL(previous, choice.image.slot);
        TEST_ASSERT_EQUAL(FW_OK, update_confirm(&manager, version));
        reopen();
        TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
        TEST_ASSERT_EQUAL(version, choice.image.version);
        TEST_ASSERT_FALSE(choice.trial);
    }
}

static void invalid_sizes_versions_and_state_transitions_are_rejected(void)
{
    confirmed_baseline();
    TEST_ASSERT_EQUAL(FW_INVALID, update_begin(&manager, 0, 10, 0));
    TEST_ASSERT_EQUAL(FW_INVALID, update_begin(&manager, 1, 10, 0));
    TEST_ASSERT_EQUAL(FW_INVALID, update_begin(&manager, 2, 0, 0));
    TEST_ASSERT_EQUAL(FW_INVALID, update_begin(&manager, 2, IMAGE_CAPACITY + 1, 0));
    TEST_ASSERT_EQUAL(FW_STATE, update_finish(&manager));
    TEST_ASSERT_EQUAL(FW_STATE, update_write(&manager, 0, image, 1));
    TEST_ASSERT_EQUAL(FW_STATE, update_confirm(&manager, 1));
    TEST_ASSERT_EQUAL(FW_OK, upload(2));
    TEST_ASSERT_EQUAL(FW_STATE, update_begin(&manager, 3, 10, 0));
    boot_choice choice;
    TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
    TEST_ASSERT_EQUAL(FW_STATE, update_begin(&manager, 3, 10, 0));
}

static void missing_duplicate_and_out_of_order_data_cannot_publish_an_image(void)
{
    confirmed_baseline();
    TEST_ASSERT_EQUAL(FW_OK, update_begin(&manager, 2, sizeof(image), crc32_update(0, image, sizeof(image))));
    TEST_ASSERT_EQUAL(FW_SEQUENCE, update_write(&manager, 1, image, 1));
    TEST_ASSERT_EQUAL(FW_OK, update_write(&manager, 0, image, 200));
    TEST_ASSERT_EQUAL(FW_SEQUENCE, update_write(&manager, 0, image, 200));
    TEST_ASSERT_EQUAL(FW_INVALID, update_write(&manager, 200, NULL, 1));
    TEST_ASSERT_EQUAL(FW_INVALID, update_write(&manager, 200, image, 0));
    TEST_ASSERT_EQUAL(FW_INVALID, update_write(&manager, 200, image, 401));
    TEST_ASSERT_EQUAL(FW_INCOMPLETE, update_finish(&manager));
    boot_choice choice;
    TEST_ASSERT_EQUAL(FW_STATE, update_boot(&manager, &choice));
    reopen();
    TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
    TEST_ASSERT_EQUAL(1, choice.image.version);
    TEST_ASSERT_EQUAL(FW_OK, upload(2));
}

static void checksum_failure_keeps_confirmed_image_available(void)
{
    confirmed_baseline();
    TEST_ASSERT_EQUAL(FW_OK, update_begin(&manager, 2, sizeof(image), 123));
    TEST_ASSERT_EQUAL(FW_OK, update_write(&manager, 0, image, sizeof(image)));
    TEST_ASSERT_EQUAL(FW_CORRUPT, update_finish(&manager));
    TEST_ASSERT_EQUAL(FW_STATE, update_finish(&manager));
    reopen();
    boot_choice choice;
    TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
    TEST_ASSERT_EQUAL(1, choice.image.version);
}

static void corrupted_candidate_is_rejected_during_boot_and_confirmation(void)
{
    confirmed_baseline();
    TEST_ASSERT_EQUAL(FW_OK, upload(2));
    memory[image_address(manager.log.record.candidate.slot)] ^= 1;
    boot_choice choice;
    TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
    TEST_ASSERT_EQUAL(1, choice.image.version);
    TEST_ASSERT_EQUAL(FW_OK, upload(2));
    TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
    memory[image_address(choice.image.slot)] ^= 1;
    TEST_ASSERT_EQUAL(FW_CORRUPT, update_confirm(&manager, 2));
    reopen();
    TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
    TEST_ASSERT_EQUAL(1, choice.image.version);
}

static void corrupt_confirmed_image_and_unconfirmed_first_install_do_not_boot(void)
{
    TEST_ASSERT_EQUAL(FW_OK, upload(1));
    boot_choice choice;
    TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
    reopen();
    TEST_ASSERT_EQUAL(FW_NO_IMAGE, update_boot(&manager, &choice));
    TEST_ASSERT_EQUAL(FW_OK, upload(1));
    TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
    TEST_ASSERT_EQUAL(FW_OK, update_confirm(&manager, 1));
    memory[image_address(choice.image.slot)] ^= 1;
    TEST_ASSERT_EQUAL(FW_CORRUPT, update_boot(&manager, &choice));
}

static void metadata_crc_and_commit_marker_protect_against_partial_records(void)
{
    confirmed_baseline();
    TEST_ASSERT_EQUAL(FW_OK, upload(2));
    uint32_t latest = (uint32_t)manager.log.bank * FLASH_SECTOR_SIZE;
    memory[latest + 44] ^= 1;
    reopen();
    TEST_ASSERT_EQUAL(BOOT_STABLE, manager.log.record.phase);
    TEST_ASSERT_EQUAL(1, manager.log.record.confirmed.version);
    TEST_ASSERT_EQUAL(FW_OK, upload(2));
    latest = (uint32_t)manager.log.bank * FLASH_SECTOR_SIZE;
    memory[latest + 48] = 0xff;
    reopen();
    TEST_ASSERT_EQUAL(1, manager.log.record.confirmed.version);
    memory[0] = 0;
    memory[FLASH_SECTOR_SIZE] = 0;
    TEST_ASSERT_EQUAL(FW_CORRUPT, update_open(&manager, &flash));
    TEST_ASSERT_EQUAL(FW_STATE, update_begin(&manager, 3, 1, 0));
}

static void generation_order_survives_wraparound(void)
{
    manager.log.record.generation = UINT32_MAX - 1;
    TEST_ASSERT_EQUAL(FW_OK, journal_store(&manager.log, manager.log.record));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, manager.log.record.generation);
    TEST_ASSERT_EQUAL(FW_OK, journal_store(&manager.log, manager.log.record));
    reopen();
    TEST_ASSERT_EQUAL_UINT32(0, manager.log.record.generation);
    TEST_ASSERT_EQUAL(1, manager.log.bank);
}

static void every_upload_mutation_survives_power_loss_with_a_confirmed_fallback(void)
{
    confirmed_baseline();
    memcpy(backup, memory, sizeof(backup));
    TEST_ASSERT_EQUAL(FW_OK, upload(2));
    uint32_t mutations = nor.mutations;
    TEST_ASSERT_GREATER_THAN(3, mutations);
    const uint32_t tears[] = {0, 1, 2, 3, 4, 24, 47, 48, 128, 255, 256, 2048, 4095, 4096};
    for (uint32_t cut = 1; cut <= mutations; ++cut) {
        for (size_t t = 0; t < sizeof(tears) / sizeof(tears[0]); ++t) {
            memcpy(memory, backup, sizeof(backup));
            reopen();
            nor.cut_after = cut;
            nor.tear_bytes = tears[t];
            TEST_ASSERT_EQUAL(FW_POWER_LOSS, upload(2));
            reopen();
            boot_choice choice;
            TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
            TEST_ASSERT_TRUE(choice.image.version == 1 || choice.image.version == 2);
            const uint8_t *expected = choice.image.version == 1 ? baseline_image : image;
            TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, memory + image_address(choice.image.slot), sizeof(image));
            if (choice.trial) {
                reopen();
                TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
            }
            TEST_ASSERT_EQUAL(1, choice.image.version);
            TEST_ASSERT_FALSE(choice.trial);
        }
    }
}

static void interrupted_boot_and_confirm_commits_select_only_complete_images(void)
{
    confirmed_baseline();
    TEST_ASSERT_EQUAL(FW_OK, upload(2));
    memcpy(backup, memory, sizeof(backup));
    const uint32_t tears[] = {0, 1, 3, 4, 24, 48, 4096};
    for (unsigned operation = 0; operation < 3; ++operation) {
        for (uint32_t cut = 1; cut <= 3; ++cut) {
            for (size_t t = 0; t < sizeof(tears) / sizeof(tears[0]); ++t) {
                memcpy(memory, backup, sizeof(backup));
                reopen();
                boot_choice choice;
                if (operation != 0) TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
                nor.mutations = 0;
                nor.cut_after = cut;
                nor.tear_bytes = tears[t];
                fw_result result = operation == 1 ? update_confirm(&manager, 2) : update_boot(&manager, &choice);
                TEST_ASSERT_EQUAL(FW_POWER_LOSS, result);
                reopen();
                TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
                TEST_ASSERT_TRUE(choice.image.version == 1 || choice.image.version == 2);
                const uint8_t *expected = choice.image.version == 1 ? baseline_image : image;
                TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, memory + image_address(choice.image.slot), sizeof(image));
                if (operation == 2) TEST_ASSERT_EQUAL(1, choice.image.version);
            }
        }
    }
}

static void maximum_image_and_one_byte_image_can_be_confirmed(void)
{
    static uint8_t maximum[IMAGE_CAPACITY];
    for (size_t i = 0; i < sizeof(maximum); ++i) maximum[i] = (uint8_t)(i * 7);
    TEST_ASSERT_EQUAL(FW_OK, update_begin(&manager, 1, sizeof(maximum), crc32_update(0, maximum, sizeof(maximum))));
    TEST_ASSERT_EQUAL(FW_OK, update_write(&manager, 0, maximum, sizeof(maximum)));
    TEST_ASSERT_EQUAL(FW_OK, update_finish(&manager));
    boot_choice choice;
    TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
    TEST_ASSERT_EQUAL(FW_OK, update_confirm(&manager, 1));
    TEST_ASSERT_EQUAL(FW_OK, update_begin(&manager, UINT32_MAX, 1, crc32_update(0, image, 1)));
    TEST_ASSERT_EQUAL(FW_OK, update_write(&manager, 0, image, 1));
    TEST_ASSERT_EQUAL(FW_OK, update_finish(&manager));
    TEST_ASSERT_EQUAL(FW_OK, update_boot(&manager, &choice));
    TEST_ASSERT_EQUAL(FW_OK, update_confirm(&manager, UINT32_MAX));
    TEST_ASSERT_EQUAL(FW_INVALID, update_begin(&manager, 1, 1, 0));
}

static void flash_failures_stop_mutations_until_reopen(void)
{
    confirmed_baseline();
    nor.io_failure = true;
    TEST_ASSERT_EQUAL(FW_IO, update_begin(&manager, 2, 1, 0));
    nor.io_failure = false;
    TEST_ASSERT_EQUAL(FW_STATE, update_begin(&manager, 2, 1, 0));
    reopen();
    TEST_ASSERT_EQUAL(FW_OK, upload(2));
    nor.io_failure = true;
    boot_choice choice;
    TEST_ASSERT_EQUAL(FW_IO, update_boot(&manager, &choice));
    TEST_ASSERT_EQUAL(FW_IO, update_open(&manager, &flash));
}

static void invalid_public_arguments_are_rejected(void)
{
    TEST_ASSERT_EQUAL(FW_INVALID, update_open(NULL, &flash));
    TEST_ASSERT_EQUAL(FW_INVALID, update_open(&manager, NULL));
    TEST_ASSERT_EQUAL(FW_STATE, update_begin(NULL, 1, 1, 0));
    TEST_ASSERT_EQUAL(FW_STATE, update_write(NULL, 0, image, 1));
    TEST_ASSERT_EQUAL(FW_STATE, update_finish(NULL));
    TEST_ASSERT_EQUAL(FW_STATE, update_boot(NULL, NULL));
    TEST_ASSERT_EQUAL(FW_INVALID, update_boot(&manager, NULL));
    TEST_ASSERT_EQUAL(FW_STATE, update_confirm(NULL, 1));
    TEST_ASSERT_EQUAL(FW_INVALID, journal_load(NULL, &flash));
    TEST_ASSERT_EQUAL(FW_INVALID, journal_load(&manager.log, NULL));
    TEST_ASSERT_EQUAL(FW_STATE, journal_store(NULL, manager.log.record));
    boot_record invalid = manager.log.record;
    invalid.phase = BOOT_READY;
    TEST_ASSERT_EQUAL(FW_INVALID, journal_store(&manager.log, invalid));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(empty_flash_has_no_bootable_image);
    RUN_TEST(initial_image_is_trial_then_confirmed_across_restarts);
    RUN_TEST(unconfirmed_upgrade_rolls_back_without_touching_old_bytes);
    RUN_TEST(confirmed_upgrades_alternate_slots);
    RUN_TEST(invalid_sizes_versions_and_state_transitions_are_rejected);
    RUN_TEST(missing_duplicate_and_out_of_order_data_cannot_publish_an_image);
    RUN_TEST(checksum_failure_keeps_confirmed_image_available);
    RUN_TEST(corrupted_candidate_is_rejected_during_boot_and_confirmation);
    RUN_TEST(corrupt_confirmed_image_and_unconfirmed_first_install_do_not_boot);
    RUN_TEST(metadata_crc_and_commit_marker_protect_against_partial_records);
    RUN_TEST(generation_order_survives_wraparound);
    RUN_TEST(every_upload_mutation_survives_power_loss_with_a_confirmed_fallback);
    RUN_TEST(interrupted_boot_and_confirm_commits_select_only_complete_images);
    RUN_TEST(maximum_image_and_one_byte_image_can_be_confirmed);
    RUN_TEST(flash_failures_stop_mutations_until_reopen);
    RUN_TEST(invalid_public_arguments_are_rejected);
    return UNITY_END();
}
