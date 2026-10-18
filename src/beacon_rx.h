// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Contributors to the nearlink-sle-beacon project
//
// Derived from the HiSilicon WS63 SDK sample "sle_uart":
//   Original: Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023.
//             Licensed under the Apache License, Version 2.0 (see NOTICE).
//   Modifications: Copyright (c) 2026 Contributors to the nearlink-sle-beacon project.
//
#ifndef BEACON_RX_H
#define BEACON_RX_H

#include <stdint.h>

/* Invoked once per accepted payload.
 *
 * payload : BEACON_PAYLOAD_LEN bytes, frame header already validated
 * src     : sender SLE address
 * rssi    : dBm, as reported by the stack
 * is_self : 1 when this is our own announcement looping back
 *
 * Runs in the stack's task context. Keep it short and do not call blocking
 * stack APIs from here. */
typedef void (*beacon_rx_handler_t)(const uint8_t *payload,
                                    const uint8_t src[6],
                                    int8_t rssi,
                                    uint8_t is_self);

void beacon_rx_set_handler(beacon_rx_handler_t handler);

#endif /* BEACON_RX_H */
