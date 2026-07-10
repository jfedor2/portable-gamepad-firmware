_If you're looking for the Slimbox BT firmware, this is it. It has been merged with the Portable Gamepad Firmware project._

# Portable Gamepad Firmware

This repository contains code for wired/wireless game controller firmware that runs on many different microcontrollers.

It uses Zephyr and should in theory run on any platform it supports. It has been tested on: RP2040, RP2350, nRF52840, nRF52832, nRF54L15, nRF54LM20, SAMD21, STM32G0B1, ESP32-S3, MIMXRT1062. The wireless function currently only works on Nordic's nRF52 and nRF54L chips. For a full list of pre-built binaries, see the [latest release](https://github.com/jfedor2/portable-gamepad-firmware/releases/latest/).

## How to use

One of the buttons on the controller is designated as the "system button". Currently on most of the provided builds it is the `start` button. If you make a custom build you can change it to any other button or even have a dedicated system button that isn't shared with any of the gamepad buttons. On the Flatbox controllers the `start` button (and therefore the system button) is the button in the top left corner.

When the controller is connected over USB, the Bluetooth connection is disabled.

When the controller is not connected over USB, it will go to sleep after 10 minutes of inactivity when connected over Bluetooth and after 1 minute when not connected. To wake it up press the system button.

When connected over USB, to put the controller in firmware flashing mode, press the system button for 10 seconds. (Please note this currently works on RP2040, RP2350, SAMD21 and nRF52840 devices that have a UF2 bootloader.)

When not connected over USB, to turn the controller off press the system button for 3 seconds, and to put the controller in pairing mode press the system button for 10 seconds.

If your wireless controller has an LED, it will blink in different patterns, depending on whether the controller is connected, trying to connect, or in pairing mode.

The controller can be paired with one device at a time.

## Input modes

The firmware currently has three input modes. To select an input mode, hold one of the face buttons while turning on or plugging in the controller.

Input mode | Button to hold
---------- | --------------
Stadia | `south`
Switch | `east`
PC | `west`

The controller will remember the last selected input mode. Please note that on battery-powered devices you need to switch them off and back on, holding a button while plugging the device in won't be enough if it's currently on.

Switch mode works on the Nintendo Switch 1 and 2 when wired. To use it on Switch consoles wirelessly, you can pair it with a [Blue Wire Bridge](https://github.com/jfedor2/blue-wire-bridge) dongle.

PC mode works as an Xbox 360 controller when wired and as an Xbox Wireless Controller when wireless. This lets you use it in Windows games that use XInput.

The default mode is Stadia, which should work on a variety of platforms, including mobile.

Please note that when you switch the input mode on a wireless controller, you will likely need to unpair and re-pair it because most platforms can't handle the device's identity change properly.

## Flashing the firmware

Assuming you're using one of the devices that come with a UF2 bootloader and for which pre-built binaries are provided, first you have to put your board in firmware flashing mode. On RP2040 and RP2350 based boards, this is done by holding the BOOTSEL button while plugging the device in. On some other boards it's done by pressing the RESET button twice quickly. If you succeed, a USB drive should appear on your computer. The name of the drive will depend on what device you're using. Download the appropriate UF2 file from the releases section (e.g. `pgf-feather_nrf52840.uf2` for the Adafruit Feather nRF52840 Express or `pgf-pico.uf2` for the Raspberry Pi Pico) and copy it to the drive that appeared.

If you already have some previous version of this firmware on your board, on some boards you can hold the system button for 10 seconds to enter firmware flashing mode.

For devices that don't come with a UF2 bootloader the procedure will be different, you might have to use OpenOCD or pyOCD or some other board-specific software to flash the firmware. In some cases additional debugger hardware may be required.

## Pinout

For boards not listed here, check the devicetree overlay files in [app/boards](app/boards).

<details>
<summary>Raspberry Pi Pico</summary>

If you're using a Raspberry Pi Pico or Pico 2, wire the buttons to pins on the board as follows:

pin | button
--- | ------
GPIO6 | south
GPIO7 | east
GPIO10 | west
GPIO11 | north
GPIO5 | D-pad left
GPIO4 | D-pad right
GPIO2 | D-pad up
GPIO3 | D-pad down
GPIO13 | L1
GPIO12 | R1
GPIO9 | L2
GPIO8 | R2
GPIO18 | L3
GPIO19 | R3
GPIO16 | select
GPIO17 | start
GPIO20 | home
GPIO21 | button 14
</details>

<details>
<summary>Adafruit Feather nRF52840 Express</summary>

If you're using an Adafruit Feather nRF52840 Express board, wire the buttons to pins on the board as follows:

pin | button
--- | ------
A5 | south
A4 | east
D2 | west
MI | north
10 | D-pad left
6 | D-pad right
A0 | D-pad up
9 | D-pad down
SCK | L1
MO | R1
A2 | L2
A3 | R2
12 | L3
13 | R3
SDA | select
11 | start
SCL | home
5 | button 14
</details>

<details>
<summary>Seeed Xiao</summary>

If you're using one of the standalone Xiao builds (not as part of Flatbox rev7), wire the buttons to pins on the board as follows:

pin | button
--- | ------
D0 | start
D1 | select
D2 | home
D3 | south
D4 | east
D5 | west
D6 | north
D7 | D-pad left
D8 | D-pad right
D9 | D-pad up
D10 | D-pad down
</details>

<details>
<summary>nice!nano and clones</summary>

If you're using a nice!nano board or one of its many clones, wire the buttons to pins on the board as follows:

pin | button
--- | ------
006 | start
008 | select
017 | D-pad left
020 | D-pad right
022 | D-pad up
024 | D-pad down
100 | south
011 | east
104 | west
106 | north
009 | L1
010 | R1
111 | L2
113 | R2
115 | L3
002 | R3
029 | home
031 | button 14
</details>

## How to build

The easiest way to compile the firmware is to let GitHub do it for you. This repository has GitHub Actions that build the firmware, so you can just fork, enable Actions, make your changes, wait for the job to complete, and look for the binaries in the artifacts produced.

To compile it on your own machine, you will need a Zephyr build environment. You can set it up yourself or you can use Docker. Either way run `./build.py builds.json` and take a look at the commands it generates. With Docker, a command like this builds all existing variants (start from the top level of the repository or adjust the path accordingly):

```
./build.py builds.json > build.sh
chmod +x build.sh
docker run --rm -v $(pwd):/workspace/project -w /workspace/project ghcr.io/zephyrproject-rtos/ci:v0.28.6 ./build.sh
```

## Building for new platforms

If you want to build for a board that we don't currently have a build for, first you need a Zephyr board definition. If you're lucky, Zephyr already has a definition. If you're not lucky or you're building for a custom board you made, you will have to create the board definition yourself. Usually you can look at definitions for boards that use the same chip and go from there.

Once you have a board definition, the only other thing you need is a devicetree overlay with the button mapping. Look at some of the overlay files in [app/boards](app/boards) to get an idea of what it might look like. The only required button is the "system" button.

You might also want to define a `status-led` alias.

For the input mode to be persisted, you will need a storage partition in your board definition. If you just want to test if everything else is working, you can set `CONFIG_NVS=n`. Setting the input mode will still work, but it won't be remembered.

## TODO

* analog inputs for sticks and triggers
* battery level reporting

## License

The software in this repository is licensed under the [MIT License](LICENSE).
