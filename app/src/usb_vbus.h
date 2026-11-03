#ifndef _USB_VBUS_H_
#define _USB_VBUS_H_

#include <stdbool.h>

// Some USB device controller drivers claim they can detect VBUS, but only
// report it when it changes, so if it's already present when the driver
// starts (on a USB-powered board, or after a soft reset) we never get
// USBD_MSG_VBUS_READY. On those platforms this reads the VBUS state from
// the hardware directly. Everywhere else it returns false and we rely on
// the driver's events.
bool vbus_present_unreported(void);

#endif
