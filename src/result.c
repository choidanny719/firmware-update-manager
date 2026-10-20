#include "result.h"

const char *fw_result_name(fw_result result)
{
    switch (result) {
        case FW_OK: return "ok";
        case FW_INVALID: return "invalid_input";
        case FW_IO: return "io_error";
        case FW_TIMEOUT: return "timeout";
        case FW_PROTECTED: return "write_protected";
        case FW_CORRUPT: return "corrupt_data";
        case FW_STATE: return "invalid_state";
        case FW_SEQUENCE: return "out_of_sequence";
        case FW_INCOMPLETE: return "incomplete_image";
        case FW_NO_IMAGE: return "no_bootable_image";
        case FW_POWER_LOSS: return "power_loss";
    }
    return "unknown_error";
}
