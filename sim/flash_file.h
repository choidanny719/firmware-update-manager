#ifndef FLASH_FILE_H
#define FLASH_FILE_H

#include "nor.h"

typedef struct {
    nor_model nor;
    int descriptor;
} flash_file;

fw_result flash_file_create(const char *path);
fw_result flash_file_open(flash_file *file, const char *path);
void flash_file_close(flash_file *file);

#endif
