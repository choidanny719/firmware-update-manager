#include "flash_file.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static fw_result write_all(int descriptor, const uint8_t *bytes, uint32_t address, size_t length)
{
    while (length) {
        ssize_t count = pwrite(descriptor, bytes, length, address);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return FW_IO;
        bytes += count;
        address += (uint32_t)count;
        length -= (size_t)count;
    }
    return FW_OK;
}

static fw_result save_region(void *context, uint32_t address, size_t length)
{
    flash_file *file = context;
    fw_result result = write_all(file->descriptor, file->nor.bytes + address, address, length);
    return result == FW_OK && fsync(file->descriptor) == 0 ? FW_OK : FW_IO;
}

fw_result flash_file_create(const char *path)
{
    if (!path) return FW_INVALID;
    int descriptor = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) return FW_IO;
    uint8_t erased[FLASH_SECTOR_SIZE];
    memset(erased, 0xff, sizeof(erased));
    fw_result result = FW_OK;
    for (uint32_t address = 0; result == FW_OK && address < FLASH_SIZE; address += sizeof(erased)) {
        result = write_all(descriptor, erased, address, sizeof(erased));
    }
    if (fsync(descriptor) != 0) result = FW_IO;
    close(descriptor);
    return result;
}

fw_result flash_file_open(flash_file *file, const char *path)
{
    if (!file || !path) return FW_INVALID;
    *file = (flash_file){.descriptor = -1};
    int descriptor = open(path, O_RDWR | O_NONBLOCK);
    if (descriptor < 0) return FW_IO;
    struct stat info;
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size != FLASH_SIZE ||
        flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        close(descriptor);
        return FW_IO;
    }
    uint8_t *bytes = malloc(FLASH_SIZE);
    if (!bytes) {
        close(descriptor);
        return FW_IO;
    }
    size_t offset = 0;
    while (offset < FLASH_SIZE) {
        ssize_t count = pread(descriptor, bytes + offset, FLASH_SIZE - offset, (off_t)offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            free(bytes);
            close(descriptor);
            return FW_IO;
        }
        offset += (size_t)count;
    }
    file->descriptor = descriptor;
    nor_reset(&file->nor, bytes);
    file->nor.persist = save_region;
    file->nor.persist_context = file;
    return FW_OK;
}

void flash_file_close(flash_file *file)
{
    if (!file) return;
    free(file->nor.bytes);
    if (file->descriptor >= 0) close(file->descriptor);
    *file = (flash_file){.descriptor = -1};
}
