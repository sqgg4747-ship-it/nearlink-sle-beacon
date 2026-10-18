# Changelog

Format follows Keep a Changelog. Versioning is not strict semver: the minor number moves
when the wire format or a public header changes, because those break deployed nodes.

## [0.1.0] - 2026-10-18

First release. The connectionless announce transport on its own: the application record
is left opaque, so whatever you put in bytes 2..15 is yours.

### Added

- Connectionless announce path: 16-byte record inside a vendor TLV, `NONCONN_SCANABLE`
  mode, 12.5 ms interval, TX power in the seek response.
- Receive path driven entirely from the seek callback: header scan, self-filter, change
  detection, application callback.
- UART framing that re-anchors on `0xAA 0x55` rather than counting bytes, so a dropped
  byte or an arbitrary chunk boundary recovers without a reset.
- Reconfiguration gate: `memcmp` dedup against the payload on air, plus a 200 ms floor
  between `stop` / `set_data` / `start`.
- Local address derived from `get_dev_addr(IFTYPE_SLE)` instead of a fixed constant.
- Static allocation throughout. No heap, no `malloc`, nothing to free.

### Known limitations

Broadcast only: cleartext, no acknowledgement, one hop, no power management. The limits
section of the README has the short version.

[0.1.0]: https://github.com/sqgg4747-ship-it/nearlink-sle-beacon/releases/tag/v0.1.0
