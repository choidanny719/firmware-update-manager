#ifndef RESULT_H
#define RESULT_H

typedef enum {
    FW_OK,
    FW_INVALID,
    FW_IO,
    FW_TIMEOUT,
    FW_PROTECTED,
    FW_CORRUPT,
    FW_STATE,
    FW_SEQUENCE,
    FW_INCOMPLETE,
    FW_NO_IMAGE,
    FW_POWER_LOSS
} fw_result;

const char *fw_result_name(fw_result result);

#endif
