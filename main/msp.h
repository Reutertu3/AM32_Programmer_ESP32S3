/*
 * msp.h — the minimal MSP responder that convinces a configurator it is
 * talking to a flight controller.
 *
 * Without this the tool never issues MSP_SET_PASSTHROUGH and 4-way mode
 * is unreachable. Both MSP v1 ($M) and MSP v2 ($X) framing are accepted.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void msp_init(void);

/* Feed one byte from the host. Replies are emitted internally. */
void msp_process_byte(uint8_t b);

/* True once after the host has successfully requested 4-way passthrough
 * and the reply has been flushed. The caller should then run
 * esc4way_process(). */
bool msp_take_passthrough_request(void);
