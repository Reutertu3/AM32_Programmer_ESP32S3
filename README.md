# esc4way — ESP32-S3 / ESP32-C3 USB programmer for 4-in-1 ESCs

Speaks MSP over native USB CDC, then switches the same endpoint into the
BLHeli 4-way protocol. Four signal pins, one serial port, ESC selected by
the index parameter of `cmd_DeviceInitFlash` — which is the part a plain
one-wire adapter cannot do.

Works with ESC Configurator (web), BLHeliSuite32, and the Betaflight
Configurator's ESC tab. Covers BLHeli_S (SiLabs), BLHeli_32 and AM32 (ARM).

## Hardware

Default target is the **ESP32-S3**. Set `ESC4W_USB_TINYUSB` to 0 in
`config.h` for the C3 — see "Why the S3" below for what you give up.

| Function | ESP32-S3 | ESP32-C3 |
|---|---|---|
| USB D− / D+ | GPIO19 / GPIO20 | GPIO18 / GPIO19 |
| ESC 1..4 signal | GPIO4, 5, 6, 7 | GPIO3, 4, 5, 6 |
| Debug console | GPIO43 / GPIO44 | GPIO20 / GPIO21 |
| Avoid | 0, 3, 45, 46 (strapping); 26–32 (flash); 33–37 (octal PSRAM) | 2, 8, 9 (strapping); 11–17 (flash) |

GPIO22–25 do not exist on the S3.

Add a 10 kΩ pull-up to 3V3 on each signal line. The internal pull-up is
weak (~45 kΩ), which is marginal on anything longer than a few
centimetres at 19200 baud.

The ESC needs its own power (battery, props off). This board does not and
must not power it. Ground must be common.

## Why the S3

The AM32 Configurator only offers ports whose USB vendor ID appears on a
hardcoded allow-list in `components/SerialDevice.vue`:

```js
const usbFCVendorIds     = [0x0483, 0x2E3C, 0x2E8A, 0x1209, 0x26AC,
                            0x27AC, 0x2DAE, 0x3162, 0x35A7, 0x28E9];
const usbDirectVendorIds = [0x1A86, 0x0403, 0x4348, 0x26BA, 0x10C4];
```

Espressif's `0x303A` is on neither list, so a stock ESP32 is filtered out
of the Chrome port picker and the tool never sends a byte.

The C3's USB Serial/JTAG descriptors live in ROM — no eFuse, no
sdkconfig option, no way to change them. The S3 has a USB-OTG peripheral,
so TinyUSB can present any identity; `config.h` defaults to STM's Virtual
COM Port (`0483:5740`), which is on the list and is what every
Betaflight-derived tool expects from a flight controller.

That VID is not yours. Fine for a bench tool, not for anything you ship.
`esc-configurator.com` calls `requestPort()` with no filters at all and
works with either chip.

## Build

```
idf.py set-target esp32s3      # or esp32c3, with ESC4W_USB_TINYUSB = 0
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

ESP-IDF v5.3 or newer (the `esp_driver_*` components `main` requires by
name were split out in 5.3); tested against v6.0.1.
`main/idf_component.yml` pulls `espressif/esp_tinyusb` automatically on
first build.

If `sdkconfig` already exists from an earlier build, delete it —
`sdkconfig.defaults` is only consulted when `sdkconfig` is first
generated, and the settings below are not optional.

### sdkconfig

- `CONFIG_FREERTOS_HZ=1000` — the bootloader link uses 2–5 ms timeouts;
  the default 100 Hz tick cannot express them.
- Console on UART0, `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` — if the
  console shares the USB Serial/JTAG endpoint, log text lands in the
  middle of MSP frames and the handshake fails. This is the single most
  likely cause of "device not recognised".

### Flashing the S3

Once the app claims USB-OTG with a custom VID/PID, esptool's auto-reset
over that connector stops working. Two options:

- Use the DevKitC-1's second connector, wired to UART0. This is the
  simple answer and also gives you the console.
- Or force ROM download mode on the USB connector: hold BOOT, tap RESET,
  release BOOT. The ROM re-enables USB Serial/JTAG, so the board comes
  back as `303a:1001` and flashes normally.

A monitor session left open on the port will block upload either way.

## Layout

| File | Role |
|---|---|
| `config.h` | target select, pins, timings, USB and MSP identity |
| `main/idf_component.yml` | pulls `espressif/esp_tinyusb` for the S3 backend |
| `host_link.c` | USB CDC transport: TinyUSB (S3) or USB Serial/JTAG (C3) |
| `msp.c` | MSP v1 + v2 responder, including `MSP_SET_PASSTHROUGH` (245) |
| `serial_4way.c` | 4-way framing and command dispatch |
| `blheli_bootloader.c` | AVRootloader device protocol |
| `esc_io.c` | half-duplex one-wire link, UART matrixed across four pads |

## Design notes

**One UART, four pads.** Rather than bit-banging, the UART1 TX and RX
signals are re-routed through the GPIO matrix to whichever ESC pin is
selected. TX is attached only while transmitting, with the pad in
`INPUT_OUTPUT` mode so the local echo is received and can be discarded
deterministically. This is far more robust under FreeRTOS than
reproducing Betaflight's interrupt-sensitive `suart`.

**Two different CRCs.** Easy to get wrong:

- Host ↔ interface framing: CRC16-XMODEM (poly 0x1021, init 0),
  transmitted **high byte first**.
- Interface ↔ bootloader: CRC16-IBM (poly 0xA001, reflected, init 0),
  transmitted **low byte first**, and only present after a successful
  connect handshake.

**Connect handshake runs without CRC.** `bl_send`/`bl_read` append and
expect a CRC only when `bl_is_connected()` — exactly as upstream.

**Keep-alive expects a rejection.** `CMD_KEEP_ALIVE` returns
`brERRORCOMMAND` from a live bootloader. That rejection is the proof of
life; any other response means the link is gone.

## Not implemented

- **STK500v2 / SimonK** (`imSK`). Atmel-based ESCs only. Returns
  `ACK_I_INVALID_PARAM` / `ACK_I_INVALID_CMD`. Port
  `serial_4way_stk500v2.c` if you ever need it.
- **`cmd_DeviceEraseAll`** and **`cmd_DeviceC2CK_LOW`** — SimonK and C2
  paths respectively.
- Not needed for AM32 or BLHeli_32.

## Diagnostic trace

With `ESC4W_LOG_ENABLE` (S3 only) the adapter enumerates as **two**
serial ports: USB interface 0 carries MSP/4-way for the configurator,
interface 2 carries a human-readable trace. Don't trust `ttyACM0/1`:
Linux keeps a ttyACM number reserved while any program still has the
old device open, so after a reflash the pair can come back as
ACM1/ACM2. Use the stable names instead:

```
ls -l /dev/serial/by-id/
tio /dev/serial/by-id/usb-STMicroelectronics_STM32_Virtual_ComPort_esc4way-s3-if02
```

`-if00` is the configurator port, `-if02` the trace. Opening the trace
port prints a banner identifying it, so you know you are on the right
one. After that it logs every MSP request, every 4-way command, and for
each bootloader connect attempt: the line level, how many bytes echoed
back, and the raw reply.

## Verify before trusting it

Ported against Betaflight master `src/main/io/serial_4way.c`,
`serial_4way_avrootloader.c` and `msp/msp.c`. Diff against those before
writing flash — reading is safe to experiment with, writing is not.

Test order:

1. Read-only first. Connect with ESC Configurator and confirm all four
   ESCs enumerate and report firmware versions. This exercises
   `MSP_SET_PASSTHROUGH`, `cmd_DeviceInitFlash` on each index, and
   `cmd_DeviceRead` — no writes.
2. Scope one signal line during a connect attempt. You should see the
   21-byte boot init at 19200 8N1, then the ESC's `471x` reply.
3. Only then try a settings write, on one ESC, with a known-good
   firmware image on hand.

A failed flash write leaves an ESC needing recovery over its SWD/C2 pads.
