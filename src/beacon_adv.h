// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Contributors to the nearlink-sle-beacon project
//
// Derived from the HiSilicon WS63 SDK sample "sle_uart":
//   Original: Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023.
//             Licensed under the Apache License, Version 2.0 (see NOTICE).
//   Modifications: Copyright (c) 2026 Contributors to the nearlink-sle-beacon project.
//
#ifndef BEACON_ADV_H
#define BEACON_ADV_H

#include <stdint.h>
#include "errcode.h"

/* SLE advertising channel map. The SDK headers do not export this enum; the
 * vendor sample defines it in its own translation unit. Values are kept
 * identical to that definition so the bit layout matches what the stack
 * expects. */
typedef enum {
    SLE_ADV_CHANNEL_MAP_77 = 0x01,
    SLE_ADV_CHANNEL_MAP_78 = 0x02,
    SLE_ADV_CHANNEL_MAP_79 = 0x04,
    SLE_ADV_CHANNEL_MAP_DEFAULT = 0x07
} sle_adv_channel_map_t;

/* ---- Tunables ---- */

/* Application payload size in bytes. The announce PDU limit is 251 bytes for
 * the whole advertising data field; 16 covers one compact status record plus
 * the vendor TLV wrapper. */
#define BEACON_PAYLOAD_LEN 16

/* TLV type that carries the payload. 0xFF is the vendor-specific range. */
#define BEACON_TLV_TYPE 0xFF

/* Announce interval, in units of 125 us. 0x64 = 100 * 125 us = 12.5 ms,
 * i.e. 80 PDUs per second per node. */
#define BEACON_INTERVAL 0x64

/* Minimum spacing between two announce reconfigurations, in milliseconds.
 * Not a performance knob. See docs/failure-modes.md, item 1. */
#define BEACON_REFRESH_MIN_MS 200U

/* Announce TX power, dBm. */
#define BEACON_TX_POWER 18

/* ---- API ---- */

/* Configure announce parameters, load the initial payload, start announcing. */
errcode_t beacon_init(void);

/* Submit a new payload. Returns immediately without touching the stack if the
 * bytes match what is already on air. */
errcode_t beacon_send(const uint8_t *payload, uint16_t len);

/* Drain the pending slot. Call periodically from the task loop; this is what
 * eventually puts a throttled payload on air. */
errcode_t beacon_flush(void);

/* Register the announce and seek callbacks. Must be called exactly once,
 * before beacon_init(). Splitting this into two calls silently wipes the
 * first callback table. */
errcode_t beacon_register_cbks(void);

/* Local address cache. The seek callback needs it for self-filtering, but that
 * callback runs in a context where the stack API is off limits, so the address
 * is captured here during announce setup. */
void beacon_cache_own_addr(const uint8_t addr[6]);
const uint8_t *beacon_get_own_addr(void);

#endif /* BEACON_ADV_H */
