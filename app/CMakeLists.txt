cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pgf)

target_sources(app PRIVATE
    src/main.c
)

if ("${CONFIG_BUILD_OUTPUT_UF2_FAMILY_ID}" STREQUAL "0xada52840")
    target_sources(app PRIVATE src/bootloader_nrf52840.c)
endif()

if (CONFIG_SOC_SERIES_RP2040 OR CONFIG_SOC_SERIES_RP2350)
    target_sources(app PRIVATE src/bootloader_rp2040.c)
endif()

if ("${CONFIG_BUILD_OUTPUT_UF2_FAMILY_ID}" STREQUAL "0x68ed2b88")
    target_sources(app PRIVATE src/bootloader_samd21.c)
endif()
