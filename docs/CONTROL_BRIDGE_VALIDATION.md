# Control bridge validation, 2026-10-20

Parent verification of the owner research fork, based on `gg-layers` commit
`f45e47466a8191c611d1ea4b9738b71eea5959e9`. See
[protocol and limits](CONTROL_BRIDGE.md).

The full x86_64 macOS app compiled with the real Apple Metal compiler and
linked successfully. All 24 native state/codec tests and 4 Python codec tests
passed. Windows source and header registration was checked as XML; no Windows
compile was performed. The reviewer's foreign-client reproducer was recompiled
against the repaired header: a rejected foreign Hello preserves the owner's
active lease. Player 1 macro suppression now passes through the controller
setter while macro bookkeeping and other ports continue.

The worker's isolated launch could not allocate shared memory inside its
process sandbox. The parent then launched a separate owned instance, using
private settings, copied BIOS, fresh memory cards and a unique socket. The
first parent attempt used Qt's offscreen platform and crashed in Cocoa refresh
lookup. That attempt is preserved. Using the normal Cocoa platform, both the
enabled and disabled processes exited 0 and were reaped; their sockets were
removed.

The enabled-instance smoke verified:

- Version/capabilities handshake and paused neutral acquisition.
- Actions of 1, 2 and 17 boundaries, with matching action ID/epoch, masks,
  monotonic timestamps, exact counts and completed validity.
- Synchronized paused snapshots at the matching completed boundary.
- Cancellation and disconnect, followed by successful paused neutral
  reacquisition. Failed action receipts do not certify zero further work.
- Legacy PINE refusal while explicitly enabled, and bridge-opcode refusal with
  working legacy status access while disabled.

The enabled instance reached boundary 22. Successful actions account for 20;
cancellation/disconnect can finish callback work before execution stops. These
are emulator-boundary diagnostics during boot, not validated game ticks,
combat intervals or proof that a particular input was consumed by the game.
No learning, reward/lifecycle calibration or qualifying evaluation is claimed.

The tested executable SHA-256 was
`3b3e47935513fe27378075fbdee018364e5b8418e927138f09b7be88277dd7a1`.
The owned process identified the disc as `SLUS-21847`, version 1.00, CRC
`F233A433`; loaded game-code identity still requires a separate RAM comparison.
The original emulator checkout and original BIOS were unchanged.

Full worker sessions, failed attempts, source manifests, build/test logs and
parent receipts are retained in SSD scratch evidence. Generated build products,
settings, game/BIOS bytes, raw memory and worker logs are excluded from Git.
