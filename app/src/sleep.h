#ifndef _SLEEP_H_
#define _SLEEP_H_

#include <zephyr/drivers/gpio.h>

// Undoes any pin state enter_sleep() left behind on wake_button. Called on
// startup, before the buttons are configured.
void sleep_init(const struct gpio_dt_spec* wake_button);

// Configures wake_button for wake-on-interrupt and puts the system to sleep.
void enter_sleep(const struct gpio_dt_spec* wake_button);

#endif
