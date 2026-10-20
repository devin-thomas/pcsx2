# Diagnostic control bridge, protocol 1

This private research-fork interface controls physical P1 digital inputs and
counts emulator VSync boundaries. It contains no game-specific logic, RAM writes,
savestate operations, screenshots, game-consumed marker, or renderer-alignment
claim. VSync boundaries are **not verified battle ticks**. Game consumption and
renderer alignment are unknown. The first counted interval can be a partial
frame if the VM was paused between VSync boundaries.

## Opt in and isolation

Start a **new, owned** PCSX2 process with `PCSX2_CONTROL_BRIDGE=1`, PINE enabled
in its private configuration, and a private data directory and socket directory.
On macOS, PINE uses `$TMPDIR/pcsx2.sock` (with `.SLOT` appended for a nondefault
slot). `-datapath ROOT` puts application data under `ROOT/PCSX2`; set
`UI/StartPaused=true` there. Check the process/socket state before launch and
never point this client at an existing user's instance. The environment flag is
read once per process. It does not enable PINE by itself.

Without this flag, legacy PINE dispatch/transport behavior is preserved, and
opcode `0x10` is rejected as an unknown legacy command. With this flag, **all
legacy PINE commands are refused throughout the instance**, including reads,
writes, save and load. This avoids old unsynchronized work interfering with a
lease or queued acquire. There is one socket client at a time and one outstanding
action. This is a trusted local diagnostic interface, not remote authentication.

Acquire requires a fully paused VM, outside CPU execution, a connected DualShock2
at unified pad index 0 (physical P1, slot 0), and a neutral pad. Port must be zero
for every command. Other ports are unsupported. Both sticks must already be
centered at `(127,127)` and every bind released. Recording tools (even inactive),
an active recorder, `GG_SCRIPT`, `GG_INPUT_LOG`, hardcore achievements, GS dump
replay, and pending legacy frame advance are refused. Environment-variable
presence is enough to refuse a script, even if its value is empty.

During a lease, normal P1 setters/resetters and P1 macro output are suppressed;
macro countdown/toggle/input bookkeeping continues for every port. Other ports'
macro output is unchanged. Script/recording polling is skipped. This explicit arbitration
persists while paused. Host bindings for other pads continue to poll. Controller
configuration changes invalidate the lease before replacing the pad. External
resume is refused and invalidates the lease; external pause/reset/load/shutdown
also invalidates it. Release leaves the VM paused and P1 neutral; no prior
running state or pad state is restored.
In diagnostic mode, reset/load requests inside CPU execution are refused after
invalidation, even if an earlier callback has already assigned Paused. Retry
after execution unwinds. This avoids resetting or loading memory from inside
the remainder of a VSync event.

## Framing

All integers are **little endian**, unsigned, with no struct padding. A request
is `u32 total_length`, `u8 opcode=0x10`, then the following 32-byte header and
optional ranges. Exactly one bridge operation is permitted per PINE frame.
Request frame length is 37 through 293 bytes; shorter malformed frames either
fail or disconnect. Oversized transport lengths disconnect before payload read.

| Header offset | Type | Meaning |
| --- | --- | --- |
| 0 | u16 | protocol version, exactly 1 |
| 2 | u8 | command |
| 3 | u8 | port, exactly 0 |
| 4 | u64 | epoch, zero only for Hello |
| 12 | u64 | nonzero increasing request ID |
| 20 | u32 | digital mask, Step only |
| 24 | u32 | boundary count, Step only, 1..300 |
| 28 | u16 | range count, Snapshot only, 1..32 |
| 30 | u16 | reserved, zero |
| 32+8*i | u32,u32 | physical EE RAM address, byte length |

Non-Step masks/counts and non-Snapshot ranges must be zero. Bounds on range
count and exact frame size are checked before allocating the range list.
Snapshot validates the **entire** list before copying: each length is nonzero,
total bytes <=4096, and every byte lies in physical EE main RAM
`[0,0x02000000)`. Virtual aliases, MMIO, scratchpad and other memory are refused.
Returned bytes are concatenated in request order, including overlaps if asked.

| Command | Value | Behavior |
| --- | --- | --- |
| Hello | 0 | Explicit capabilities/version handshake; epoch=0; once per connection |
| Acquire | 1 | Require current epoch and supported paused/neutral state; return a new lease epoch |
| Step | 2 | Apply pad on CPU thread before resume; return **Pending**, not completion |
| Query | 3 | Read current CPU-thread receipt; does not renew the deadline |
| Snapshot | 4 | Copy bounded physical RAM only after a completed control action, with lease paused outside CPU execution |
| Cancel | 5 | Neutralize/pause, invalidate lease/epoch; return Cancelled |
| Release | 6 | Neutralize and leave paused; active actions are cancelled, otherwise return Ok |

IDs increase within a connection, across acquire/release cycles. Well-formed
requests reaching ID validation consume the ID even when later rejected.
Version/port/command errors occur before ID validation. Duplicate Hello is Busy.
Acquire, release, cancellation, timeout, and VM lifecycle changes advance the
epoch. The initial process epoch is seeded from the native steady clock;
epochs are process-local generation tokens, not a durable cross-process ID.
Hello is required on each new socket connection. Every new lease resets the
previous action receipt, preserving the current boundary counter.

## Replies and validity

A reply is `u32 total_length`, `u8 pine_status`, then a 112-byte header and data.
The PINE status is 0 only for Ok or Pending; otherwise it is 255. Unknown legacy
opcodes return the legacy five-byte failure with no bridge header. Never infer
completion from the PINE tag alone. The bridge header layout is:

| Reply offset | Type | Meaning |
| --- | --- | --- |
| 0 | u16 | version=1 |
| 2 | u16 | status of this command |
| 4 | u16 | retained action status |
| 6 | u16 | validity flags: bit0 leased, bit1 applied valid, bit2 completed valid |
| 8 | u64 | current epoch |
| 16 | u64 | this request ID |
| 24 | u64 | action request ID (Step), zero when none |
| 32 | u64 | current VSync boundary ID |
| 40 | u64 | pad-applied boundary ID |
| 48 | u64 | completed boundary ID |
| 56 | u64 | pad-applied native steady-clock nanoseconds |
| 64 | u64 | completion native steady-clock nanoseconds |
| 72 | u32 | exact executed digital mask |
| 76 | u32 | released mask, zero |
| 80 | u32 | requested boundary count |
| 84 | u32 | observed boundary count for the action |
| 88 | u32 | max boundaries=300 |
| 92 | u32 | max snapshot bytes=4096 |
| 96 | u16 | max ranges=32 |
| 98 | u16 | capabilities=15: digital P1, centered sticks, async receipt, physical RAM |
| 100 | u32 | data byte length |
| 104 | u64 | epoch in which the retained action was applied, zero when none |
| 112 | bytes | snapshot data, otherwise empty |

Input mask bits 0..15 are respectively Up, Right, Down, Left, Triangle, Circle,
Cross, Square, Select, Start, L1, L2, R1, R2, L3, R3. Pressed pressure is 255,
released pressure is zero. Both sticks are explicitly `(127,127)` for executed
and released input. Analog toggle and pressure modifier are released. These
bits are the UI bind order, not the active-low DS2 serial button bit order.

Status codes, in order: Ok=0, Pending=1, Disabled=2, Malformed=3, Version=4,
Handshake=5, RequestID=6, Epoch=7, Owner=8, Busy=9, VMState=10, Pad=11,
Conflict=12, NotNeutral=13, Bounds=14, Cancelled=15, Timeout=16,
Disconnected=17, Invalidated=18, Unsupported=19.

The command status and retained action status are separate. A Query can return
Ok while its action is Pending. Completion requires the matching action ID and
epoch, action_status=Ok, and completed-valid. `applied` is the **current**
boundary at the actual pad application while paused. It is not forced to
requested+1. On success, `completed - applied == completed_count == requested_count`.
The retained action epoch is distinct from the current epoch after release or
invalidation; historical applied/completed fields belong to the action epoch.
Times are sampled after pad application and after completion synchronization;
they are host monotonic time, not emulated cycles or UTC. Completed-valid is
false on failures. Counts on a failed receipt are only observations before
invalidation, not a successful transaction or proof of zero subsequent work.

## CPU ordering and failure cleanup

The IPC thread parses, queues a bounded CPU-thread operation, then waits up to
two seconds for its receipt. A Step receipt acknowledges pad application and
resume only. It never waits on the CPU thread for that same thread to advance.
The client polls Query for completion and may send Cancel between queries.

The CPU thread owns the state machine and pad. P1 host writes are suppressed
after application. The bridge counts at the tail of `Counters.cpp::VSyncStart`,
after GS push, input polling and VBlank work. At the requested final boundary,
it releases P1 and requests pause. **Only after `Cpu->Execute()` returns to
`VMManager::Execute`**, with the remaining event callback work finished, does it
drain VU/GS work and publish completed-valid. It does not acknowledge the early
pause assignment used by legacy FrameAdvance. Snapshot is dispatched to the CPU
thread and refuses both in-execution and in-flight states. It requires Paused,
waits for VU and GS work, then copies EE RAM before any further CPU execution.
An arbitrary initially paused point has no completed control boundary receipt;
run a neutral Step first. Snapshot before a verified completion is Busy and
invalidates the lease. This prevents labeling a mid-frame initial pause as a
completed VSync snapshot.

The lease/action deadline is ten host seconds from acquire/Step; successful
Snapshot renews it. Query does not extend it. Native boundary and idle polling
enforce it. Each socket receive/send wait is bounded to five seconds, including
partial requests; trickled bytes do not renew the independent lease deadline.
Disconnect queues CPU-thread pause/neutralization. Malformed owner packets and
rejected owner commands fail closed by invalidating that connection's lease;
legacy/opcode errors also queue connection-checked disconnect cleanup. Rejected
foreign requests cannot invalidate the owner's lease or change its active receipt
or request-ID sequence. Only owner errors perform lease cleanup; independently
expired deadlines still apply regardless of the caller. Request/epoch errors cannot
silently replay an action. A second Step returns Busy and cancels the lease.

IPC wait expiry returns Timeout with epoch=0 and both validity bits clear: the
CPU outcome is unknown. It marks queued work abandoned and queues disconnect
cleanup. A late CPU callback checks abandonment before executing and again
before publishing; if execution raced expiry, it neutralizes/pauses instead of
publishing success. Cleanup necessarily waits until the CPU thread can run;
there is no claim of immediate neutralization if that thread is wedged. No
error receipt claims a verified paused/completed boundary merely from a bool.

## Build and checks

Use the existing x86_64 dependency setup, not the host's default arm64 target:

```sh
TMPDIR="$PWD/.bridge-build/tmp" /Volumes/PortableSSD/stagepan/venv/bin/cmake \
  -S . -B .bridge-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DCMAKE_MAKE_PROGRAM=/Volumes/PortableSSD/stagepan/venv/bin/ninja \
  -DCMAKE_PREFIX_PATH=/Volumes/PortableSSD/stagepan/pcsx2/deps \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 -DENABLE_QT_UI=ON \
  -DUSE_SYSTEM_LIBS=OFF -DENABLE_TESTS=ON
TMPDIR="$PWD/.bridge-build/tmp" /Volumes/PortableSSD/stagepan/venv/bin/cmake \
  --build .bridge-build --target pcsx2-qt control_bridge_test -j 4
/Volumes/PortableSSD/stagepan/venv/bin/ctest \
  --test-dir .bridge-build/tests/ctest -R '^control_bridge_test$' --output-on-failure
PYTHONDONTWRITEBYTECODE=1 python3 tools/control_bridge_client.py --self-test
```

The test target executes the production state machine and wire codec with a
fake environment. It does not prove native VM/pad/renderer integration. The
client's `--self-test` is also fixture-only. For a separately verified owned
instance, use an explicit endpoint; omit `--steps` for capability/error checks
without a booted VM, or set `--steps 1 --mask 0` with a paused neutral VM.
The client releases a successful lease and always closes its socket. It does
not launch/shutdown an emulator or restore a state. Its stdout omits RAM bytes
from the live smoke.

The initial macOS build compiled the real emulator core and tests, but full
`pcsx2-qt` packaging was blocked by Metal tool discovery. A later repair built
the full target using Apple's actual cryptex compiler/linker, through an owned
`xcrun` wrapper with explicitly resolved tool paths and an SSD-local cache.
This avoids the build subprocess selecting the Xcode placeholder compiler.
No global toolchain installation or prebuilt shader/app substitution is needed.
Logs belong in ignored `.bridge-build`;
do not stage generated files, proprietary BIOS/game data, or local settings.
Cross-platform integration and actual game-consumption timing need proper testing.
