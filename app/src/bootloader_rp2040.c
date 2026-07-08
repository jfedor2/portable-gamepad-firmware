#include "bootloader.h"

#include <pico/bootrom.h>

void reset_to_bootloader() {
    reset_usb_boot(0, 0);
}
