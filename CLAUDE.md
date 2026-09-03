# virtual-sx1262

A virtual Semtech SX1262 with a C ABI, linked in process by four hosts: the
native MeshCore build in `MeshBench/meshcore-native`, the `sx1262` device in
`MeshBench/qemu`, Renode's C# peripheral, and `MeshBench/meshbench` through cgo.

**MIT, and it has to stay permissive.** Two hosts are copyleft and not the same
copyleft (QEMU is GPLv2, MeshBench is GPL-3.0-or-later), which cannot both link
one copyleft library. See `## Licence` in `README.md` before touching `LICENSE`.

## The one rule

**No dependencies.** The model includes the C++ standard library and nothing
else. Not RadioLib, not MeshCore, not a test framework, not a logging library.
That is what makes MIT honest and what lets a GPLv2 host link it. A pull request
that adds an include from outside the standard library is answering the wrong
question, however convenient the library is.

## Style

Enforced in CI, not by review:

```bash
STRICT=1 ./build.sh        # warnings are errors, and the C header builds as C
./build.sh sanitize        # AddressSanitizer and UBSan, clean
clang-format --dry-run --Werror include/*.h src/* test/*
```

`./build.sh pedantic` adds `-Wconversion`, `-Wsign-conversion` and
`-Wold-style-cast`. It is **expected to fail** (31 findings at the time of
writing) and is deliberately not wired into CI. The model is register work:
packing datasheet fields into bytes narrows on purpose, and a C-style cast reads
the way the datasheet does. Clearing it means auditing every narrowing to tell
the deliberate ones from the accidental ones, which is owed work and not a flag
flip. The target exists so the backlog is measured rather than hidden; new code
should not add to it.

C++17 for the model, C11 for the ABI header. The header is compiled by a C
compiler in CI on purpose: two hosts are not C++, and a C++ism that slips into
it passes every other test here and then fails in the one place it matters.

## Limits

Mechanical, because taste does not survive scale.

| Rule | Limit |
|---|---|
| File length | 300 lines soft, **500 hard** |
| Function length | 50 lines soft |
| Nesting depth | 4 |
| Dead code | none, git remembers |
| Third-party include | none, see the one rule |
| Exceptions | none; `-fno-exceptions` is on, because unwinding into C or across a P/Invoke is undefined |
| Allocation in the SPI path | none per byte; a transaction is one call for a reason |
| Speculative ABI | none; add an entry point at the *second* host that needs it |
| Comments | explain *why*, never *what*, and never cite a ticket the reader will not have |
| Em-dashes | none, in code, docs or commits; comma, colon, stop, or spaced hyphen |

## The ABI is a contract

Four hosts link this, one of them by a `DllImport` declaration written by hand
from the header. So:

- **Never change the meaning of an existing entry point.** Add a new one.
- **Never reorder or resize a struct** that crosses the boundary. Append only,
  and only at the end.
- **Bump `VSX_ABI_MINOR`** when you add; bump `VSX_ABI_MAJOR` only when you have
  accepted that every host must be rebuilt, and say so in the commit.
- **Every entry point tolerates a null handle** and returns a quiet zero. A host
  that failed to create a chip should not crash inside the library it just
  loaded. `test_c_abi.c` checks all of them.
- **The C++ class is not the ABI.** Hosts hold an opaque pointer. Rearranging the
  class is free; changing the header is not.

## Domain rules that are easy to get wrong

- **This models a chip, not the air.** It is told "a carrier is present" and
  handed frames. It never decides whether a packet got through, how far it
  reached, or who else heard it. A rule like "if two frames overlap, drop both"
  belongs to the simulator, and putting it here makes every host wrong at once.
- **The detection flags mean "a carrier is present *now*".** Not "a packet
  arrived". Raising them when a reception completes asserts a signal at the
  instant it stopped, which is what made repeaters believe the channel was
  permanently busy and relay nothing. Raise as the signal arrives, clear when it
  goes.
- **`RxDone` is latched; the detection flags are not.** A completed packet waits
  for the firmware to read it. A carrier does not wait for anything.
- **A frame handed over while the chip is not listening is lost, not queued.**
  Bounded by a short grace, because a real driver's re-arm is standby, configure,
  `SetRx`, and a frame landing in that gap is genuinely receivable. Beyond it,
  nothing that arrives that late was carried by a signal that has already ended.
- **Airtime is Semtech's formula, not an approximation.** MeshCore's CSMA is
  built on the figure the firmware itself computes. A second formula here that
  is nearly right desynchronises the two silently.
- **The chip lies only on purpose.** `vsx_set_stuck_irq_ms` exists so MeshCore
  1.17's stale-flag recovery can be exercised against a part that misbehaves the
  way real ones do. The default must never lie, or every node in a run believes
  the channel is busy.
- **Determinism is a feature.** Same calls in the same order, same answers. No
  wall-clock reads, no randomness that is not seeded by the host, no state that
  depends on how fast the caller ran.

## Tests

**Every bug fixed gets the test that would have caught it**, named after what
went wrong rather than after the function. The four cases in `test_model.cpp` are
each a bug that previously took a four minute emulated boot to see; that is the
standard, and it is why this repository exists separately at all.

Two binaries, both run by CI on every platform:

- `test_model.cpp`, C++, behaviour.
- `test_c_abi.c`, C, that the header is reachable from C and that every entry
  point survives a null handle.

No test framework, per the one rule. `check(condition, "what it means")` prints a
line and counts a failure, which is all a library this size needs.
