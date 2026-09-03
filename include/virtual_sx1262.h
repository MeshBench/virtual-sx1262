/* virtual-sx1262 - a virtual Semtech SX1262, as a C ABI.
 *
 * The model itself is C++ and knows nothing of its hosts. This is the surface
 * every host links against instead of talking to a server over a socket:
 *
 *   the native MeshCore build   links the C++ class directly
 *   QEMU's sx1262 device        links this, extern "C"
 *   Renode's C# peripheral      DllImports this
 *   MeshBench                   cgo, where it wants the model in Go tests
 *
 * Two deliberate shapes, both learned from the socket this replaces:
 *
 *   Transactions, not bytes. vsx_spi_transaction takes a whole chip-select
 *   framed exchange. A call per byte across a managed-code boundary costs more
 *   than the socket it was meant to remove, and the chip has to know where one
 *   command ends anyway - there is no length in the wire protocol.
 *
 *   The chip raises DIO1; nobody asks it. The socket was request-response, so
 *   the model had no way to call back and its hosts polled the line on a timer
 *   instead - up to a millisecond of latency on every received packet, for a
 *   pin a real part asserts. Register a callback and it is pushed.
 *
 * Not thread-safe. A host that drives SPI and the clock from different threads
 * serialises them itself, which is what the radioserver's lock did.
 */
#ifndef VIRTUAL_SX1262_H
#define VIRTUAL_SX1262_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vsx_chip vsx_chip;

/* DIO1 changed level. Called from inside whichever vsx_ call caused it, on that
 * caller's thread - never from a thread of the model's own - so a host may
 * drive its own interrupt line straight from here. */
typedef void (*vsx_dio1_fn)(void* user, int asserted);

/* ---- lifecycle ---- */
vsx_chip* vsx_create(void);
void vsx_destroy(vsx_chip* chip);

/* NULL fn goes back to polling with vsx_dio1_asserted. */
void vsx_set_dio1_callback(vsx_chip* chip, vsx_dio1_fn fn, void* user);

/* ---- the firmware side ---- */

/* One chip-select framed SPI exchange. `out` is MOSI, `in` receives MISO, both
 * `len` bytes. `in` may be NULL for a command whose reply is not wanted. */
void vsx_spi_transaction(vsx_chip* chip, const uint8_t* out, uint8_t* in, size_t len);

/* BUSY, which RadioLib spins on. */
int vsx_busy(const vsx_chip* chip);

/* DIO1 now, for a host that would rather ask than be told. */
int vsx_dio1_asserted(const vsx_chip* chip);

/* ---- the simulator side ---- */

/* Advance the chip's clock. Monotonic, in milliseconds of simulated time. */
void vsx_tick(vsx_chip* chip, uint64_t now_ms);

/* Whether another station is on the air here. Drives preamble and header
 * detection, and the answer CAD gives. */
void vsx_set_channel_busy(vsx_chip* chip, int busy);

/* Hand the chip a received frame. It is delivered to the firmware on a tick
 * where the chip is listening; if it is not listening it is dropped, because a
 * packet cannot arrive after the signal that carried it has gone. */
void vsx_deliver_frame(vsx_chip* chip, const uint8_t* frame, size_t len);

/* The waveform the chip started has finished on the air. The chip cannot know
 * this: how long a transmission occupied the channel is a property of the
 * samples the simulator generated. */
void vsx_transmit_finished(vsx_chip* chip);

/* Take a frame the firmware transmitted, if there is one. Returns its length,
 * or 0 when there is nothing to send. Copies at most `cap` bytes; a return
 * greater than `cap` means the frame was truncated. */
size_t vsx_take_tx(vsx_chip* chip, uint8_t* dst, size_t cap);

/* The front-end module's enable line, which decides how much power leaves the
 * board. */
void vsx_set_fem_enabled(vsx_chip* chip, int enabled);

/* What the last reception measured, for the registers the firmware reads back. */
void vsx_set_last_signal(vsx_chip* chip, float rssi_dbm, float snr_db);

/* ---- what the chip has been programmed to, and what it has seen ---- */

typedef struct {
  uint32_t freq_hz;
  uint32_t bandwidth_hz;
  uint16_t preamble_syms;
  uint16_t irq_mask;
  uint16_t irq_flags;
  uint8_t spreading_factor;
  uint8_t coding_rate;
  uint8_t mode; /* 0 standby, 1 rx, 2 tx, 3 cad */
  int8_t tx_power_dbm;
  uint8_t rx_gain_reg;
  uint8_t fem_at_tx; /* 0 never transmitted, 1 module out, 2 module in */
} vsx_state;

void vsx_get_state(const vsx_chip* chip, vsx_state* out);

typedef struct {
  uint32_t irq_reads;
  uint32_t busy_reads;
  uint32_t busy_ms;
  uint32_t spurious_raises;
  uint32_t preamble_raises;
  uint32_t frames_dropped; /* handed over while deaf, past the grace */
} vsx_counters;

void vsx_get_counters(const vsx_chip* chip, vsx_counters* out);

/* MeshCore's listen-before-talk is built on the airtime the firmware itself
 * computes, so a host that needs the figure asks the chip rather than
 * reimplementing Semtech's formula beside it. */
uint32_t vsx_est_airtime_ms(const vsx_chip* chip, int len_bytes);

/* Make the chip misbehave the way real SX1262s do: latch a detection flag and
 * refuse to drop it. 0 disables. This is what MeshCore 1.17's stale-flag
 * recovery exists to survive, and the only place the model lies on purpose. */
void vsx_set_stuck_irq_ms(vsx_chip* chip, uint32_t ms);

/* Semantic version of this ABI, for a host that loads the library at runtime. */
void vsx_abi_version(int* major, int* minor);

#ifdef __cplusplus
}
#endif
#endif /* VIRTUAL_SX1262_H */
