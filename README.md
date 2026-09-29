<div align="center">

# AM32 Programmer · ESP32-S3

**A USB programmer for 4-in-1 and single ESCs, built on a Seeed XIAO ESP32-S3.**

Configure and flash AM32, BLHeli_32 and BLHeli_S ESCs without a flight controller.

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.3%2B-E7352C?logo=espressif&logoColor=white)
![Target](https://img.shields.io/badge/target-ESP32--S3-blue)
![Protocol](https://img.shields.io/badge/protocol-BLHeli%204--way-orange)

</div>

---

## How it works

To the configurator, the programmer looks like a Betaflight flight controller. It answers MSP until the configurator asks for passthrough, then switches the same USB port over to the BLHeli 4-way protocol. The ESC index the configurator sends picks which of the four signal pads is used. One USB cable reaches all four ESCs of a 4-in-1.

```mermaid
flowchart LR
    A[Configurator<br/>web / desktop] -- USB CDC --> B[MSP]
    B -- SET_PASSTHROUGH --> C[4-way<br/>interface]
    C --> D[Bootloader<br/>protocol]
    D -- UART1 via GPIO matrix --> E1[ESC 1]
    D --> E2[ESC 2]
    D --> E3[ESC 3]
    D --> E4[ESC 4]
```

**Works with:** [AM32 Configurator](https://am32.ca), [ESC Configurator](https://esc-configurator.com), BLHeliSuite32 and the Betaflight Configurator's ESC tab.

## Hardware

<p align="center">
  <a href="schematic.pdf"><img src="docs/schematic.png" alt="Schematic: AM32 Programming Breakout for XIAO ESP32-S3, Rev 0.1" width="760"></a>
</p>

The breakout board (KiCad, Rev 0.1, [PDF](schematic.pdf)) has six ESC channels. The firmware drives the first four by default.

| ESC | XIAO pin | GPIO | Enabled |
|:---:|:---:|:---:|:---:|
| 1 | D5 | 6 | ✅ |
| 2 | D4 | 5 | ✅ |
| 3 | D3 | 4 | ✅ |
| 4 | D10 | 9 | ✅ |
| 5 | D9 | 8 | – |
| 6 | D8 | 7 | – |

To use channels 5 and 6, set `ESC4W_ESC_COUNT` to `6` and `ESC4W_ESC_PINS` to `{ 6, 5, 4, 9, 8, 7 }` in [`main/config.h`](main/config.h).

**Connectors**

| Ref | Purpose | Pinout |
|---|---|---|
| CN1 | 4-in-1 ESC harness | 1 GND · 2–7 ESC1–6 · 8 n/c |
| J1–J6 | single ESCs, JR servo plug | 1 GND · 2 n/c · 3 signal |
| J7 | UART0 console (115200) | 1 GND · 2 TX (D6) · 3 RX (D7) |

> [!CAUTION]
> Standard FC-to-ESC harnesses carry battery voltage, current sense and telemetry on some pins. Check your harness against the CN1 pinout before plugging it in. Battery voltage on an ESC line goes straight to a GPIO.

**Signal conditioning**

- **R1–R6, 470 Ω in series:** limits current when both sides drive the line at once, and protects the GPIO. At 19200 baud the added delay is negligible.
- **No external pull-up.** The line idles high through the ESP32's internal pull-up (`ESC4W_RX_INTERNAL_PULLUP`), which is enough for short leads.
- **D1–D6 (BAT54S clamps) are optional.** The board works without them.

> [!WARNING]
> **Rev 0.1: do not fit D1–D6.** The clamps are drawn reversed: pin 1 (anode) goes to 3V3 and pin 2 (cathode) to GND. Fitted as drawn, they short the 3V3 rail. For a correct clamp, pin 1 goes to GND, pin 2 to 3V3 and pin 3 (common) to the signal line.

**Power:** the programmer never powers the ESC; J1–J6 leave the centre pin unconnected. Power the ESC from its own supply with props off. Ground is shared through the connectors.

| Onboard LED | Meaning |
|---|---|
| off | waiting for the configurator |
| on | passthrough session active |
| flashing | reading or writing ESC memory |

Pins, timings and USB identity are all set in [`main/config.h`](main/config.h).

## Build & flash

```bash
idf.py set-target esp32s3
idf.py build flash
```

Requires ESP-IDF v5.3 or newer; tested on v6.0.1. The component manager fetches `esp_tinyusb` automatically.

> [!TIP]
> After the first flash, the app takes over the USB port, so esptool can no longer reset the board into download mode. To reflash, **hold BOOT, tap RESET, release BOOT**, then flash as usual. Boot messages and panics appear on the J7 console header.

> [!IMPORTANT]
> If an old `sdkconfig` exists, delete it before building. `sdkconfig.defaults` sets the 1 kHz tick and keeps the console off USB, and both are required.

## Why the S3 and not the C3?

The AM32 Configurator only lists serial ports whose USB vendor ID is on a built-in allow-list. Espressif's `0x303A` isn't on it. The C3's USB descriptors are fixed in ROM. The S3 has a USB-OTG peripheral, so TinyUSB can present as an STM32 virtual COM port (`0483:5740`), the same as a real flight controller.

> [!WARNING]
> That vendor ID belongs to STMicroelectronics. It is fine for a bench tool, but don't ship a product with it.

## Diagnostic trace

The board shows up as **two** serial ports. Interface 0 carries the protocol for the configurator. Interface 2 is a readable trace of every MSP request, 4-way command and bootloader exchange:

```bash
tio /dev/serial/by-id/usb-STMicroelectronics_STM32_Virtual_ComPort_esc4way-s3-if02
```

A successful command ends with `-> ack 00`. Turn the trace off with `ESC4W_LOG_ENABLE` in `config.h`.

## Safety

Every frame is checked by a CRC on both hops, from the configurator to the programmer and from the programmer to the ESC. A corrupted transfer returns an error instead of being written. If a flash write fails partway, the ESC's bootloader is left intact and you can flash it again. The real risk is flashing firmware built for the wrong target.

**First time?** Read the ESCs, then change one setting, then reflash the version already installed, and only then try a new firmware.

## Credits

The 4-way interface and bootloader protocol are ported from [Betaflight](https://github.com/betaflight/betaflight) (`serial_4way.c`, `serial_4way_avrootloader.c`). The bit-banged UART is replaced by a hardware UART whose signals are routed through the GPIO matrix to the selected pad.
