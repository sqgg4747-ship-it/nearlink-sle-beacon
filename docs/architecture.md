# Architecture

## Context split

Two execution contexts, strictly separated.

```
IRQ   uart_rx_isr()          byte alignment, message queue post
      - no stack calls
      - no allocation
      - no logging

Task  beacon_task()          everything that touches the radio
      - beacon_init / beacon_send / beacon_flush
      - all announce API calls
      - all diagnostics
```

The separation is not stylistic. `sle_stop_announce()` completes asynchronously inside
`bt_service`; issuing a `set`/`start` from a second context races that pending stop and
corrupts internal stack state. One task, one cadence, one gate — see
`failure-modes.md`, item 1.

## Transmit path

```
host MCU
  |  UART 115200
  |  AA 55 <14 bytes>
  v
uart_rx_isr()                    irq
  |  anchor on 0xAA 0x55, slide on mismatch
  |  accumulate to 16 bytes
  v
osal message queue               capacity 5 x 16 bytes
  |
  v
beacon_task()                    task
  |  dequeue, 500 ms timeout
  v
beacon_send()
  |  memcmp against g_beacon_on_air
  |  identical -> return, zero stack calls
  |  differs   -> stage into g_beacon_pending, set dirty
  v
beacon_flush()
  |  gate: now - last_op >= 200 ms
  |  too soon -> leave pending, return
  |  else      -> stop, set_data, start
  v
air                             12.5 ms interval, 80 PDU/s
```

`beacon_send()` is idempotent with respect to the air interface: repeated submits of the
same bytes produce no radio work. The pending slot holds exactly one record; a newer
record overwrites it. That is deliberate — for a state beacon, the newest state is the
only state worth transmitting, and a queue would deliver stale intermediate states after
a backlog.

## Receive path

```
air
  |
  v
beacon_seek_result_cbk()         stack task context
  |  data_length >= 16
  |  linear scan for 0xAA 0x55
  |  compare src against cached local address -> SELF / PEER
  |  compare against last payload and last src -> changed?
  |  print: full on change, sampled on repeat
  v
beacon_rx_handler_t              application callback
  |  (payload, src, rssi, is_self)
  v
application
```

The scan is linear and does not consult TLV lengths. The stack delivers a first segment
that declares length 1 while occupying 3 bytes, so a length-stepped walk lands one byte
off and runs off the end (see `failure-modes.md`, item 6). Receiving by byte pattern
costs at most a few dozen comparisons per PDU and is independent of how the sender built
the PDU.

Self-originated frames are forwarded, not silently dropped. The `is_self` flag lets the
application decide; in a single-board test that flag is the difference between a working
link and a false positive.

## Announce lifecycle

```
        +-----------+
        |  INIT     |  set_param -> set_data -> start_announce
        +-----+-----+
              | start ok
              v
        +-----------+   payload differs,
        | ANNOUNCING|<-- gate open -------+
        +-----+-----+                    |
              |                        +-+----------+
              | payload differs,       | RECONFIG   |
              | gate closed            | stop       |
              v                        | set_data   |
        +-----------+                  | start      |
        |  PENDING  |------------------+            |
        +-----------+   gate opens     +------------+
```

`ANNOUNCING` is the steady state. `PENDING` never persists longer than the residual of
the 200 ms gate, because `beacon_flush()` runs on every 50 ms pass of the task loop.

## Invariants

These hold at every point in the program, and each is load-bearing.

1. **Every radio call happens on the injection task.** No announce API is reachable from
   the interrupt handler or from the seek callback.
2. **Two announce reconfigurations are at least 200 ms apart.** Enforced before the
   stack is touched, and the timestamp is written before the operation, so a failing
   reconfiguration still consumes budget.
3. **The announce payload on air always equals `g_beacon_on_air`.** The copy is updated
   immediately before stop, so a diff against it is truthful even if the reconfiguration
   afterwards fails partway.
4. **A failed reconfiguration never leaves the node silently announcing old data.** Each
   step logs its error code; a stop failure leaves the node not announcing, which is
   visible, rather than announcing stale content.
5. **The local address is unique per board and stable within one boot.** Cache and stack
   are written in the same call path, so self-filtering compares like with like.
6. **The receive path never blocks.** No allocation, no stack calls, bounded logging.

## Resource footprint

Static only. Nothing is allocated at runtime.

| Item | Size |
|---|---|
| UART RX ring | 512 B |
| Message queue | 5 x 16 B |
| Task stack | 0x1200 B |
| Accumulator, payload copies, counters | under 80 B |

Task priority 28. Tick 50 ms. Queue read timeout 50 ticks (500 ms), short enough that the
flush cadence holds with the host silent, long enough that an idle task is not spinning.
