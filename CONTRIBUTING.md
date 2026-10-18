# Contributing

This is a single-maintainer project. There is no roadmap, no release cadence, and no
guarantee that a pull request gets reviewed. With that in mind:

## Bug reports

Open an issue. A report that can be acted on contains:

- the board (BearPi-Pico H3863, bare WS63 module, something else)
- the SDK snapshot you built against
- the UART log around the failure, not only the last line
- how many nodes were powered on

That last point matters more than it sounds. Most "it does not receive" reports turn
out to be an address collision, and you cannot see that from a single node. The README
has three notes on this, the second one being the address.

## Pull requests

Small, focused fixes are welcome. Things that will not be merged:

**Reformatting.** Do not run clang-format over a file and send it back. The diff becomes
unreadable and the change gets rejected on that basis alone.

**Wire format changes without discussion.** The format is `docs/protocol.md`. Changing it
breaks every deployed node. Open an issue first.

**Anything that moves an `sle_*` call into interrupt context or into the seek callback.**
Both are non-negotiable. See `docs/internals.md`.

## Before you send a change

1. Build it.

   ```bash
   python build.py ws63-liteos-app -j8
   ```

   Confirm `Build target:ws63_liteos_app success`.

2. Run it on more than one node. A change that merely compiles is not verified. The
   behaviour this library exists for only appears once two radios are actually talking.

3. If you touched the announce path, state in the PR what the counters looked like
   before and after. `bytes / frames / resync` from the UART side, plus a few `[RX]`
   lines. Those four numbers catch most regressions.

## Style

Plain C99, SDK conventions. Match the file you are editing rather than introducing a new
style; there is no formatter enforced here and no linter configuration to satisfy.

Comments should carry something the code does not already say. A comment that restates
the line beneath it will be removed, and that is the only style rule that gets enforced.
