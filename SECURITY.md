# Security

A transport, not a security layer.

Records go out in the clear, and nothing in the protocol marks who sent one. That is the
normal shape for a broadcast: a receiver learns that some node in range is advertising a
value, not that a particular node is. Applications that need to know the sender have to
establish that above this layer — pairing is the usual mechanism, and connectionless
operation is what this library trades it for.

`main` is the only supported branch. Bugs go in the issue tracker; anything you would
rather not post publicly can go through GitHub's private vulnerability reporting.
