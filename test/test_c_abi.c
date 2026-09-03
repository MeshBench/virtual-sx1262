/* The header has to be valid C, not just C++.
 *
 * Two of the four hosts are not C++: QEMU's device is C, and Renode reaches the
 * library through a P/Invoke declaration written from this header by hand. A
 * header that only compiles under a C++ compiler passes every C++ test in this
 * repository and then fails in the one place it matters, so this file is
 * compiled by the C compiler on purpose and calls every entry point.
 *
 * It asserts almost nothing about behaviour. That is test_model.cpp's job. What
 * this proves is that the ABI is reachable from C: no C++ types leak through the
 * header, no default arguments, no references, and every symbol links.
 */
#include "virtual_sx1262.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int ok, const char* what) {
  printf("    %-4s %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) {
    ++failures;
  }
}

static int dio1_edges = 0;

static void on_dio1(void* user, int asserted) {
  (void)asserted;
  ++*(int*)user;
}

int main(void) {
  vsx_chip* chip;
  vsx_state state;
  vsx_counters counters;
  /* Big enough for the longest transaction below: SetDioIrqParams is 9. A
   * short reply buffer here overflowed the stack and the DIO1 callback
   * silently stopped arriving, which is how this was found. */
  uint8_t in[16];
  uint8_t frame[4];
  uint8_t tx[64];
  int major = -1, minor = -1;
  uint64_t now = 0;

  /* SetRx, so the chip is listening and a delivered frame has somewhere to go. */
  const uint8_t set_rx[4] = {0x82, 0xFF, 0xFF, 0xFF};
  /* SetDioIrqParams, everything unmasked onto DIO1. */
  const uint8_t set_dio[9] = {0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};

  printf("virtual-sx1262 C ABI\n\n  every entry point, from C\n");

  vsx_abi_version(&major, &minor);
  check(major >= 1, "vsx_abi_version");

  chip = vsx_create();
  check(chip != NULL, "vsx_create");

  vsx_set_dio1_callback(chip, on_dio1, &dio1_edges);
  check(1, "vsx_set_dio1_callback");

  memset(in, 0, sizeof(in));
  vsx_spi_transaction(chip, set_dio, in, sizeof(set_dio));
  vsx_spi_transaction(chip, set_rx, in, sizeof(set_rx));
  check(1, "vsx_spi_transaction");

  check(vsx_busy(chip) == 0, "vsx_busy");
  check(vsx_dio1_asserted(chip) == 0, "vsx_dio1_asserted");

  vsx_tick(chip, ++now);
  check(1, "vsx_tick");

  vsx_set_channel_busy(chip, 1);
  vsx_set_channel_busy(chip, 0);
  check(1, "vsx_set_channel_busy");

  frame[0] = 0x11;
  frame[1] = 0x00;
  frame[2] = 0xAB;
  frame[3] = 0xCD;
  vsx_deliver_frame(chip, frame, sizeof(frame));
  vsx_tick(chip, ++now);
  check(dio1_edges == 1, "vsx_deliver_frame reaches the callback through C");

  vsx_transmit_finished(chip);
  check(1, "vsx_transmit_finished");

  check(vsx_take_tx(chip, tx, sizeof(tx)) == 0, "vsx_take_tx with nothing to send");

  vsx_set_fem_enabled(chip, 1);
  vsx_set_last_signal(chip, -92.5f, -7.25f);
  check(1, "vsx_set_fem_enabled, vsx_set_last_signal");

  vsx_get_state(chip, &state);
  check(state.mode == 1, "vsx_get_state reports receive mode");
  /* The field appended in ABI 1.1. Checked from C because a struct that grew is
   * exactly where a host built against the old header and a library built
   * against the new one disagree, and two of the four hosts are not C++. */
  check(state.dio1_mask == 0xFFFF, "vsx_get_state reports the DIO1 mask");

  vsx_get_counters(chip, &counters);
  check(counters.frames_dropped == 0, "vsx_get_counters");

  check(vsx_est_airtime_ms(chip, 119) > 0, "vsx_est_airtime_ms");

  vsx_set_stuck_irq_ms(chip, 0);
  check(1, "vsx_set_stuck_irq_ms");

  /* Every call must tolerate a null handle, because a host that failed to
   * create one should get a quiet no-op rather than a crash inside the
   * library it just loaded. */
  vsx_set_dio1_callback(NULL, on_dio1, &dio1_edges);
  vsx_spi_transaction(NULL, set_rx, in, sizeof(set_rx));
  vsx_tick(NULL, 1);
  vsx_set_channel_busy(NULL, 1);
  vsx_deliver_frame(NULL, frame, sizeof(frame));
  vsx_transmit_finished(NULL);
  vsx_set_fem_enabled(NULL, 1);
  vsx_set_last_signal(NULL, 0.0f, 0.0f);
  vsx_get_state(NULL, &state);
  vsx_get_counters(NULL, &counters);
  check(vsx_busy(NULL) == 0, "a null handle is a no-op, not a crash");
  check(vsx_dio1_asserted(NULL) == 0, "vsx_dio1_asserted(NULL)");
  check(vsx_take_tx(NULL, tx, sizeof(tx)) == 0, "vsx_take_tx(NULL)");
  check(vsx_est_airtime_ms(NULL, 119) == 0, "vsx_est_airtime_ms(NULL)");

  /* A transaction with no reply buffer is legal: most commands are writes. */
  vsx_spi_transaction(chip, set_rx, NULL, sizeof(set_rx));
  check(1, "vsx_spi_transaction with no reply buffer");

  vsx_destroy(chip);
  vsx_destroy(NULL);
  check(1, "vsx_destroy, including of nothing");

  printf("\n%s: %d failure(s)\n\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
