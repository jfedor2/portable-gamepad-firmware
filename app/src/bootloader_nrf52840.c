#include "bootloader.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(bootloader_nrf52840, LOG_LEVEL_DBG);

#define CHK(X) ({ int err = X; if (err != 0) { LOG_ERR("%s returned %d (%s:%d)", #X, err, __FILE__, __LINE__); } err == 0; })

static const struct device* gpregret_dev = DEVICE_DT_GET(DT_NODELABEL(gpregret1));

void reset_to_bootloader() {
    if (!device_is_ready(gpregret_dev)) {
        LOG_ERR("GPREGRET device not ready.");
        return;
    }

    // https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/master/src/main.c#L112
    uint8_t dfu_magic_uf2_reset = 0x57;

    // Save the magic value and reboot, the bootloader will see it and enter UF2 mode.
    if (CHK(retained_mem_write(gpregret_dev, 0, &dfu_magic_uf2_reset, 1))) {
        sys_reboot(SYS_REBOOT_WARM);
    }
}
