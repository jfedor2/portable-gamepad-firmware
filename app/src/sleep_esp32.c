// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <driver/rtc_io.h>
#include <soc/soc_caps.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>

#include "chk.h"
#include "sleep.h"

LOG_MODULE_DECLARE(pgf);

#if SOC_PM_SUPPORT_EXT1_WAKEUP
static gpio_num_t pad_number(const struct gpio_dt_spec* spec) {
#if DT_NODE_EXISTS(DT_NODELABEL(gpio1))
    if (spec->port == DEVICE_DT_GET(DT_NODELABEL(gpio1))) {
        return (gpio_num_t) (spec->pin + 32);
    }
#endif
    return (gpio_num_t) spec->pin;
}
#endif

void sleep_init(const struct gpio_dt_spec* wake_button) {
#if SOC_PM_SUPPORT_EXT1_WAKEUP
    gpio_num_t pad = pad_number(wake_button);
    if (rtc_gpio_is_valid_gpio(pad)) {
        // Undo what enter_sleep() did before the last deep sleep.
        rtc_gpio_hold_dis(pad);
        rtc_gpio_deinit(pad);
    }
#endif
}

void enter_sleep(const struct gpio_dt_spec* wake_button) {
    // Unlike on other platforms, no GPIO interrupt is configured here: the
    // deep sleep wakeup was already set up when the pin was configured with
    // GPIO_INT_WAKEUP. A level interrupt would also fire continuously once
    // the pad is routed to the LP IO block below.

#if SOC_PM_SUPPORT_EXT1_WAKEUP
    // The ESP-IDF sleep code only holds EXT1 wakeup pads when the RTC peripheral
    // power domain is off, but Zephyr's sys_poweroff() keeps it on. On the
    // ESP32-C6 the pad's pull-up doesn't survive deep sleep anyway and the pin
    // floats low, waking the chip right back up. So route the pad to the LP IO
    // block with its pull-up ourselves and hold that configuration.
    gpio_num_t pad = pad_number(wake_button);
    if (rtc_gpio_is_valid_gpio(pad)) {
        CHK(rtc_gpio_init(pad));
        CHK(rtc_gpio_set_direction(pad, RTC_GPIO_MODE_INPUT_ONLY));
        CHK(rtc_gpio_pulldown_dis(pad));
        CHK(rtc_gpio_pullup_en(pad));
        CHK(rtc_gpio_hold_en(pad));
    }
#endif

    sys_poweroff();
}
