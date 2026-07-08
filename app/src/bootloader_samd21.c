#include "bootloader.h"

#include <stdint.h>

#include <zephyr/sys/reboot.h>

#define BOOT_DOUBLE_TAP_ADDRESS 0x20007FFC
#define DBL_TAP_MAGIC 0xF01669EF

void reset_to_bootloader() {
    *((volatile uint32_t*) BOOT_DOUBLE_TAP_ADDRESS) = DBL_TAP_MAGIC;

    sys_reboot(SYS_REBOOT_COLD);
}
