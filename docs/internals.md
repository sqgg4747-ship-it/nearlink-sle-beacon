# Internals

## Two contexts, kept apart

```
IRQ    uart_rx_isr()    align bytes, post to a queue. No stack calls, no allocation,
                        no logging.
Task   beacon_task()    everything that touches the radio.
```

This is not organisation for its own sake. `sle_stop_announce()` finishes asynchronously
inside `bt_service`, so a `set`/`start` from any other context races that pending stop and
leaves the stack's internal list inconsistent. The next preemption then faults. One task,
one cadence, one gate.

## Transmit

```
host
  | UART 115200
  v
uart_rx_isr()          anchor on 0xAA 0x55, slide on mismatch, accumulate to 16
  |
  v
message queue          5 slots
  |
  v
beacon_task()          dequeue, 500 ms timeout
  |
  v
beacon_send()          memcmp against what is on air; equal means return, no stack calls
  |
  v
beacon_flush()         200 ms gate -> stop / set_data / start
  |
  v
air                    12.5 ms interval
```

`beacon_send()` is idempotent as far as the radio is concerned. The pending slot holds one
record and a newer record overwrites it, which is deliberate: for a state beacon the
newest state is the only one worth sending, and a queue would just deliver stale
intermediates after a backlog.

## Receive

```
air
  |
  v
seek callback          length >= 16, linear scan for 0xAA 0x55
  |                    self filter against the cached local address
  |                    change detection against the previous record and sender
  |                    print: full on change, sampled on repeat
  v
beacon_rx_handler_t    (payload, src, rssi, is_self)
  |
  v
application
```

The scan is linear and ignores TLV lengths on purpose. The stack hands over a first
segment that declares a length shorter than the bytes it actually occupies, so a
length-stepped walk lands one byte off and runs off the end.

Self-originated frames are forwarded with a flag rather than dropped. With only one node
powered on, that flag is the difference between a working link and a false positive.

## Lifecycle

```
    set_param -> set_data -> start_announce
                 |
                 v
            ANNOUNCING  <--------+
                 |               |
      payload differs            | gate open
                 |               |
                 v               |
              PENDING -----------+
            (gate still closed)
```

PENDING never lasts longer than the remainder of the 200 ms gate, because the task loop
tries a flush every 50 ms.

## Rules

These hold at every point in the program, and each one is load-bearing:

1. Radio calls happen on the injection task. Nothing from the interrupt, nothing from the
   seek callback.
2. Two reconfigurations are at least 200 ms apart. The timestamp is written before the
   operation, so a failed attempt still consumes budget.
3. The payload on air always equals `g_beacon_on_air`. The copy is updated immediately
   before stop, so a diff against it stays truthful even if the sequence fails halfway.
4. A failed reconfiguration never leaves the node quietly repeating stale data. A stop
   failure leaves it not announcing, which is visible.
5. The local address is unique per board and stable within one boot.
6. The receive path never blocks: no allocation, no stack calls, bounded logging.

## Footprint

| item | size |
|---|---|
| UART RX ring | 512 B |
| message queue | 5 x 16 B |
| task stack | 0x1200 |
| everything else | under 80 B |

Task priority 28, tick 50 ms, queue timeout 500 ms. The timeout is short enough that the
flush cadence survives a silent host, and long enough that an idle task is not spinning.
