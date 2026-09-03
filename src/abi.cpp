/* The C ABI over VirtualSX1262.
 *
 * The model stays a plain C++ class that knows nothing about its hosts. Two
 * things live here rather than in it:
 *
 *   The DIO1 edge. The model exposes the level (irqAsserted); turning that into
 *   "the line just changed, tell somebody" is a host concern, and doing it here
 *   means every host gets a pushed interrupt without the model growing a
 *   callback it would have to invoke at the right moments.
 *
 *   The handle. Hosts get an opaque pointer, so the C++ layout is not part of
 *   the ABI and the model can be rearranged without breaking a DllImport.
 */
#include "virtual_sx1262.h"

#include "VirtualSX1262.h"

#include <cstring>
#include <new>

#define VSX_ABI_MAJOR 1
#define VSX_ABI_MINOR 0

struct vsx_chip {
  VirtualSX1262 chip;
  vsx_dio1_fn dio1_fn = nullptr;
  void* dio1_user = nullptr;
  bool dio1_last = false;
};

namespace {

/* Every entry point that can move an IRQ bit ends here. The callback fires only
 * on a change, so a host can wire it straight to a level-driven interrupt line
 * without filtering, and a chip nobody is listening to costs one comparison. */
void settle_dio1(vsx_chip* c) {
  const bool now = c->chip.irqAsserted();
  if (now == c->dio1_last) {
    return;
  }
  c->dio1_last = now;
  if (c->dio1_fn) {
    c->dio1_fn(c->dio1_user, now ? 1 : 0);
  }
}

}  // namespace

extern "C" {

vsx_chip* vsx_create(void) {
  return new (std::nothrow) vsx_chip();
}

void vsx_destroy(vsx_chip* chip) {
  delete chip;
}

void vsx_set_dio1_callback(vsx_chip* chip, vsx_dio1_fn fn, void* user) {
  if (!chip) {
    return;
  }
  chip->dio1_fn = fn;
  chip->dio1_user = user;
  /* Report the line as it stands, so a host that registers late is not left
   * waiting for an edge that already happened. */
  if (fn && chip->dio1_last) {
    fn(user, 1);
  }
}

void vsx_spi_transaction(vsx_chip* chip, const uint8_t* out, uint8_t* in, size_t len) {
  if (!chip || !out || len == 0) {
    return;
  }
  uint8_t scratch[260];
  uint8_t* dst = in;
  if (!dst) {
    dst = scratch;
    if (len > sizeof(scratch)) {
      len = sizeof(scratch);
    }
  }
  chip->chip.spiTransfer(out, len, dst);
  settle_dio1(chip);
}

int vsx_busy(const vsx_chip* chip) {
  (void)chip;
  /* Held low, which is what the native path does too: the model does not
   * represent the time a real part spends digesting a command, and answering
   * differently here would give an emulated node a different radio from a
   * native one. */
  return 0;
}

int vsx_dio1_asserted(const vsx_chip* chip) {
  return chip && chip->chip.irqAsserted() ? 1 : 0;
}

void vsx_tick(vsx_chip* chip, uint64_t now_ms) {
  if (!chip) {
    return;
  }
  chip->chip.tick(now_ms);
  settle_dio1(chip);
}

void vsx_set_channel_busy(vsx_chip* chip, int busy) {
  if (!chip) {
    return;
  }
  chip->chip.setChannelBusy(busy != 0);
  settle_dio1(chip);
}

void vsx_deliver_frame(vsx_chip* chip, const uint8_t* frame, size_t len) {
  if (!chip || !frame || len == 0) {
    return;
  }
  chip->chip.inbox.emplace_back(frame, frame + len);
  /* No settle: nothing is asserted until a tick delivers it, and delivering
   * on arrival would let a packet in while the chip was transmitting. */
}

void vsx_transmit_finished(vsx_chip* chip) {
  if (!chip) {
    return;
  }
  chip->chip.transmitFinished();
  settle_dio1(chip);
}

size_t vsx_take_tx(vsx_chip* chip, uint8_t* dst, size_t cap) {
  if (!chip || !chip->chip.hasPendingTx) {
    return 0;
  }
  const size_t n = chip->chip.pendingTx.size();
  if (dst && cap) {
    std::memcpy(dst, chip->chip.pendingTx.data(), n < cap ? n : cap);
  }
  chip->chip.hasPendingTx = false;
  return n;
}

void vsx_set_fem_enabled(vsx_chip* chip, int enabled) {
  if (chip) {
    chip->chip.setFemEnabled(enabled != 0);
  }
}

void vsx_set_last_signal(vsx_chip* chip, float rssi_dbm, float snr_db) {
  if (chip) {
    chip->chip.setLastSignal(rssi_dbm, snr_db);
  }
}

void vsx_get_state(const vsx_chip* chip, vsx_state* out) {
  if (!chip || !out) {
    return;
  }
  const VirtualSX1262& c = chip->chip;
  std::memset(out, 0, sizeof(*out));
  out->freq_hz = c.freqHz();
  out->bandwidth_hz = (uint32_t)(c.bwKHz() * 1000.0f + 0.5f);
  out->preamble_syms = (uint16_t)c.preambleSyms();
  out->irq_mask = c.irqMask();
  out->irq_flags = c.irqFlags();
  out->spreading_factor = (uint8_t)c.sf();
  out->coding_rate = (uint8_t)c.cr();
  out->mode = c.mode();
  out->tx_power_dbm = c.txPowerDbm();
  out->rx_gain_reg = c.rxGainReg();
  /* Three states, because "has not transmitted" is not "transmitted with the
   * module out". */
  out->fem_at_tx = !c.hasTransmitted() ? 0 : (c.femAtTx() ? 2 : 1);
}

void vsx_get_counters(const vsx_chip* chip, vsx_counters* out) {
  if (!chip || !out) {
    return;
  }
  const VirtualSX1262& c = chip->chip;
  out->irq_reads = c.irqReads();
  out->busy_reads = c.busyReads();
  out->busy_ms = c.busyMs();
  out->spurious_raises = c.spuriousRaises();
  out->preamble_raises = c.preambleRaises();
  out->frames_dropped = c.framesDropped();
}

uint32_t vsx_est_airtime_ms(const vsx_chip* chip, int len_bytes) {
  return chip ? chip->chip.estAirtimeMs(len_bytes) : 0;
}

void vsx_set_stuck_irq_ms(vsx_chip* chip, uint32_t ms) {
  if (chip) {
    chip->chip.setStuckIrqMs(ms);
  }
}

void vsx_abi_version(int* major, int* minor) {
  if (major) {
    *major = VSX_ABI_MAJOR;
  }
  if (minor) {
    *minor = VSX_ABI_MINOR;
  }
}

}  // extern "C"
