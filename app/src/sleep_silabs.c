// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <em_emu.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

#include "chk.h"
#include "sleep.h"

LOG_MODULE_DECLARE(pgf);

void sleep_init(const struct gpio_dt_spec* wake_button) {
}

void enter_sleep(const struct gpio_dt_spec* wake_button) {
    // Silicon Labs' GPIO interrupt controller doesn't support level-triggered
    // interrupts (only edge), and it checks GPIO_INT_WAKEUP here.
    CHK(gpio_pin_interrupt_configure_dt(wake_button, GPIO_INT_EDGE_TO_ACTIVE | GPIO_INT_WAKEUP));

    // sys_poweroff() isn't implemented so enter EM4 directly instead
    (void) irq_lock();
    EMU_EnterEM4();
}
