// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Contributors to the nearlink-sle-beacon project
//
// Derived from the HiSilicon WS63 SDK sample "sle_uart":
//   Original: Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023.
//             Licensed under the Apache License, Version 2.0 (see NOTICE).
//   Modifications: Copyright (c) 2026 Contributors to the nearlink-sle-beacon project.
//
#include "common_def.h"
#include "soc_osal.h"
#include "app_init.h"
#include "pinctrl.h"
#include "uart.h"
#include "securec.h"
#include "tcxo.h"

#include "beacon_adv.h"
#include "beacon_uart.h"

#define BEACON_UART_LOG "[beacon uart]"
#define log_print(fmt, args...) osal_printk(fmt, ##args)

#define BEACON_UART_BAUDRATE 115200
#define BEACON_UART_RX_BUF 512
#define BEACON_TASK_STACK 0x1200
#define BEACON_TASK_PRIO 28

/* Queue read timeout, in ticks. A blocking read would park the task whenever
 * the host is quiet, and the pending-payload flush would never run. 50 ticks is
 * 500 ms on the default tick. */
#define BEACON_QUEUE_TIMEOUT 50

/* Loop cadence. One flush attempt every 50 ms leaves the 200 ms throttle
 * budget with enough margin to land within one announce interval of the
 * deadline. */
#define BEACON_TASK_TICK_MS 50

#ifndef CONFIG_BEACON_UART_BUS
#define CONFIG_BEACON_UART_BUS 0
#endif
#ifndef CONFIG_BEACON_UART_TXD
#define CONFIG_BEACON_UART_TXD 0
#endif
#ifndef CONFIG_BEACON_UART_RXD
#define CONFIG_BEACON_UART_RXD 1
#endif

#define FRAME_LEN BEACON_PAYLOAD_LEN
#define HEAD0 0xAA
#define HEAD1 0x55

static uint8_t g_uart_rx_buf[BEACON_UART_RX_BUF] = {0};
static uart_buffer_config_t g_uart_buf_cfg = {
    .rx_buffer = g_uart_rx_buf,
    .rx_buffer_size = BEACON_UART_RX_BUF
};

/* Accumulator holding the frame currently being aligned. */
static uint8_t g_acc[FRAME_LEN];
static uint8_t g_acc_len = 0;

/* Diagnostic counters. They separate "no bytes arrived" from "bytes arrived
 * but never framed" without attaching a logic analyser. */
static volatile uint32_t g_uart_bytes = 0;
static volatile uint32_t g_uart_frames = 0;
static volatile uint32_t g_uart_resync = 0;

static unsigned long g_msgq_id;

uint32_t beacon_uart_stats(uint32_t *bytes, uint32_t *frames, uint32_t *resync)
{
    if (bytes) { *bytes = g_uart_bytes; }
    if (frames) { *frames = g_uart_frames; }
    if (resync) { *resync = g_uart_resync; }
    return g_acc_len;
}

static void uart_pin_init(void)
{
    uapi_pin_set_mode(CONFIG_BEACON_UART_TXD, PIN_MODE_1);
    uapi_pin_set_mode(CONFIG_BEACON_UART_RXD, PIN_MODE_1);
}

static void uart_cfg_init(void)
{
    uart_attr_t attr = {
        .baud_rate = BEACON_UART_BAUDRATE,
        .data_bits = UART_DATA_BIT_8,
        .stop_bits = UART_STOP_BIT_1,
        .parity = UART_PARITY_NONE
    };
    uart_pin_config_t pin_cfg = {
        .tx_pin = CONFIG_BEACON_UART_TXD,
        .rx_pin = CONFIG_BEACON_UART_RXD,
        .cts_pin = PIN_NONE,
        .rts_pin = PIN_NONE
    };

    uapi_uart_deinit(CONFIG_BEACON_UART_BUS);
    uapi_uart_init(CONFIG_BEACON_UART_BUS, &pin_cfg, &attr, NULL, &g_uart_buf_cfg);
}

static void msgq_put(uint8_t *buf, uint16_t len)
{
    osal_msg_queue_write_copy(g_msgq_id, (void *)buf, (uint32_t)len, 0);
}

/* UART receive callback.
 *
 * The naive version counts to FRAME_LEN and posts. One lost byte shifts the
 * accumulator permanently: every subsequent block is cut from the middle of a
 * frame, the application's header check never passes, and nothing recovers
 * short of a reset. Measured: 455 injected frames across five host rates, zero
 * delivered.
 *
 * This version treats the port as an unframed byte stream. It anchors on
 * 0xAA 0x55 and slides one byte on a mismatch, so any dropped byte and any
 * chunk boundary resynchronises on its own. */
static void uart_rx_isr(const void *buffer, uint16_t length, bool error)
{
    const uint8_t *p = (const uint8_t *)buffer;
    uint16_t i;
    unused(error);

    if (buffer == NULL || length == 0) {
        return;
    }
    /* Interrupt context: align and post, nothing else. The stack is only
     * touched from the task. */
    g_uart_bytes += length;

    for (i = 0; i < length; i++) {
        uint8_t b = p[i];

        if (g_acc_len == 0) {
            if (b != HEAD0) {
                continue;
            }
        } else if (g_acc_len == 1) {
            if (b != HEAD1) {
                g_uart_resync++;
                if (b == HEAD0) {
                    g_acc[0] = HEAD0;
                    g_acc_len = 1;
                } else {
                    g_acc_len = 0;
                }
                continue;
            }
        }

        g_acc[g_acc_len++] = b;

        if (g_acc_len == FRAME_LEN) {
            g_uart_frames++;
            msgq_put(g_acc, FRAME_LEN);
            g_acc_len = 0;
        }
    }
}

static void *beacon_task(const char *arg)
{
    uint8_t rx[FRAME_LEN];
    uint32_t rx_len = FRAME_LEN;
    uint64_t t_diag = 0;
    unused(arg);

    if (osal_msg_queue_create("beacon_msgq", 5, (unsigned long *)&g_msgq_id,
                              0, FRAME_LEN) != OSAL_SUCCESS) {
        log_print("^%s msg queue create fail\r\n", BEACON_UART_LOG);
        return NULL;
    }

    uart_pin_init();
    uart_cfg_init();
    (void)uapi_uart_register_rx_callback(CONFIG_BEACON_UART_BUS,
                                         UART_RX_CONDITION_FULL_OR_SUFFICIENT_DATA,
                                         1, uart_rx_isr);

    beacon_init();

    while (1) {
        (void)memset_s(rx, sizeof(rx), 0, sizeof(rx));
        rx_len = FRAME_LEN;

        if (osal_msg_queue_read_copy(g_msgq_id, (void *)rx, &rx_len,
                                     BEACON_QUEUE_TIMEOUT) == OSAL_SUCCESS &&
            rx_len == FRAME_LEN) {
            (void)beacon_send(rx, FRAME_LEN);
        }

        /* Attempt a flush every pass. This is where a throttled payload
         * eventually reaches the air. */
        (void)beacon_flush();

        /* UART-side counters, once per second. */
        uint64_t now = uapi_tcxo_get_ms();
        if ((now - t_diag) >= 1000U) {
            t_diag = now;
            log_print("%s bytes=%d frames=%d resync=%d\r\n", BEACON_UART_LOG,
                      (int)g_uart_bytes, (int)g_uart_frames, (int)g_uart_resync);
        }

        osal_msleep(BEACON_TASK_TICK_MS);
    }
    return NULL;
}

void beacon_uart_start(void)
{
    osal_task *task = NULL;
    osal_kthread_lock();
    task = osal_kthread_create((osal_kthread_handler)beacon_task, 0, "BeaconTask", BEACON_TASK_STACK);
    if (task != NULL) {
        osal_kthread_set_priority(task, BEACON_TASK_PRIO);
    }
    osal_kthread_unlock();
}
