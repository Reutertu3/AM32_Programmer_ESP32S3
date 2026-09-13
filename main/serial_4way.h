/*
 * serial_4way.h — BLHeli 4-way interface, host side.
 *
 * Port of Betaflight src/main/io/serial_4way.c. The wire format is
 * unchanged; only the byte transport (USB CDC instead of a UART
 * serialPort_t) and the ESC IO layer differ.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Interface modes */
#define imC2        0
#define imSIL_BLB   1
#define imATM_BLB   2
#define imSK        3
#define imARM_BLB   4

/* Bring up the ESC pins and report how many outputs exist. This is the
 * value returned to the configurator in the MSP_SET_PASSTHROUGH reply. */
uint8_t esc4way_init(void);

/* Run the 4-way command loop. Blocks until the host sends
 * cmd_InterfaceExit, then releases the pins and returns. */
void esc4way_process(void);
