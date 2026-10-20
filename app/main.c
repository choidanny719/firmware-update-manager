#include "flash_file.h"
#include "runtime.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int usage(FILE *stream, int code)
{
    fputs("Usage:\n"
          "  fw_update init FLASH\n"
          "  fw_update status FLASH\n"
          "  fw_update upload FLASH PACKETS [fault options]\n"
          "  fw_update boot FLASH [fault options]\n"
          "  fw_update confirm FLASH VERSION [fault options]\n"
          "Fault options: --cut-after N [--tear-bytes 0..4096] | --stuck-busy\n", stream);
    return code;
}

static bool number(const char *text, uint32_t maximum, uint32_t *output)
{
    if (!text || !isdigit((unsigned char)*text)) return false;
    errno = 0;
    char *end;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || *end || value > maximum) return false;
    *output = (uint32_t)value;
    return true;
}

static fw_result read_input(const char *path, uint8_t **bytes, size_t *length)
{
    int descriptor = open(path, O_RDONLY | O_NONBLOCK);
    if (descriptor < 0) return FW_IO;
    struct stat info;
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 || info.st_size > UPLOAD_STREAM_MAX) {
        close(descriptor);
        return FW_INVALID;
    }
    *length = (size_t)info.st_size;
    *bytes = malloc(*length ? *length : 1);
    if (!*bytes) {
        close(descriptor);
        return FW_IO;
    }
    size_t offset = 0;
    while (offset < *length) {
        ssize_t count = read(descriptor, *bytes + offset, *length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            free(*bytes);
            *bytes = NULL;
            close(descriptor);
            return FW_IO;
        }
        offset += (size_t)count;
    }
    close(descriptor);
    return FW_OK;
}

static int simple_result(fw_result result)
{
    printf("{\"result\":\"%s\"}\n", fw_result_name(result));
    return result == FW_OK ? 0 : 1;
}

static void print_record(const boot_record *record)
{
    const char *phases[] = {"stable", "ready", "trial"};
    printf(",\"phase\":\"%s\",\"generation\":%" PRIu32 ",\"confirmed_version\":%" PRIu32
           ",\"candidate_version\":%" PRIu32, phases[record->phase], record->generation,
           record->confirmed.version, record->candidate.version);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) return usage(stdout, 0);
    if (argc < 3) return usage(stderr, 2);
    bool initialize = strcmp(argv[1], "init") == 0;
    bool status = strcmp(argv[1], "status") == 0;
    bool upload = strcmp(argv[1], "upload") == 0;
    bool boot = strcmp(argv[1], "boot") == 0;
    bool confirm = strcmp(argv[1], "confirm") == 0;
    if (!initialize && !status && !upload && !boot && !confirm) return usage(stderr, 2);
    if ((initialize || status) && argc != 3) return usage(stderr, 2);
    if ((upload || confirm) && argc < 4) return usage(stderr, 2);
    uint32_t version = 0, cut_after = 0, tear_bytes = 0;
    if (confirm && (!number(argv[3], UINT32_MAX, &version) || version == 0)) return usage(stderr, 2);
    unsigned seen = 0;
    bool stuck_busy = false;
    for (int i = upload || confirm ? 4 : 3; i < argc; ++i) {
        if (strcmp(argv[i], "--stuck-busy") == 0 && !(seen & 4)) {
            seen |= 4;
            stuck_busy = true;
        } else if (strcmp(argv[i], "--cut-after") == 0 && !(seen & 1) && i + 1 < argc) {
            seen |= 1;
            if (!number(argv[++i], UINT32_MAX, &cut_after) || !cut_after) return usage(stderr, 2);
        } else if (strcmp(argv[i], "--tear-bytes") == 0 && !(seen & 2) && i + 1 < argc) {
            seen |= 2;
            if (!number(argv[++i], FLASH_SECTOR_SIZE, &tear_bytes)) return usage(stderr, 2);
        } else return usage(stderr, 2);
    }
    if (((seen & 2) && !(seen & 1)) || ((seen & 4) && (seen & 3))) return usage(stderr, 2);
    if (initialize) return simple_result(flash_file_create(argv[2]));
    flash_file file;
    fw_result result = flash_file_open(&file, argv[2]);
    if (result != FW_OK) return simple_result(result);
    file.nor.cut_after = cut_after;
    file.nor.tear_bytes = tear_bytes;
    file.nor.stuck_busy = stuck_busy;
    flash_device flash = {nor_transfer, &file.nor, NULL, 100};
    update_manager manager = {0};
    runtime_report report = {0};
    boot_choice choice = {0};
    if (upload) {
        uint8_t *bytes = NULL;
        size_t length;
        result = read_input(argv[3], &bytes, &length);
        if (result == FW_OK) result = runtime_upload(&file.nor, bytes, length, &report);
        free(bytes);
        manager = report.manager;
    } else {
        result = update_open(&manager, &flash);
        if (result == FW_OK && boot) result = update_boot(&manager, &choice);
        if (result == FW_OK && confirm) result = update_confirm(&manager, version);
    }
    printf("{\"result\":\"%s\",\"mutations\":%" PRIu32, fw_result_name(result), file.nor.mutations);
    if (result == FW_OK) print_record(&manager.log.record);
    if (result == FW_OK && boot) {
        printf(",\"selected_version\":%" PRIu32 ",\"slot\":%" PRIu32 ",\"trial\":%s",
               choice.image.version, choice.image.slot, choice.trial ? "true" : "false");
    }
    if (upload) {
        printf(",\"received_bytes\":%" PRIu32 ",\"receiver_packets\":%" PRIu32
               ",\"controller_packets\":%" PRIu32 ",\"spi_commands\":%" PRIu32 ",\"queue_high_water\":%" PRIu32,
               report.manager.received, report.receiver_packets, report.controller_packets,
               report.spi_commands, report.queue_high_water);
    }
    puts("}");
    flash_file_close(&file);
    return result == FW_OK ? 0 : 1;
}
