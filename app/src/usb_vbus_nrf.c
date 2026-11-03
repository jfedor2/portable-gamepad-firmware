// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <nrfx_power.h>

#include "usb_vbus.h"

// udc_nrf only reports VBUS on the POWER peripheral's USBDETECTED event,
// which doesn't fire after a soft reset (e.g. when the UF2 bootloader starts
// the application) with the cable already connected.
bool vbus_present_unreported(void) {
    return nrfx_power_usbstatus_get() != NRFX_POWER_USB_STATE_DISCONNECTED;
}
