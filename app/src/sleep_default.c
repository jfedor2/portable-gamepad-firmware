// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>

#include "chk.h"
#include "sleep.h"

LOG_MODULE_DECLARE(pgf);

void sleep_init(const struct gpio_dt_spec* wake_button) {
}

void enter_sleep(const struct gpio_dt_spec* wake_button) {
    CHK(gpio_pin_interrupt_configure_dt(wake_button, GPIO_INT_LEVEL_ACTIVE));
#ifdef CONFIG_POWEROFF
    sys_poweroff();
#endif
}
