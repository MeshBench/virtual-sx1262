# virtual-sx1262

A virtual Semtech SX1262, with a C ABI. This is the chip real
[MeshCore](https://github.com/meshcore-dev/MeshCore) firmware talks to when it
runs under [MeshBench](https://github.com/MeshBench/meshbench), and the only
thing between an unmodified radio driver and a simulated sky.

This is not a model of MeshCore's radio driver. The driver is real and runs
unmodified: `CustomSX1262` over RadioLib, clocking bytes at a chip select. What
is ours is the part that answers those bytes. A simulator that answered them
approximately would be deciding listen-before-talk for the firmware, which is
answering a different question from the one anybody asked.

## Where the real ends and the simulation begins

Worth being exact about, because the interesting bugs live on the boundary.

| Layer | What runs |
|---|---|
| MeshCore application and mesh logic | **real**, unmodified |
| MeshCore radio driver, `CustomSX1262` and `RadioLibWrapper` | **real**, unmodified |
| RadioLib | **real**, vendored by the host, unmodified |
| **The SX1262 chip** | **this repository** |
| Arduino, board, filesystem, RTC, sensors, RNG | the host's, per host |
| The air: path loss, noise, collisions, capture effect | the simulator's |

The line matters in both directions. This part never decides whether a packet
gets through: it is told "a carrier is present" and handed frames. And it never
guesses at airtime: `vsx_est_airtime_ms` is Semtech's own formula, because
MeshCore's CSMA is built on the figure the firmware computes and a second
formula beside it would drift.

## Why this is its own repository

It used to be a file inside `MeshBench/meshcore-native`, beside the host firmware
build, with a small server process wrapping it so emulators could reach it down a
socket. That put a shared model inside a repository named after one of its
consumers, and left every other consumer talking to it through a pipe.

It has four, and none is more entitled to it than the others:

| Host | How it links | Its licence |
|---|---|---|
| `MeshBench/meshcore-native`, the native firmware build | the C++ class, in process | MIT |
| `MeshBench/qemu`, the `sx1262` device | this ABI, `extern "C"` | GPLv2 |
| `MeshBench/meshbench`, the simulator | this ABI, via cgo | GPL-3.0-or-later |
| Renode's SX1262 peripheral | this ABI, via `DllImport` | MIT |

One model, four hosts, no server. That table is also the whole of the licence
argument below.

## The ABI

`include/virtual_sx1262.h` is the entire surface, and two shapes in it were
learned from the socket it replaces.

**Whole transactions, not single bytes.** `vsx_spi_transaction` takes one
chip-select framed exchange. A call per byte across a P/Invoke boundary costs
more than the socket it was meant to remove, and the chip has to know where a
command ends regardless: the SX1262 wire protocol carries no length, so chip
select is the only frame there is.

**The chip raises DIO1. Nobody asks it.** The socket was request-response, so the
model had no way to call back and its hosts sampled the line on a one
millisecond timer instead. That is a millisecond of latency on every received
packet, for a pin a real part asserts the instant it has something to say.
`vsx_set_dio1_callback` pushes the edge. `vsx_dio1_asserted` is still there for a
host that would rather ask.

The C++ class is not part of the ABI. Hosts hold an opaque `vsx_chip *`, so the
model can be rearranged without breaking a `DllImport` written by hand.

## What the tests are for

`test/test_model.cpp` is not coverage for its own sake. Every case is a bug that
previously took a four minute emulated boot to see, because the model had no
bench of its own:

- **A carrier that has ended is not still present.** The model raised
  `PreambleDetected`, `HeaderValid` and `SyncWordValid` at `RxDone`, which
  asserts that a signal is arriving at the instant it stopped. MeshCore's
  `CustomSX1262::isReceiving` reads `HEADER_VALID` as "the channel is busy", so a
  repeater holding a packet to forward never saw a clear channel and relayed
  nothing at all. Detection flags are raised as the signal arrives and cleared
  when it goes.
- **A packet handed to a deaf chip does not turn up later.** A frame delivered
  while the chip was not listening waited in the inbox until it started
  listening, however long that took, so a packet could arrive seconds after the
  air that carried it went quiet.
- **A brief re-arm gap still receives.** The bound on that wait is a short grace
  rather than an immediate drop, because the driver's own `startReceive` is
  standby, configure, `SetRx`: the chip is briefly not listening inside a
  perfectly healthy receiver, and a frame landing in that gap is receivable.
- **The chip only lies when asked to.** Latching a detection flag past the signal
  is what real SX1262s sometimes do, and what MeshCore 1.17's recovery exists to
  survive, so it is a deliberate variant and never the default.

`test/test_c_abi.c` is compiled by the **C** compiler on purpose. Two hosts are
not C++, and a header that only builds under a C++ compiler passes every other
test here and then fails in the one place it matters.

## Building

```
./build.sh            everything, then run both test binaries
./build.sh static     libvirtualsx1262.a, for C and C++ hosts
./build.sh shared     libvirtualsx1262.{so,dylib,dll}, for hosts loading at runtime
./build.sh test       build and run the tests
./build.sh sanitize   the tests under AddressSanitizer and UBSan
```

`STRICT=1` turns warnings into errors, which is what CI uses. No dependencies
beyond a C++17 compiler, which is not an accident: see the licence.

## What it does not model

Real errata, and a real preamble detector's behaviour under interference. It is
our best understanding of an SX1262, which is a different thing from an SX1262,
and every result that rests on it is a best case.

## Licence

MIT, and deliberately rather than by inheritance.

Two of the four hosts above are copyleft, and they are not the same copyleft:
QEMU is GPLv2 as upstream ships it, MeshBench is GPL-3.0-or-later. Those two
cannot both link one copyleft library, because GPLv2-only and GPL-3.0 are
mutually incompatible. A permissive licence is the only thing that lets the same
chip answer a native node, a QEMU device and a Renode peripheral, and one chip
answering all of them is the entire reason those backends are comparable.

Nothing here constrains the choice from below: the model links the C++ standard
library and nothing else, so no third-party terms are inherited. It was already
MIT in `meshcore-native`, so this is where it stays rather than where it moved.
See `NOTICE.md`.
