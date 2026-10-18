// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Contributors to the nearlink-sle-beacon project
//
// Derived from the HiSilicon WS63 SDK sample "sle_uart":
//   Original: Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023.
//             Licensed under the Apache License, Version 2.0 (see NOTICE).
//   Modifications: Copyright (c) 2026 Contributors to the nearlink-sle-beacon project.
//
#ifndef BEACON_UART_H
#define BEACON_UART_H

#include <stdint.h>

/* Spawn the injection task. It initialises the announce side, then loops over
 * inbound UART frames and pending-payload flushes. */
void beacon_uart_start(void);

/* Diagnostic counters. A non-zero resync means bytes were lost on the wire and
 * the framer had to re-align; it recovers on its own. */
uint32_t beacon_uart_stats(uint32_t *bytes, uint32_t *frames, uint32_t *resync);

#endif /* BEACON_UART_H */
