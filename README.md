# nearlink-sle-beacon

Connectionless state broadcast for NearLink SLE on the WS63 SoC.

A beacon node puts a 16-byte application record into its announce PDU. Any node in
range reads it out of the seek callback. No pairing, no connection, no handshake, no
central node to fail. Twenty devices powered on in the same room see each other within
one announce interval.

Target: WS63 / BearPi-Pico H3863, WS63 LiteOS SDK.

---

## The premise

The vendor sample `sle_uart` is a transparent serial bridge, and its UART handler is
unambiguous about the contract:

```c
static void sle_uart_server_read_int_handler(const void *buffer, uint16_t length, bool error)
{
    unused(error);
    if (sle_uart_client_is_connected()) {
        sle_uart_server_send_report_by_handle(buffer, length);
    } else {
        osal_printk("%s sle client is not connected! \r\n", ...);
    }
}
```

Bytes are forwarded only while a connection exists; otherwise they are dropped with a
log line. That is correct for a bridge. It is also the exact assumption this repository
removes.

The scenarios that need the opposite contract share a shape: many nodes, none of them
administratively related, arriving and leaving without notice, all of them needing to
know who is nearby and how close. A connection-oriented bridge cannot express that. A
broadcast can.

Removing the assumption takes a few lines. Living with the consequences took the
remaining 700.

---

## Architecture

Three translation units, one task, one interrupt handler. No dynamic allocation.

```
beacon_uart.c    IRQ: byte alignment -> message queue
                 Task: dequeue -> beacon_send -> beacon_flush
beacon_adv.c     Announce lifecycle: parameter set, payload swap, throttle gate
beacon_rx.c      Seek callback: header scan, self filter, change detection, dispatch
```

The split is not cosmetic. `sle_stop_announce()` is asynchronous; the actual stop is
queued inside `bt_service`. Interleaving a `set`/`start` with that pending stop
corrupts internal list state, and the next preemption faults. Every stack call in this
repository therefore happens on one task, at one cadence, behind one time gate.

See `docs/architecture.md` for the full sequence and `docs/protocol.md` for the wire
format.

---

## Wire format

Announce PDU, as it appears on air:

```
02 01 01                discovery level TLV: length 2, type 0x01, value 0x01
11 FF <16 bytes>        vendor TLV: length 17, type 0xFF, then the record
```

21 bytes total. The stack may prepend segments of its own before this buffer, so the
record has no fixed on-air offset — which is why the receiver scans rather than walks.

Seek response PDU, used only to expose transmit power and the device name:

```
02 0C 12                TX power TLV, 18 dBm
<name TLV>              local name, default "sle_beacon"
```

The application record itself is opaque to this library. It is `BEACON_PAYLOAD_LEN`
bytes, and the first two are the frame header:

```
offset 0   0xAA
offset 1   0x55
offset 2..  application-defined
```

Convention, not requirement. Both ends must agree; the library never interprets
offsets 2 and up. The header exists so the receiver can find the record without
parsing the TLV chain, which on this stack is not length-consistent (see
`docs/failure-modes.md`, item 6).

---

## Timing model

| Parameter | Value | Consequence |
|---|---|---|
| `BEACON_INTERVAL` | `0x64` = 12.5 ms | 80 PDUs/s per node |
| `BEACON_REFRESH_MIN_MS` | 200 ms | at most 5 reconfigurations/s per node |
| Task tick | 50 ms | throttle deadline lands within one interval |
| Queue timeout | 500 ms | flush keeps running while the host is silent |

The interval and the reconfiguration rate are independent budgets. A node can emit 80
PDUs per second while changing content at most five times per second. Between
reconfigurations the stack retransmits the last payload on its own.

This separation is what makes the design survive a chatty host. A host that pushes at
9 Hz produces 9 reconfiguration requests per second against a budget of 5; the excess
is queued in a single pending slot and coalesced, never dropped and never retried in a
loop.

---

## Address derivation

`beacon_pick_own_addr()` does not use the value the vendor sample hardcodes, and does
not use `sle_get_local_addr()`.

```c
get_dev_addr(mac, 6, IFTYPE_SLE)     // stack -> NV factory MAC -> EFUSE -> random
sle_set_local_addr(&addr)            // keep the stack's view in sync
```

Two reasons, both measured:

1. The hardcoded address is identical on every board. The stack filters inbound
   announcements against the local address, so two boards running the same image
   discard each other. The link looks like "transmits fine, receives nothing", with no
   error in either log.
2. After `hal_reboot_chip()`, `sle_get_local_addr()` returns leftover RAM — repeatably
   `08 00 45 00 08 00` on this hardware. Seeking with that address leaves the radio
   deaf across resets, including watchdog resets, because the value is rebuilt the same
   broken way every time.

The `sle_set_local_addr()` call is not optional. Skipping it leaves the stack's internal
address disagreeing with the announce parameter, and self-filtering silently stops
matching.

---

## Divergence from the vendor sample

Measured against `application/samples/products/sle_uart` as published on
`gitee.com/bearpi/bearpi-pico_h3863`, four files, 944 lines.

| Property | Vendor sample | This repository |
|---|---|---|
| Announce mode | `SLE_ANNOUNCE_MODE_CONNECTABLE_SCANABLE` | `SLE_ANNOUNCE_MODE_NONCONN_SCANABLE` |
| Precondition for transmitting | connection established | none |
| Announce PDU contents | discovery metadata | discovery metadata + 16-byte record |
| Inbound announce handling | absent (`seek_result` appears 0 times in the server TU) | full seek callback path |
| Payload refresh | set once at init | diff + throttle + stop/set/start |
| Local address | hardcoded `01:02:03:04:05:06` | derived from chip MAC |
| Announce interval | SDK default | 12.5 ms |
| Callback registration | announce only | announce + seek in one table |

Line count: 944 -> 1676 (+78%). The delta is not feature work. It is the set of
constraints that appear once broadcast replaces connection, documented one per entry in
`docs/failure-modes.md`.

---

## Build

Drop the sources into the SDK sample tree. The filenames differ from the ones the
build system expects, so map them explicitly:

| This repository | Installed as |
|---|---|
| `src/beacon_adv.c` | `sle_uart_server/sle_uart_server_adv.c` |
| `src/beacon_rx.c` | `sle_uart_server/sle_uart_server.c` |
| `src/beacon_uart.c` | `sle_uart.c` |
| `src/beacon_adv.h`, `beacon_rx.h`, `beacon_uart.h` | `sle_uart_server/` |

Then drive the entry point from the task file:

```c
#include "app_init.h"

static void beacon_entry(void)
{
    (void)beacon_register_cbks();
    beacon_uart_start();
}

app_run(beacon_entry);
```

`beacon_register_cbks()` runs before `beacon_uart_start()` because the announce and
seek callback tables are installed as one unit. Two separate registrations replace each
other, and the second one wins.

Build:

```bash
cd /path/to/bearpi-pico_h3863
python build.py ws63-liteos-app -j8
```

Output: `output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg`.

---

## Bring-up

Two boards. UART0 of each wired to a host:

```
host TX  ->  WS63 RX
host RX  <-  WS63 TX
GND      --- GND          // the one that gets forgotten
```

Inject a record:

```
AA 55 01 FF 11 22 33 44 55 66 77 88 99 00 EE FF
```

The peer should print, within one announce interval plus the seek window:

```
>>> [RX] AA 55 01 FF 11 22 33 44 55 66 77 88 99 00 EE FF <<< src:xx:xx:xx:xx:xx:xx rssi:-62 PEER
```

Change one byte and inject again; a second line appears immediately. Re-inject the same
bytes and nothing is printed, because the payload never left the diff gate — expected,
not a fault.

UART-side counters print once per second:

```
[beacon uart] bytes=4800 frames=300 resync=0
```

`resync` should be 0 on a clean wire. Non-zero means bytes were lost and the framer
re-anchored; it recovers without intervention, and the counter is the only trace.

---

## Verification

Compiled end to end against a WS63 SDK snapshot:

- `riscv32-linux-musl-gcc`, toolchain shipped with the SDK
- 765 targets, `Build target:ws63_liteos_app success`
- Output `ws63-liteos-app_all.fwpkg`, 1,427,432 bytes
- Zero warnings from the three files in `src/`; all warnings in the build log originate
  in the SDK's bundled mbedtls and libcoap

One compile error is expected on a fresh tree: `SLE_ADV_CHANNEL_MAP_DEFAULT` is not
exported by the SDK headers, the vendor sample defines it locally, and the definition is
reproduced in `src/beacon_adv.h`.

---

## Failure modes

Seven of them, each with a symptom, a root cause and a fix. The full write-ups are in
`docs/failure-modes.md`; this is the index.

| # | Symptom | Root cause |
|---|---|---|
| 1 | `Oops:NMI`, `mcause=0x8000000c`, reboot loop at ~20 s | 9 Hz stop/set/start against an asynchronous stop |
| 2 | Payload swap returns success, air keeps old bytes | hot update not reaching the air interface |
| 3 | Two boards, both transmitting, neither receiving | hardcoded local address filtered as self |
| 4 | Radio deaf after soft reset, survives watchdog reset | `sle_get_local_addr()` returning RAM residue |
| 5 | 455 injected frames, zero delivered, permanent | accumulator misalignment never re-anchors |
| 6 | Payload never found while walking TLVs | declared segment length shorter than actual |
| 7 | Stream goes silent, address changes | per-frame logging saturates the UART, watchdog fires |

Items 1 and 2 are two faces of the same root cause. Refresh requires
stop/set/start; stop is asynchronous; therefore refresh frequency is a correctness
constraint, not a performance knob.

---

## Non-goals

- **Unicast.** There is no ACK and no retransmission. Broadcast is "whoever is here,
  hears it". A lost PDU is lost.
- **Ordering.** PDUs from different senders carry no sequence relationship.
- **Security.** Records are plaintext and unauthenticated. Anyone in range can read and
  replay them. Pairing is what would normally provide authentication, and pairing is
  exactly what this design gives up.
- **Multi-hop.** Range is one link. No relaying.
- **Power management.** The radio announces continuously. A coin cell will not carry a
  node running this as-is.

---

## Limits

- Validated on one SDK snapshot (a 2025-01 WS63 tree). Items 2 and 6 are stack-behaviour
  observations and may change between releases.
- Density was not characterised. Several dozen nodes announcing every 12.5 ms in one
  space will contend; no measurement exists for that regime.
- The receive path keeps one "last seen" record, not a table. Multiple simultaneous
  peers are delivered to the application as a stream, with no per-sender state.
- DW1000 ranging, host-side decision logic, telemetry and the dashboard from the
  originating project are not part of this repository and were never intended to be.
  This is the transport substrate only.

---

## Provenance and license

MIT for the modifications, see `LICENSE`.

Derived from the HiSilicon WS63 SDK sample `sle_uart`, which ships under Apache License
2.0. The original copyright notice is preserved in every source header and in `NOTICE`,
as clause 4 of that license requires. No SDK headers or SDK sources are redistributed
here; obtain those from the vendor.

The announce modes used here are part of the SLE specification — all five are exported
by `sle_device_discovery.h`. What is not part of the specification is the decision to
carry application state in an announce PDU and to run the receive path entirely out of
the seek callback. That decision, and the constraints it drags in, are what this
repository documents.
