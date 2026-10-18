# Failure modes

Seven faults met while converting the transmit path from connection-oriented to
connectionless. Each entry states the symptom as observed, the mechanism behind it, and
the change that resolves it. Numbers are from captures, not estimates.

---

## 1. Reconfiguring at 9 Hz corrupts `bt_service`

**Symptom**

Host pushes 16-byte records at roughly 9 Hz. Legacy path issued stop/set/start per
record. Over a 60 s capture:

- 729 announce restarts
- three `Oops:NMI` faults, `mcause=0x8000000c`, `mepc` inside `OsSchedPreempt`,
  one roughly every 20 s
- continuous soft-reset loop

**Mechanism**

`sle_stop_announce()` is asynchronous. It returns once the request is queued; the stop
itself runs later, on `bt_service`. The `set` and `start` that follow therefore race a
pending stop. After a few collisions the stack's internal list is inconsistent, and the
next preemption takes the fault.

A second observation from the same capture: of 484 distinct host frames, 482 were
byte-identical. 99.6% of those restarts carried no new information at all.

**Fix**

Three layers, in order of effect:

1. `memcmp` against the payload currently on air. Identical input makes zero stack calls.
2. Hard floor of 200 ms between two announce reconfigurations.
3. A rejected payload goes into a single pending slot with a dirty flag, drained by
   `beacon_flush()` from the task loop.

**Result**

Restart rate 12 Hz -> 0.24 Hz. NMI faults: zero over the same capture length.

---

## 2. Live payload update returns success and does nothing

**Symptom**

With announcing active, call `sle_set_announce_data()` to swap the content. Return value
is `ERRCODE_SLE_SUCCESS`. Capture at the peer: 943 frames over 12 s, every one carrying
the initial `AA 55 01 FF 00 00 ...`, never the new bytes.

**Mechanism**

On this stack build the call updates the host-side buffer only. The change never reaches
the air interface, and no error is surfaced. Success is reported for an operation that
did not happen.

**Fix**

Always go through the trio:

```c
sle_stop_announce(handle);
beacon_set_data();
sle_start_announce(handle);
```

**Relation to item 1**

Same root cause, opposite face. Because a real refresh requires stop/set/start, and stop
is asynchronous, a high refresh rate is guaranteed to collide. The 200 ms gate is
therefore a correctness requirement. Treating it as a performance tuning knob
reintroduces item 1.

---

## 3. Hardcoded local address makes two boards mute to each other

**Symptom**

Two boards flashed with the same image, both announcing, both seeking, neither receives
the other. Nothing anomalous in either log. The `seek_result` callback is never entered.

**Mechanism**

Vendor sample, line 134:

```c
unsigned char local_addr[SLE_ADDR_LEN] = {0x01,0x02,0x03,0x04,0x05,0x06};
```

Both boards claim one address. The stack filters inbound announcements by comparing the
source against the local address, concludes every frame is its own reflection, and drops
it before the callback.

**Fix**

Derive from the chip:

```c
get_dev_addr(mac, 6, IFTYPE_SLE);
sle_set_local_addr(&addr);
```

The second call matters as much as the first. Without it the stack's notion of the local
address diverges from the announce parameter, and self-filtering stops matching anything,
which fails in the opposite direction: the node accepts its own frames as peers.

---

## 4. `sle_get_local_addr()` returns RAM residue after soft reset

**Symptom**

After `hal_reboot_chip()`, the call repeatably yields `08 00 45 00 08 00`. Same five
bytes on every restart, which reads like IP stack buffer remnants rather than a MAC.

Seeking with that address leaves the radio permanently deaf. A watchdog reset does not
help, because the address is rebuilt the same broken way, so the failure is a loop.

**Fix**

Do not use `sle_get_local_addr()`. Use `get_dev_addr(IFTYPE_SLE)`, which rebuilds on
each boot from the stack, the NV factory MAC, EFUSE, then a random fallback. If all four
fail, fall back to a fixed address and log it — a colliding address is recoverable, an
address of dead bytes is not.

---

## 5. One lost byte misaligns the injector permanently

**Symptom**

Legacy path counted to 16 bytes, then posted. After a single dropped byte the
accumulator stays shifted forever: every following block is cut from inside a frame, the
application's `buf[0] == 0xAA` check never passes, and nothing recovers short of a reset.

Measured: 455 injected frames across five host rates from 100 ms to 2 s, zero delivered.

**Fix**

Treat the port as an unframed stream anchored on `0xAA 0x55`:

- empty accumulator accepts only `0xAA` as a start byte, everything else is discarded
- with only `0xAA` held, the next byte must be `0x55`; otherwise the current byte is
  re-tested as a start byte, which slides the alignment by one
- once aligned, 16 bytes are posted to the message queue

Any dropped byte and any chunk boundary resynchronises. Three counters —
`bytes`, `frames`, `resync` — distinguish "nothing arrived" from "arrived but never
framed" from the log alone.

---

## 6. Walking the TLV chain lands one byte off

**Symptom**

Iterating segments by declared length never locates the payload.

**Mechanism**

The stack prepends segments of its own ahead of the buffer handed to
`sle_set_announce_data()`, and at least one of them declares a length shorter than the
bytes it occupies. On this build the delivered PDU reads:

```
<stack prefix> | 02 01 01 | 11 FF | AA 55 ...
```

Stepping by `field_len` lands one index past the vendor segment header. From there the
reader sees `0xFF` (255) as a length, trips a bounds check, and breaks out without ever
comparing the record. The prefix layout is not contractual and shifts between SDK
revisions, which is the second reason not to walk it.

**Fix**

Ignore length semantics entirely. Scan the buffer for `0xAA 0x55` and take the following
16 bytes. This survives SDK revisions precisely because it assumes nothing about how the
far end assembled the PDU.

---

## 7. Per-frame logging saturates the UART and trips the watchdog

**Symptom**

One log line per received frame, roughly 120 characters. At 80 PDUs/s that is 9600
characters per second against a 11520 characters per second ceiling at 115200 baud.

Result: the stream goes silent after some frames, the local address comes back
randomised, and the peer stops receiving.

**Mechanism**

Saturating the log port blocks the task hosting the SLE callback. The task stalls, the
watchdog resets the node, the address changes, and the link has to be rebuilt from
scratch.

**Fix**

- payload or sender changed -> full print
- repeat -> one line every 40 frames
- once per second -> aggregate counter

Line length is bounded too: hex output uses fixed-width assembly rather than `snprintf`,
which on this target is slow and stack-hungry enough to matter inside a callback.

---

## Appendix: traps that were avoided rather than hit

- `sle_announce_seek_register_callbacks()` accepts one table. Calling it twice replaces
  the previous callbacks wholesale. The symptom is "transmits fine, receives nothing",
  with no diagnostic output whatsoever.
- Stack APIs are off limits in interrupt context. stop/set/start all belong to the task.
  The UART callback does alignment and posting only, nothing else.
- Use `uapi_tcxo_get_ms()` for pacing. Counting loop iterations as a time base drifts as
  soon as the inbound frame rate changes, because each pass does a different amount of
  work.
