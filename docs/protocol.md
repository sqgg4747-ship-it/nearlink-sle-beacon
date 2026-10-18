# Wire format

This document fixes the byte layout so that independent implementations can exchange
records. It describes what this library places on air, not a proprietary extension to
SLE.

## 1. Announce PDU

`sle_set_announce_data()` receives an application buffer that this library assembles as
two consecutive TLVs:

```
offset  bytes        field
0       02           TLV length: 2 (type + value)
1       01           TLV type: discovery level
2       01           value: 0x01 (normal)
3       11           TLV length: 17 (type + 16-byte record)
4       FF           TLV type: vendor-specific
5       00..0F       application record, BEACON_PAYLOAD_LEN bytes
```

Total: 21 bytes. The vendor TLV length is `BEACON_PAYLOAD_LEN + 1` because the type octet
counts toward the declared length.

The stack may prepend segments of its own before this buffer. Do not assume the record
starts at offset 5 on air — the receiver never does.

## 2. Seek response PDU

```
02 0C 12             TX power TLV: length 2, type 0x0C, 18 dBm
0B 0A "sle_beacon"   local name TLV: length 10, type 0x0B, 10 characters
```

Present so a generic SLE scanner shows something recognisable. Nothing in the receive
path depends on it, and a seek response arriving without a record is counted as
`RAW DROP` and sampled, not treated as an error.

## 3. Application record

Opaque to this library. Two structural conventions:

```
offset 0    0xAA     frame header, high
offset 1    0x55     frame header, low
offset 2..  15       application-defined
```

The header exists for receiver-side framing only. It is the anchor the seek callback
scans for; the library never validates or interprets bytes 2 through 15. Substituting a
different two-byte pattern is legal as long as both ends change it together — there are
two `HEAD0`/`HEAD1` definitions, one in `beacon_rx.c` and one in `beacon_uart.c`.

A record shorter than 16 bytes is rejected at the API boundary, and a PDU shorter than 16
bytes is discarded before scanning. There is no partial-record mode: a beacon either
states its full state or stays silent.

## 4. Byte order

The library imposes none. Application fields inside the record are copied verbatim from
the host UART frame, so whatever convention the host uses is preserved end to end.
Multi-byte integers should be fixed-endian in the application spec; little-endian is the
common choice on the host side.

## 5. Receiver acceptance rules

A PDU is accepted when all of the following hold, in this order:

1. `data_length >= 16`
2. some index `i` satisfies `data[i] == 0xAA` and `data[i+1] == 0x55`, with
   `i + 16 <= data_length`
3. the 16 bytes at `i` are forwarded regardless of TLV structure

Sender identity comes from the stack's source address, not from the payload. Signal
strength likewise comes from the stack, at 1 dBm resolution, signed.

## 6. Duplicate suppression

Transmit side: byte comparison against the payload currently on air. Equal bytes produce
no radio operation.

Receive side: a record is considered new when either its bytes or its source address
differ from the previous accepted record. There is one stored record, not a table; with
multiple senders interleaved, every change of sender registers as a new event.

Print output is sampled for repeats at 1 in 40, and an aggregate counter is emitted once
per second. This affects logging only. The application callback receives every accepted
record.

## 7. Versioning

The record carries no explicit version field, and the library does not add one. Projects
that need one should place it at offset 2, keep the header at offset 0..1, and treat
unknown versions as unreadable rather than as a best-effort parse. Silent
misinterpretation of a state record is worse than dropping it.

## 8. Interoperability notes

- The announce interval is 12.5 ms, which is a property of the sender. A receiver must
  not assume any particular arrival cadence; a listener that expects one record per N
  milliseconds will misbehave against a different sender.
- Because there is no connection, there is no negotiation. Receiver and sender must agree
  on `BEACON_PAYLOAD_LEN`, `BEACON_TLV_TYPE` and the header bytes out of band.
- A node listening on a different `BEACON_TLV_TYPE` still finds the record, because
  acceptance does not consult the TLV chain. This is intentional: the record is located
  by header scan, and the TLV wrapper exists only so the PDU is well-formed for
  third-party scanners.
