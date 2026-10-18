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
#include "product.h"
#include "sle_common.h"
#include "sle_device_discovery.h"
#include "sle_errcode.h"
#include "osal_debug.h"
#include "string.h"
#include "mac_addr.h"
#include "tcxo.h"

#include "beacon_adv.h"

#define BEACON_LOG "[beacon]"
#define log_print(fmt, args...) osal_printk(fmt, ##args)

#define BEACON_ADV_HANDLE 1
#define BEACON_ADV_DATA_LEN_MAX 251
#define BEACON_NAME_MAX_LEN 16

/* Common TLV shape used inside the advertising data field: length, type, value.
 * The length byte counts the type and value octets only. */
typedef struct {
    uint8_t length;
    uint8_t type;
    uint8_t value;
} beacon_adv_tlv_t;

/* TLV type codes used here. Values come from the SLE advertising spec. */
#define BEACON_ADV_TYPE_DISCOVERY_LEVEL 0x01
#define BEACON_ADV_TYPE_TX_POWER_LEVEL 0x0C
#define BEACON_ADV_TYPE_LOCAL_NAME 0x0B
#define BEACON_ADV_TYPE_MANUFACTURER 0xFF

static uint8_t beacon_local_name[BEACON_NAME_MAX_LEN] = "sle_beacon";

/* Payload currently on air. beacon_send() diffs candidates against this copy
 * so identical inputs cost zero stack calls. */
static uint8_t g_beacon_on_air[BEACON_PAYLOAD_LEN] = {0};

/* Payload that failed the throttle gate. beacon_flush() retries it. */
static uint8_t g_beacon_pending[BEACON_PAYLOAD_LEN];
static uint8_t g_beacon_pending_valid = 0;

/* Timestamp of the last real stack operation, in milliseconds. */
static uint64_t g_beacon_last_op_ms = 0;

/* Local address cache. The seek callback needs it to discard self-originated
 * frames, but that context forbids stack calls, so the value is snapshotted
 * here while announce parameters are being set. */
static uint8_t g_beacon_own_addr[6] = {0};
static uint8_t g_beacon_own_addr_valid = 0;

void beacon_cache_own_addr(const uint8_t addr[6])
{
    if (memcpy_s(g_beacon_own_addr, sizeof(g_beacon_own_addr), addr, 6) == EOK) {
        g_beacon_own_addr_valid = 1;
    }
}

const uint8_t *beacon_get_own_addr(void)
{
    return g_beacon_own_addr_valid ? g_beacon_own_addr : NULL;
}

static uint16_t beacon_put_name(uint8_t *buf, uint16_t max_len)
{
    uint8_t name_len = sizeof(beacon_local_name) - 1;
    if ((uint16_t)(name_len + 2) > max_len) {
        return 0;
    }
    buf[0] = name_len + 1;
    buf[1] = BEACON_ADV_TYPE_LOCAL_NAME;
    if (memcpy_s(&buf[2], max_len - 2, beacon_local_name, name_len) != EOK) {
        return 0;
    }
    return name_len + 2;
}

/* Assemble the announce PDU: discovery-level TLV followed by the vendor TLV
 * that carries the application payload. Discovery metadata stays in the
 * announce PDU; TX power and the device name go into the seek response so the
 * hot path stays at one TLV plus payload. */
static uint16_t beacon_build_adv(uint8_t *buf)
{
    uint16_t idx = 0;
    beacon_adv_tlv_t lvl = {
        .length = sizeof(beacon_adv_tlv_t) - 1,
        .type = BEACON_ADV_TYPE_DISCOVERY_LEVEL,
        .value = 0x01, /* normal */
    };

    if (memcpy_s(&buf[idx], BEACON_ADV_DATA_LEN_MAX - idx, &lvl, sizeof(lvl)) != EOK) {
        return 0;
    }
    idx += sizeof(lvl);

    /* Vendor TLV: length field = payload length + 1 for the type octet. */
    buf[idx++] = BEACON_PAYLOAD_LEN + 1;
    buf[idx++] = BEACON_TLV_TYPE;
    if (memcpy_s(&buf[idx], BEACON_ADV_DATA_LEN_MAX - idx,
                 g_beacon_on_air, BEACON_PAYLOAD_LEN) != EOK) {
        return 0;
    }
    idx += BEACON_PAYLOAD_LEN;
    return idx;
}

static uint16_t beacon_build_scan_rsp(uint8_t *buf)
{
    uint16_t idx = 0;
    beacon_adv_tlv_t txp = {
        .length = sizeof(beacon_adv_tlv_t) - 1,
        .type = BEACON_ADV_TYPE_TX_POWER_LEVEL,
        .value = BEACON_TX_POWER,
    };

    if (memcpy_s(buf, BEACON_ADV_DATA_LEN_MAX, &txp, sizeof(txp)) != EOK) {
        return 0;
    }
    idx += sizeof(txp);
    idx += beacon_put_name(&buf[idx], BEACON_ADV_DATA_LEN_MAX - idx);
    return idx;
}

/* Derive a unique local address.
 *
 * The vendor sample hardcodes:
 *     unsigned char local_addr[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
 * Two boards flashed with the same image therefore share one address. The
 * stack filters inbound announcements against the local address, so each board
 * discards the other's traffic. The symptom is "transmits fine, receives
 * nothing", with no error anywhere in the logs.
 *
 * sle_get_local_addr() is not a substitute. After a soft reset it returns
 * leftover RAM (observed consistently as 08 00 45 00 08 00 here), and seeking
 * with that address leaves the radio deaf until the next power cycle.
 *
 * get_dev_addr(IFTYPE_SLE) rebuilds the address on every boot from, in order,
 * the stack, the NV factory MAC, EFUSE, then a random fallback. */
static void beacon_pick_own_addr(sle_addr_t *out)
{
    uint8_t mac[6] = {0};

    out->type = 0;
    if (get_dev_addr(mac, 6, IFTYPE_SLE) == ERRCODE_SUCC) {
        if (memcpy_s(out->addr, 6, mac, 6) == EOK) {
            /* Push it into the stack as well. Without this the stack keeps its
             * own idea of the local address, and self-filtering stops working
             * because inbound frames never compare equal to us. */
            if (sle_set_local_addr(out) != ERRCODE_SLE_SUCCESS) {
                log_print("%s sle_set_local_addr fail\r\n", BEACON_LOG);
            }
            return;
        }
    }

    log_print("%s get_dev_addr fail, fallback to fixed addr\r\n", BEACON_LOG);
    const uint8_t fallback[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    (void)memcpy_s(out->addr, 6, fallback, 6);
}

static int beacon_set_param(void)
{
    sle_announce_param_t param = {0};
    uint8_t i;

    /* Non-connectable, scannable. All five announce modes ship in
     * sle_device_discovery.h; this is the one that turns the radio into a
     * beacon instead of a discovery endpoint for a connection. */
    param.announce_mode = SLE_ANNOUNCE_MODE_NONCONN_SCANABLE;
    param.announce_handle = BEACON_ADV_HANDLE;
    param.announce_gt_role = SLE_ANNOUNCE_ROLE_T_CAN_NEGO;
    param.announce_level = SLE_ANNOUNCE_LEVEL_NORMAL;
    param.announce_channel_map = SLE_ADV_CHANNEL_MAP_DEFAULT;
    param.announce_interval_min = BEACON_INTERVAL;
    param.announce_interval_max = BEACON_INTERVAL;
    param.conn_interval_min = 0x64;
    param.conn_interval_max = 0x64;
    param.conn_max_latency = 0x1F3;
    param.conn_supervision_timeout = 0x1F4;
    param.announce_tx_power = BEACON_TX_POWER;

    beacon_pick_own_addr(&param.own_addr);
    beacon_cache_own_addr(param.own_addr.addr);

    log_print("%s own addr: ", BEACON_LOG);
    for (i = 0; i < 6; i++) {
        log_print("%02x ", param.own_addr.addr[i]);
    }
    log_print("\r\n");

    return sle_set_announce_param(BEACON_ADV_HANDLE, &param);
}

static int beacon_set_data(void)
{
    sle_announce_data_t data = {0};
    uint8_t adv_buf[BEACON_ADV_DATA_LEN_MAX] = {0};
    uint8_t rsp_buf[BEACON_ADV_DATA_LEN_MAX] = {0};
    errcode_t ret;

    data.announce_data_len = beacon_build_adv(adv_buf);
    data.announce_data = adv_buf;
    data.seek_rsp_data_len = beacon_build_scan_rsp(rsp_buf);
    data.seek_rsp_data = rsp_buf;

    if (data.announce_data_len == 0 || data.seek_rsp_data_len == 0) {
        log_print("%s build adv data fail\r\n", BEACON_LOG);
        return -1;
    }

    ret = sle_set_announce_data(BEACON_ADV_HANDLE, &data);
    if (ret != ERRCODE_SLE_SUCCESS) {
        /* Log the code. A silently failed announce update looks exactly like a
         * healthy one from the caller's side. */
        log_print("%s set announce data fail: 0x%x\r\n", BEACON_LOG, ret);
    }
    return ret;
}

static void beacon_announce_enable_cbk(uint32_t id, errcode_t status)
{
    log_print("%s announce enable id:%02x status:%x\r\n", BEACON_LOG, id, status);
}

static void beacon_announce_disable_cbk(uint32_t id, errcode_t status)
{
    log_print("%s announce disable id:%02x status:%x\r\n", BEACON_LOG, id, status);
}

static void beacon_announce_terminal_cbk(uint32_t id)
{
    log_print("%s announce terminal id:%02x\r\n", BEACON_LOG, id);
}

/* Implemented in beacon_rx.c */
void beacon_seek_result_cbk(sle_seek_result_info_t *result);
void beacon_seek_enable_cbk(errcode_t status);
void beacon_seek_disable_cbk(errcode_t status);

errcode_t beacon_register_cbks(void)
{
    sle_announce_seek_callbacks_t cbks = {0};

    /* Announce and seek callbacks must go in as one table. Calling
     * sle_announce_seek_register_callbacks() twice replaces the previous table
     * wholesale, which yields "transmits fine, receives nothing" with a clean
     * log. */
    cbks.announce_enable_cb = beacon_announce_enable_cbk;
    cbks.announce_disable_cb = beacon_announce_disable_cbk;
    cbks.announce_terminal_cb = beacon_announce_terminal_cbk;
    cbks.seek_result_cb = beacon_seek_result_cbk;
    cbks.seek_enable_cb = beacon_seek_enable_cbk;
    cbks.seek_disable_cb = beacon_seek_disable_cbk;

    return sle_announce_seek_register_callbacks(&cbks);
}

errcode_t beacon_flush(void)
{
    errcode_t ret;
    uint64_t now;

    if (!g_beacon_pending_valid) {
        return ERRCODE_SLE_SUCCESS;
    }

    now = uapi_tcxo_get_ms();
    if ((now - g_beacon_last_op_ms) < BEACON_REFRESH_MIN_MS) {
        /* Too soon. Keep the pending copy; the caller's loop will come back.
         * This single check is what keeps the stack alive under load. */
        return ERRCODE_SLE_SUCCESS;
    }
    /* Stamp before touching the stack, so a failure below still counts against
     * the budget and cannot be retried in a tight loop. */
    g_beacon_last_op_ms = now;

    (void)memcpy_s(g_beacon_on_air, BEACON_PAYLOAD_LEN, g_beacon_pending, BEACON_PAYLOAD_LEN);

    /* stop -> set -> start is mandatory.
     * Calling sle_set_announce_data() while announcing returns success but the
     * air interface keeps the old bytes. Measured: 943 frames captured by the
     * peer over 12 s, every one of them the initial payload. */
    ret = sle_stop_announce(BEACON_ADV_HANDLE);
    if (ret != ERRCODE_SLE_SUCCESS) {
        log_print("%s stop announce fail: 0x%x\r\n", BEACON_LOG, ret);
        return ret;
    }
    ret = beacon_set_data();
    if (ret != ERRCODE_SLE_SUCCESS) {
        return ret;
    }
    ret = sle_start_announce(BEACON_ADV_HANDLE);
    if (ret != ERRCODE_SLE_SUCCESS) {
        log_print("%s start announce fail: 0x%x\r\n", BEACON_LOG, ret);
        return ret;
    }

    g_beacon_pending_valid = 0;
    return ERRCODE_SLE_SUCCESS;
}

errcode_t beacon_send(const uint8_t *payload, uint16_t len)
{
    if (payload == NULL || len != BEACON_PAYLOAD_LEN) {
        return ERRCODE_SLE_PARAM_ERR;
    }

    /* Identical bytes cost nothing.
     * This is not an optimisation. A host pushing status frames at 9 Hz sent
     * 484 frames of which 482 were byte-identical; without the diff that is
     * nine stop/set/start cycles per second, which the stack cannot survive. */
    if (memcmp(g_beacon_on_air, payload, BEACON_PAYLOAD_LEN) == 0) {
        return ERRCODE_SLE_SUCCESS;
    }

    (void)memcpy_s(g_beacon_pending, BEACON_PAYLOAD_LEN, payload, BEACON_PAYLOAD_LEN);
    g_beacon_pending_valid = 1;

    return beacon_flush();
}

errcode_t beacon_init(void)
{
    errcode_t ret;

    (void)beacon_set_param();
    (void)beacon_set_data();

    ret = sle_start_announce(BEACON_ADV_HANDLE);
    if (ret != ERRCODE_SLE_SUCCESS) {
        log_print("%s start announce fail: 0x%x\r\n", BEACON_LOG, ret);
        return ret;
    }
    return ERRCODE_SLE_SUCCESS;
}
