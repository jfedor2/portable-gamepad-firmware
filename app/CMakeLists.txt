cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pgf)

target_sources(app PRIVATE
    src/battery.c
    src/main.c
)

if (CONFIG_PGF_BATTERY_LEVEL_REPORTING_VBATT)
    target_sources(app PRIVATE src/battery_vbatt.c)
elseif (CONFIG_PGF_BATTERY_LEVEL_REPORTING_PMIC)
    target_sources(app PRIVATE src/battery_pmic.c)
else()
    target_sources(app PRIVATE src/battery_null.c)
endif()

if (CONFIG_ADC)
    target_sources(app PRIVATE src/analog_axes.c)
endif()

if ("${CONFIG_BUILD_OUTPUT_UF2_FAMILY_ID}" STREQUAL "0xada52840")
    target_sources(app PRIVATE src/bootloader_nrf52840.c)
endif()

if (CONFIG_BT_HIDS)
    target_sources(app PRIVATE src/hids_ncs.c)
elseif (CONFIG_BT)
    target_sources(app PRIVATE src/hids_zephyr.c)
endif()

if (CONFIG_SOC_FAMILY_SILABS_S2)
    target_sources(app PRIVATE src/sleep_silabs.c)
elseif (CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
    target_sources(app PRIVATE src/sleep_esp32.c)
else()
    target_sources(app PRIVATE src/sleep_default.c)
endif()

if (CONFIG_UDC_NRF)
    target_sources(app PRIVATE src/usb_vbus_nrf.c)
elseif (CONFIG_UDC_NXP_EHCI)
    target_sources(app PRIVATE src/usb_vbus_mcux_ehci.c)
else()
    target_sources(app PRIVATE src/usb_vbus_default.c)
endif()

if (CONFIG_SOC_SERIES_RP2040 OR CONFIG_SOC_SERIES_RP2350)
    target_sources(app PRIVATE src/bootloader_rp2040.c)
endif()

if ("${CONFIG_BUILD_OUTPUT_UF2_FAMILY_ID}" STREQUAL "0x68ed2b88")
    target_sources(app PRIVATE src/bootloader_samd21.c)
endif()
