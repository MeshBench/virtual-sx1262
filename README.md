# virtual-sx1262

A virtual Semtech SX1262: the chip MeshBench's real MeshCore firmware talks to
over SPI, with the air underneath it simulated.

Everything above the SPI pins in a MeshBench run is the firmware's own code —
the application, the mesh logic, `CustomSX1262`, RadioLib. This is what answers
those pins. It is a model of a part, written against the datasheet and validated
against RadioLib's unmodified driver, and it is the reason a native node and an
emulated one can be compared at all: they answer to the same chip.

## Why this is its own repository

It used to live inside `MeshBench/meshcore-native`, as a file beside the host
firmware build, with one small server process wrapping it so emulators could
reach it over a socket. That put a shared model inside a repository named for
one of its consumers, and left everything else talking to it down a pipe.

It has four consumers, and none of them is more entitled to it than the others:

| host | how it links | its licence |
|---|---|---|
| `MeshBench/meshcore-native` — the native firmware build | the C++ class, in process | MIT |
| `MeshBench/qemu` — the `sx1262` device | the C ABI, `extern "C"` | GPLv2 |
| `MeshBench/meshbench` — the simulator | the C ABI, via cgo | GPL-3.0-or-later |
| Renode's SX1262 peripheral | the C ABI, via `DllImport` | MIT |

That table is why the licence is MIT and says so deliberately in `NOTICE.md`: a
GPLv2 host and a GPL-3.0 host cannot both link the same copyleft library, and
both of those hosts are real. Nothing here links MeshCore, RadioLib, or any
third-party code — only the C++ standard library.

## Building

```sh
./build.sh          # static + shared + tests, then run the tests
./build.sh static   # libvirtualsx1262.a       - for C and C++ hosts
./build.sh shared   # libvirtualsx1262.so|dylib|dll - for hosts that load at runtime
./build.sh test     # build and run the tests
```

No dependencies beyond a C++17 compiler.

## The ABI

`include/virtual_sx1262.h` is the whole surface. Two shapes in it were learned
from the socket it replaces, and both matter:

**Transactions, not bytes.** `vsx_spi_transaction` takes a whole chip-select
framed exchange rather than one byte at a time. A call per byte across a
managed-code boundary costs more than the socket it was meant to remove, and the
chip has to know where one command ends regardless — the SX1262 wire protocol
carries no length, so chip select is the only frame there is.

**The chip raises DIO1; nobody asks it.** The socket was request-response, so the
model had no way to call back and its hosts polled the line on a 1 ms timer —
a millisecond of latency on every received packet, for a pin a real part
asserts. `vsx_set_dio1_callback` pushes the edge instead. `vsx_dio1_asserted`
is still there for a host that would rather ask.

The C++ class is not part of the ABI. Hosts get an opaque `vsx_chip *`, so the
model can be rearranged without breaking a `DllImport`.

## What the tests are for

`test/test_model.cpp` is not coverage for its own sake. Every case in it is a bug
that previously took a four-minute emulator run to see, because the model had no
bench of its own:

- **A carrier that has ended is not still "present."** The model used to raise
  `PreambleDetected|HeaderValid|SyncWordValid` at `RxDone` — asserting a signal
  was arriving at the instant it stopped. MeshCore's
  `CustomSX1262::isReceiving` reads `HEADER_VALID` as "the channel is busy", so
  a repeater holding a packet to forward never saw a clear channel and relayed
  nothing. Detection flags are now raised as the signal arrives and cleared when
  it goes.
- **A packet handed to a deaf chip does not turn up later.** A frame delivered
  while the chip was not listening sat in the inbox until it started listening,
  however long that took, so a packet could arrive seconds after the air that
  carried it went quiet. It is now bounded, and dropped past the bound.
- **A brief re-arm gap still receives.** The bound is a short grace rather than
  an immediate drop, because the driver's own `startReceive` is
  standby → configure → SetRx: the chip is briefly not listening in a perfectly
  healthy receiver, and a frame landing in that gap is receivable.
- **The chip only lies when it is asked to.** Latching a detection flag past the
  signal is what real SX1262s sometimes do, and what MeshCore 1.17's recovery
  exists to survive — so it is a deliberate variant
  (`vsx_set_stuck_irq_ms`), never the default.

## What it does not model

Real errata, and a real preamble detector's behaviour under interference. The
air above it — path loss, noise, collisions, capture effect — is the simulator's
job, not the chip's; this part only ever sees "a carrier is present" and the
frames handed to it.
