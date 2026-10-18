// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Contributors to the nearlink-sle-beacon project
//
// Derived from the HiSilicon WS63 SDK sample "sle_uart":
//   Original: Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023.
//             Licensed under the Apache License, Version 2.0 (see NOTICE).
//   Modifications: Copyright (c) 2026 Contributors to the nearlink-sle-beacon project.
//
#include "securec.h"
#include "errcode.h"
#include "osal_addr.h"
#include "sle_common.h"
#include "sle_device_discovery.h"
#include "sle_errcode.h"
#include "osal_debug.h"
#include "string.h"

#include "beacon_adv.h"
#include "beacon_rx.h"

#define BEACON_RX_LOG "[beacon rx]"
#define log_print(fmt, args...) osal_printk(fmt, ##args)

/* Frame header. Any two bytes work as long as both ends agree. */
#define BEACON_FRAME_HEAD0 0xAA
#define BEACON_FRAME_HEAD1 0x55

/* Print sampling. Logging every frame saturates the UART and stalls the task
 * that owns this callback. See docs/failure-modes.md, item 7. */
#define BEACON_PRINT_EVERY 40
#define BEACON_STAT_EVERY 400
#define BEACON_RAW_DROP_SAMPLE 100

static beacon_rx_handler_t g_beacon_rx_handler = NULL;

static uint8_t g_last_payload[BEACON_PAYLOAD_LEN];
static uint8_t g_last_src[6];
static uint8_t g_have_last = 0;
static uint32_t g_rx_ok = 0;
static uint32_t g_rx_drop = 0;

void beacon_rx_set_handler(beacon_rx_handler_t handler)
{
    g_beacon_rx_handler = handler;
}

/* Reject our own announcements. Without this, a single-board test looks like
 * it is receiving a peer when it is only hearing itself. */
static uint8_t is_from_self(const uint8_t *src)
{
    const uint8_t *own = beacon_get_own_addr();
    if (own == NULL || src == NULL) {
        return 0;
    }
    return (memcmp(own, src, 6) == 0) ? 1 : 0;
}

void beacon_seek_result_cbk(sle_seek_result_info_t *result)
{
    uint8_t *data;
    uint16_t len;
    uint16_t i;

    if (result == NULL || result->data == NULL) {
        return;
    }
    data = result->data;
    len = result->data_length;
    if (len < BEACON_PAYLOAD_LEN) {
        return;
    }

    /* Scan for the frame header directly; do not walk the TLV chain.
     * The stack hands over data shaped like 01 02 01 | 11 FF | AA 55 ...,
     * where the first segment declares a length of 1 but occupies 3 bytes.
     * Stepping by the declared length lands one byte off every time, and the
     * walk runs off the end. See docs/failure-modes.md, item 6. */
    for (i = 0; (uint16_t)(i + BEACON_PAYLOAD_LEN) <= len; i++) {
        if (data[i] != BEACON_FRAME_HEAD0 || data[i + 1] != BEACON_FRAME_HEAD1) {
            continue;
        }

        const uint8_t *payload = &data[i];
        uint8_t self = is_from_self(result->addr.addr);
        uint8_t changed = 1;
        uint8_t k;

        g_rx_ok++;

        if (g_have_last) {
            changed = 0;
            for (k = 0; k < BEACON_PAYLOAD_LEN; k++) {
                if (g_last_payload[k] != payload[k]) {
                    changed = 1;
                    break;
                }
            }
            /* A different sender is a new event even with identical bytes. */
            if (!changed) {
                for (k = 0; k < 6; k++) {
                    if (g_last_src[k] != result->addr.addr[k]) {
                        changed = 1;
                        break;
                    }
                }
            }
        }

        /* Changed state prints in full; repeats are sampled. One line is about
         * 120 characters. At 80 PDUs/s that is 9600 characters per second
         * against a 11520 characters per second ceiling on a 115200 baud port,
         * so per-frame logging eventually blocks the task and trips the
         * watchdog. */
        if (changed || (g_rx_ok % BEACON_PRINT_EVERY) == 1) {
            log_print("\r\n>>> [RX] ");
            for (k = 0; k < BEACON_PAYLOAD_LEN; k++) {
                log_print("%02X ", payload[k]);
            }
            log_print("<<< src:%02x:%02x:%02x:%02x:%02x:%02x rssi:%d %s\r\n",
                      result->addr.addr[0], result->addr.addr[1], result->addr.addr[2],
                      result->addr.addr[3], result->addr.addr[4], result->addr.addr[5],
                      (int)(int8_t)result->rssi,
                      self ? "SELF" : "PEER");

            (void)memcpy_s(g_last_payload, BEACON_PAYLOAD_LEN, payload, BEACON_PAYLOAD_LEN);
            (void)memcpy_s(g_last_src, 6, result->addr.addr, 6);
            g_have_last = 1;
        }

        if ((g_rx_ok % BEACON_STAT_EVERY) == 0) {
            log_print("\r\n[RX STAT] total=%d\r\n", (int)g_rx_ok);
        }

        /* Hand it up. Self-originated frames are forwarded too, tagged, so the
         * application decides what to do with them. */
        if (g_beacon_rx_handler != NULL) {
            g_beacon_rx_handler(payload, result->addr.addr, (int8_t)result->rssi, self);
        }
        return;
    }

    /* No header found. Most likely a seek response, which carries only TX power
     * and the device name. Sampled for introspection: it separates "nothing
     * arrived" from "something arrived in an unexpected shape". */
    g_rx_drop++;
    if ((g_rx_drop % BEACON_RAW_DROP_SAMPLE) == 1) {
        log_print("\r\n[RAW DROP] len=%d: ", len);
        for (i = 0; i < len && i < 24; i++) {
            log_print("%02X ", data[i]);
        }
        log_print("\r\n");
    }
}

void beacon_seek_enable_cbk(errcode_t status)
{
    log_print("%s seek enable status:%x\r\n", BEACON_RX_LOG, status);
}

void beacon_seek_disable_cbk(errcode_t status)
{
    log_print("%s seek disable status:%x\r\n", BEACON_RX_LOG, status);
}
