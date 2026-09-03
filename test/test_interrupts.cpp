/* DIO1: what raises it, when it is pushed, and the one case where the chip is
 * asked to misbehave the way real parts do.
 *
 * The pin is the whole reception path on an nRF52 - MeshCore reads a received
 * packet only from the interrupt it raises - so a chip that receives perfectly
 * and cannot say so is a node that forwards nothing.
 */
#include "harness.h"

int main() {
  std::printf("virtual-sx1262");

  /* ---------------------------------------------------------------- */
  CASE("only what is routed to DIO1 raises DIO1");
  /* The bug this repository was extracted to fix, and the reason every other
   * case here missed it: bring_up unmasks everything onto DIO1, which is more
   * generous than any real driver, so the model could confuse the two masks and
   * still pass.
   *
   * SetDioIrqParams carries IrqMask and then Dio1Mask. RadioLib's startReceive
   * enables RxDone, Timeout, CrcErr, HeaderValid and HeaderErr in the status
   * register but routes ONLY RxDone to the pin. Gate the pin on the enable mask
   * and HeaderValid raises DIO1 twelve symbols into the carrier, so DIO1 is
   * already high when RxDone lands, there is no rising edge, and a driver that
   * attached the pin with GpioInterruptRising - which RadioLib does - never
   * learns the packet exists. */
  {
    vsx_chip* c = vsx_create();
    uint64_t now = 0;
    const uint8_t mod[] = {0x8B, 8, 0x1A, 4, 0};
    vsx_spi_transaction(c, mod, nullptr, sizeof(mod));

    /* RadioLib's own two masks, not a convenient pair. 0x0272 is
     * RxDone|HeaderValid|HeaderErr|CrcErr|Timeout; 0x0002 is RxDone alone. */
    const uint8_t dio[] = {0x08, 0x02, 0x72, 0x00, 0x02, 0, 0, 0, 0};
    vsx_spi_transaction(c, dio, nullptr, sizeof(dio));
    const uint8_t rx[] = {0x82, 0xFF, 0xFF, 0xFF};
    vsx_spi_transaction(c, rx, nullptr, sizeof(rx));
    vsx_tick(c, ++now);

    vsx_state st;
    vsx_get_state(c, &st);
    check(st.irq_mask == 0x0272, "the enable mask is what the driver sent");
    check(st.dio1_mask == 0x0002, "and the DIO1 mask is the narrower one");

    /* A carrier arrives and is detected. */
    vsx_set_channel_busy(c, 1);
    for (int i = 0; i < 200; ++i) {
      vsx_tick(c, ++now);
    }
    check((irq_flags(c) & IRQ_HEADER) != 0,
          "HeaderValid is set, so the channel check can still read it");
    check(vsx_dio1_asserted(c) == 0, "but HeaderValid does not raise DIO1");

    /* The carrier ends and the frame is handed over. */
    vsx_set_channel_busy(c, 0);
    const uint8_t frame[] = {0x11, 0x00, 0xAB, 0xCD};
    vsx_deliver_frame(c, frame, sizeof(frame));
    vsx_tick(c, ++now);
    check((irq_flags(c) & IRQ_RX_DONE) != 0, "RxDone is set on delivery");
    check(vsx_dio1_asserted(c) == 1, "and RxDone does raise DIO1");
    vsx_destroy(c);
  }

  /* ---------------------------------------------------------------- */
  CASE("the chip raises DIO1 instead of waiting to be asked");
  /* The socket this ABI replaces was request-response, so hosts polled the
   * line on a 1 ms timer - a millisecond of latency on every packet, for a
   * pin a real part asserts. */
  {
    struct Seen {
      int edges = 0;
      int level = -1;
    } seen;
    vsx_chip* c = vsx_create();
    uint64_t now = 0;
    bring_up(c, &now);
    vsx_set_dio1_callback(
        c,
        [](void* u, int asserted) {
          auto* s = static_cast<Seen*>(u);
          ++s->edges;
          s->level = asserted;
        },
        &seen);

    check(seen.edges == 0, "a quiet chip raises nothing");

    const uint8_t frame[] = {0x11, 0x00, 0x01, 0x02};
    vsx_deliver_frame(c, frame, sizeof(frame));
    check(seen.edges == 0, "handing over a frame alone does not assert it");

    vsx_tick(c, ++now);
    check(seen.edges == 1 && seen.level == 1, "delivering the packet asserts DIO1, once");

    vsx_tick(c, ++now);
    vsx_tick(c, ++now);
    check(seen.edges == 1, "and it does not re-fire while the level holds");

    /* ClearIrqStatus of everything - the driver acknowledging. */
    const uint8_t clr[] = {0x02, 0xFF, 0xFF};
    vsx_spi_transaction(c, clr, nullptr, sizeof(clr));
    check(seen.edges == 2 && seen.level == 0, "acknowledging it drops the line, once");
    vsx_destroy(c);
  }

  /* ---------------------------------------------------------------- */
  CASE("the chip only lies when it is asked to");
  /* Latching a detection flag past the signal is what real SX1262s do
   * sometimes, and what MeshCore 1.17's recovery exists to survive. It is a
   * deliberate variant, not the default - a normal chip must not lie, or
   * every repeater in a run believes the channel is busy for ever. */
  {
    vsx_chip* c = vsx_create();
    uint64_t now = 0;
    bring_up(c, &now);
    vsx_set_stuck_irq_ms(c, 50);

    for (int i = 0; i < 200; ++i) {
      vsx_tick(c, ++now);
    }
    vsx_counters k;
    vsx_get_counters(c, &k);
    check(k.spurious_raises > 0,
          "the stuck variant raises a flag with nothing on the air");
    vsx_destroy(c);

    c = vsx_create();
    now = 0;
    bring_up(c, &now);
    for (int i = 0; i < 200; ++i) {
      vsx_tick(c, ++now);
    }
    vsx_get_counters(c, &k);
    check(k.spurious_raises == 0, "the default chip does not");
    check((irq_flags(c) & (IRQ_PREAMBLE | IRQ_HEADER)) == 0,
          "and reports a quiet channel when the air is quiet");
    vsx_destroy(c);
  }

  return report("");
}
