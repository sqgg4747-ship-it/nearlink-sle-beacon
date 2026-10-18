# nearlink-sle-beacon

**English** | [简体中文](README.zh-CN.md)

Connectionless state broadcast for NearLink SLE, on the WS63.

A node writes a 16-byte record into its announce PDU, and every node in range reads it back
out of the seek callback. No pairing, no connection, no coordinator, and nothing that has
to stay in sync between them — which is what makes it work when the peers have never met.

The library does not interpret the record. Bytes 2..15 are yours.

WS63 / BearPi-Pico H3863, WS63 LiteOS SDK. MIT, see [LICENSE](LICENSE).

## Why not the sample

The vendor's `sle_uart` is a serial bridge, and its UART handler is explicit about the
requirement:

```c
if (sle_uart_client_is_connected()) {
    sle_uart_server_send_report_by_handle(buffer, length);
} else {
    osal_printk("%s sle client is not connected! \r\n", ...);
}
```

No connection, no data. That is the right model for a bridge between two known endpoints,
and the wrong one when the endpoints have no prior relationship and no chance to negotiate
before the first useful byte has to go out — which is the case this library exists for. So
it drops the connection requirement and keeps everything on the announce path.

## The three files

```
beacon_adv.c    announce side: parameters, payload swap, throttle
beacon_rx.c     receive side: find the record, filter self, hand it up
beacon_uart.c   UART framing, message queue, task loop
```

One task, one interrupt handler, no heap.

The split is forced by the stack. `sle_stop_announce()` is asynchronous, so a second
context issuing `set` / `start` races the pending stop and leaves the stack's internal list
inconsistent. Every radio call happens on the one task.

The UART is the default input because most hosts already have one. `beacon_send()` takes a
pointer and a length, so another transport can feed it and `beacon_uart.c` can go — keep
the flush loop though, since `beacon_flush()` is what eventually puts a throttled payload
on air.

Six calls make up the public surface; `beacon_uart_start()` runs them all for the UART
path.

## Build

Drop the three sources into the SDK sample tree as `sle_uart_server_adv.c`,
`sle_uart_server.c` and `sle_uart.c`, then call `beacon_register_cbks()` followed by
`beacon_uart_start()` from an `app_run()`:

```bash
python build.py ws63-liteos-app -j8
```

Builds clean on a 2025-01 SDK snapshot, 765 targets. One thing that stops a fresh tree:
`SLE_ADV_CHANNEL_MAP_DEFAULT` is not exported by the SDK headers. The vendor sample
defines it locally and the copy is in `beacon_adv.h`.

## Wire format

```
02 01 01            discovery level TLV
11 FF <16 bytes>    vendor TLV carrying the record
```

The record starts with `AA 55` so a receiver can find it without walking the TLV chain.
Those two bytes and the length are the only agreements this library needs; the rest is in
`docs/protocol.md`.

The interval is `0x64` in 125 us units, so 12.5 ms, but content can only change once per
200 ms; between changes the stack keeps repeating the last payload on its own.

## Notes

Three things the sample does not tell you, in the order they cost me time:

- Logging every received frame on 115200 baud does not work. At 80 frames a second and
  about 120 characters a line that is 9600 a second against a ceiling of 11520, and the
  task hosting the callback stalls. Print on change, sample the repeats.
- Devices flashed with the same image claim the same local address. The stack filters
  inbound frames against it, so they never hear each other, and no log line says why.
  Derive from the chip MAC instead.
- Updating the payload while announcing returns success and changes nothing on air. Stop,
  set, start. Three calls, that order.

The 200 ms floor is not tuning. It falls out of the second and third items.

## Limits

A transport, not a security layer: records go out in the clear and nothing marks who sent
one. No acknowledgement either, so a lost PDU is lost. One hop. The radio runs
continuously, which a coin cell will not appreciate.

Verified senders and guaranteed delivery belong above this layer, not in it.

## Files

```
src/               announce, receive, UART
docs/protocol.md   wire format, acceptance rules
docs/internals.md  contexts, data flow, lifecycle
CHANGELOG.md       releases
CONTRIBUTING.md    what gets merged
```

## Credits

Derived from the HiSilicon WS63 SDK sample `sle_uart`, Apache-2.0. Original copyright is
kept in every source header and in [NOTICE](NOTICE). No SDK code is redistributed here.

The announce modes are part of the SLE spec — all five ship in `sle_device_discovery.h`.
What is not in the spec is carrying application state inside an announce PDU and doing the
whole receive path in the seek callback. That part, and the constraints it drags in, is
what this repository is about.
