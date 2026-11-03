// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <fsl_device_registers.h>
#include <zephyr/devicetree.h>

#include "usb_vbus.h"

// udc_mcux_ehci only reports VBUS on the B-session valid change interrupt,
// so it's never reported if VBUS is already present when the controller is
// initialized (which is always the case on a USB-powered board like the
// Teensy).
bool vbus_present_unreported(void) {
    USBHS_Type* usb = (USBHS_Type*) DT_REG_ADDR(DT_NODELABEL(zephyr_udc0));
    return (usb->OTGSC & USBHS_OTGSC_BSV_MASK) != 0;
}
