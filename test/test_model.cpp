/* The model's own tests, through the C ABI its hosts use.
 *
 * Every case here is a bug that reached a four-minute emulator run before this
 * library had a bench of its own. The names say what went wrong, not what the
 * function is called.
 */
#include "virtual_sx1262.h"

/* The class as well as the ABI: the byte-at-a-time path below is how the
 * emulators drive this chip and it is not reachable through the C surface. */
#include "VirtualSX1262.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;
static const char* g_case = "";

#define CASE(name)                 \
  do {                             \
    g_case = name;                 \
    std::printf("\n  %s\n", name); \
  } while (0)

static void check(bool ok, const char* what) {
  std::printf("    %-4s %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) {
    ++g_failures;
  }
}

/* SX1262 IRQ bits, as the datasheet numbers them. */
enum {
  IRQ_TX_DONE = 1u << 0,
  IRQ_RX_DONE = 1u << 1,
  IRQ_PREAMBLE = 1u << 2,
  IRQ_SYNC_WORD = 1u << 3,
  IRQ_HEADER = 1u << 4,
};

/* Put the chip in a state a driver would recognise: LoRa at SF8/62.5 kHz, every
 * IRQ unmasked onto DIO1, and receiving.
 *
 * Deliberately more generous than RadioLib, which routes only RxDone to the
 * pin, so the cases below can talk about one flag at a time without restating a
 * mask each time. That generosity is also how a confusion between the two masks
 * in SetDioIrqParams passed every test here for months, so the case named "only
 * what is routed to DIO1 raises DIO1" sends the real pair instead. */
static void bring_up(vsx_chip* c, uint64_t* now) {
  /* SetModulationParams: sf, bw, cr, ldro. 0x1A is the 62.5 kHz code. */
  const uint8_t mod[] = {0x8B, 8, 0x1A, 4, 0};
  vsx_spi_transaction(c, mod, nullptr, sizeof(mod));

  /* SetDioIrqParams: mask, dio1, dio2, dio3 - unmask everything onto DIO1. */
  const uint8_t dio[] = {0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
  vsx_spi_transaction(c, dio, nullptr, sizeof(dio));

  /* SetRx, timeout infinite. */
  const uint8_t rx[] = {0x82, 0xFF, 0xFF, 0xFF};
  vsx_spi_transaction(c, rx, nullptr, sizeof(rx));

  vsx_tick(c, ++*now);
}

static uint16_t irq_flags(vsx_chip* c) {
  vsx_state s;
  vsx_get_state(c, &s);
  return s.irq_flags;
}

static uint8_t mode_of(vsx_chip* c) {
  vsx_state s;
  vsx_get_state(c, &s);
  return s.mode;
}

static void set_standby(vsx_chip* c) {
  const uint8_t sb[] = {0x80, 0};
  vsx_spi_transaction(c, sb, nullptr, sizeof(sb));
}

int main() {
  std::printf("virtual-sx1262");

  /* ---------------------------------------------------------------- */
  CASE("a carrier that has ended is not still 'present'");
  /* The bug: deliverPending raised PreambleDetected|HeaderValid|SyncWord at
   * RxDone - asserting that a signal was arriving at the instant it stopped.
   * MeshCore's CustomSX1262::isReceiving reads HEADER_VALID as "the channel is
   * busy", so a repeater with a packet to forward never saw a clear channel
   * and relayed nothing. */
  {
    vsx_chip* c = vsx_create();
    uint64_t now = 0;
    bring_up(c, &now);

    /* A transmission arrives and runs for a while, then ends and is
     * delivered - which is the order the simulator uses. */
    vsx_set_channel_busy(c, 1);
    for (int i = 0; i < 200; ++i) {
      vsx_tick(c, ++now);
    }
    check((irq_flags(c) & IRQ_HEADER) != 0,
          "while the carrier is on the air, HeaderValid is set");

    const uint8_t frame[] = {0x11, 0x00, 0x33, 0x0a, 0xc2, 0x1f};
    vsx_deliver_frame(c, frame, sizeof(frame));
    vsx_set_channel_busy(c, 0);
    vsx_tick(c, ++now);

    const uint16_t f = irq_flags(c);
    check((f & IRQ_RX_DONE) != 0, "the delivered packet raises RxDone");
    check((f & IRQ_HEADER) == 0, "HeaderValid is clear once the carrier has gone");
    check((f & IRQ_PREAMBLE) == 0, "PreambleDetected is clear too");
    check((f & IRQ_SYNC_WORD) == 0, "SyncWordValid is clear too");
    vsx_destroy(c);
  }

  /* ---------------------------------------------------------------- */
  CASE("a packet handed to a deaf chip does not turn up later");
  /* The bug: a frame delivered while the chip was not listening sat in the
   * inbox until it started listening - however long that took - so a packet
   * could arrive seconds after the air that carried it went quiet. */
  {
    vsx_chip* c = vsx_create();
    uint64_t now = 0;
    bring_up(c, &now);

    set_standby(c);
    vsx_tick(c, ++now);
    check(mode_of(c) == 0, "the chip is in standby, not listening");

    const uint8_t frame[] = {0x11, 0x00, 0xde, 0xad};
    vsx_deliver_frame(c, frame, sizeof(frame));

    /* Far longer than any re-arm, well short of the seconds this used to
     * survive. */
    for (int i = 0; i < 500; ++i) {
      vsx_tick(c, ++now);
    }

    /* Now it starts listening again. The stale frame must not appear. */
    const uint8_t rx[] = {0x82, 0xFF, 0xFF, 0xFF};
    vsx_spi_transaction(c, rx, nullptr, sizeof(rx));
    for (int i = 0; i < 10; ++i) {
      vsx_tick(c, ++now);
    }

    vsx_counters k;
    vsx_get_counters(c, &k);
    check((irq_flags(c) & IRQ_RX_DONE) == 0,
          "the stale packet is not delivered when the chip returns to receive");
    check(k.frames_dropped == 1, "it is counted as dropped, not silently lost");
    vsx_destroy(c);
  }

  /* ---------------------------------------------------------------- */
  CASE("a brief re-arm gap still receives");
  /* The other half of that fix: the driver's own startReceive is
   * standby -> configure -> SetRx, so mode is briefly 0 in a perfectly
   * healthy receiver. A frame landing in that gap is receivable, and an
   * immediate drop would have lost it. */
  {
    vsx_chip* c = vsx_create();
    uint64_t now = 0;
    bring_up(c, &now);

    set_standby(c);
    const uint8_t frame[] = {0x11, 0x00, 0xbe, 0xef};
    vsx_deliver_frame(c, frame, sizeof(frame));

    vsx_tick(c, ++now); /* one millisecond of re-arm */
    const uint8_t rx[] = {0x82, 0xFF, 0xFF, 0xFF};
    vsx_spi_transaction(c, rx, nullptr, sizeof(rx));
    vsx_tick(c, ++now);

    vsx_counters k;
    vsx_get_counters(c, &k);
    check((irq_flags(c) & IRQ_RX_DONE) != 0,
          "a frame that arrived during the gap is delivered");
    check(k.frames_dropped == 0, "and nothing was dropped");
    vsx_destroy(c);
  }

  /* ---------------------------------------------------------------- */
  CASE("the wait a frame gets is the time its signal was on the air");
  /* The bound on that wait started life as a fixed 10 ms and that was the wrong
   * shape: the hosts advance this clock at their own rate, so a constant that
   * is generous against one is tight against another and drops a frame that was
   * genuinely receivable. It is derived from the frame's own airtime now. This
   * pins that, because a regression to a small constant passes both tests above
   * and quietly loses packets in the host that matters. */
  {
    vsx_chip* c = vsx_create();
    uint64_t now = 0;
    bring_up(c, &now);

    /* A full-length advert: hundreds of milliseconds on the air at SF8. */
    uint8_t frame[119];
    std::memset(frame, 0xA5, sizeof(frame));
    const uint32_t air = vsx_est_airtime_ms(c, (int)sizeof(frame));
    check(air > 100, "a 119-byte frame at SF8 is hundreds of ms on the air");

    set_standby(c);
    vsx_deliver_frame(c, frame, sizeof(frame));

    /* Well past any fixed small constant, still inside the airtime. */
    for (uint32_t i = 0; i < air / 2; ++i) {
      vsx_tick(c, ++now);
    }
    vsx_counters k;
    vsx_get_counters(c, &k);
    check(k.frames_dropped == 0, "half an airtime into a re-arm it is still held");

    const uint8_t rx[] = {0x82, 0xFF, 0xFF, 0xFF};
    vsx_spi_transaction(c, rx, nullptr, sizeof(rx));
    vsx_tick(c, ++now);
    check((irq_flags(c) & IRQ_RX_DONE) != 0,
          "and it is delivered when listening resumes");
    vsx_destroy(c);
  }

  /* ---------------------------------------------------------------- */
  CASE("arming the receiver delivers, without waiting to be ticked");
  /* Delivery used to happen only on a tick, which made every reception depend
   * on a tick landing after the firmware armed the receiver. A host that ticks
   * in its own process nearly always got one; a host whose ticks arrive from a
   * simulator over a socket did not, and the frame went in the bin. Same model,
   * same calls, different answer depending on the plumbing. No tick here on
   * purpose: the SetRx alone has to be enough. */
  {
    vsx_chip* c = vsx_create();
    uint64_t now = 0;
    bring_up(c, &now);

    set_standby(c);
    const uint8_t frame[] = {0x11, 0x00, 0x5A, 0x5A};
    vsx_deliver_frame(c, frame, sizeof(frame));
    vsx_tick(c, ++now); /* the engine's last tick, while the chip is still deaf */
    check((irq_flags(c) & IRQ_RX_DONE) == 0, "nothing arrives while it is deaf");

    const uint8_t rx[] = {0x82, 0xFF, 0xFF, 0xFF};
    vsx_spi_transaction(c, rx, nullptr, sizeof(rx));
    check((irq_flags(c) & IRQ_RX_DONE) != 0, "the SetRx alone hands the frame over");
    check(vsx_dio1_asserted(c) == 1, "and DIO1 is up in that same transaction");
    vsx_destroy(c);
  }

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
  CASE("the receiver has noise, and two chips do not share it");
  /* What this is really testing is an identity. RadioLib's randomByte() reads
   * GetRssiInst eight times and keeps the low bit of each; MeshCore seeds its
   * PRNG with that and derives its keypair from the PRNG. This model returned
   * -rssi*2, which is always even, so the bit was always 0, the seed was always
   * 0, and every emulated board came up with the same identity. Worse, both
   * Arduino cores treat a zero seed as "ignore me", so nothing even looked
   * wrong until two boards were put side by side. */
  {
    auto sample_byte = [](vsx_chip* c) {
      /* Exactly what RadioLib's randomByte() does: ReadRegister of
       * REG_RANDOM_NUMBER_0 (0x0819), eight times, keeping the low bit of each.
       * [op][addr hi][addr lo][nop][value]. */
      uint8_t b = 0;
      for (int i = 0; i < 8; ++i) {
        const uint8_t cmd[] = {0x1D, 0x08, 0x19, 0x00, 0x00};
        uint8_t in[5] = {0, 0, 0, 0, 0};
        vsx_spi_transaction(c, cmd, in, sizeof(cmd));
        b = (uint8_t)((b << 1) | (in[4] & 1));
      }
      return b;
    };

    vsx_chip* a = vsx_create();
    uint64_t now = 0;
    bring_up(a, &now);
    vsx_set_noise_seed(a, 1);
    uint8_t first = sample_byte(a);
    uint8_t second = sample_byte(a);
    check(!(first == 0x00 && second == 0x00), "the low bit is not always zero");
    check(first != second, "successive reads differ, so eight of them are eight bits");

    vsx_chip* b = vsx_create();
    uint64_t bnow = 0;
    bring_up(b, &bnow);
    vsx_set_noise_seed(b, 2);
    check(sample_byte(b) != first, "a chip with another seed reads differently");

    /* And still reproducible: same seed, same stream. Determinism is the reason
     * this is seeded by the host rather than sampled from the machine. */
    vsx_chip* again = vsx_create();
    uint64_t anow = 0;
    bring_up(again, &anow);
    vsx_set_noise_seed(again, 1);
    check(sample_byte(again) == first, "the same seed gives the same noise");

    /* The dither must not move the reading itself more than a receiver's own
     * noise would: this register is read for signal strength as well. */
    vsx_state st;
    vsx_get_state(a, &st);
    const uint8_t cmd[] = {0x15, 0x00, 0x00};
    uint8_t in[3] = {0, 0, 0};
    vsx_spi_transaction(a, cmd, in, sizeof(cmd));
    const int reported = -(int)in[2] / 2;
    check(reported < -95 && reported > -105, "and it still reads about -100 dBm");

    vsx_destroy(a);
    vsx_destroy(b);
    vsx_destroy(again);
  }

  /* ---------------------------------------------------------------- */
  CASE("both ways of framing a transaction give the same answer");
  /* There are two ways into this chip and they must not disagree.
   *
   * A host with a whole buffer calls spiTransfer. A host with a real chip
   * select shifts bytes one at a time, and that path consults returnsData() to
   * decide whether a command answers with anything. An opcode missing from that
   * list reads as zeroes down the byte path and correctly down the buffer path,
   * so the same chip behaves differently depending on how the caller is
   * plumbed - and both emulators use the byte path, so it is the one that
   * matters and the one nothing was checking.
   *
   * GetRssiInst was missing. Every emulated board read its receiver as 0 dBm:
   * no noise floor, no signal strength, and no entropy in the register RadioLib
   * samples to seed the firmware's random number generator. Rather than assert
   * that one opcode, walk every reader. */
  {
    const uint8_t readers[] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x17, 0x1D, 0x1E, 0xC0};
    for (uint8_t op : readers) {
      VirtualSX1262 chip;
      chip.setNoiseSeed(7);
      chip.setLastSignal(-73.5f, 4.25f);

      /* Six bytes is past the data of every command in the list. */
      uint8_t cmd[6] = {op, 0, 0, 0, 0, 0};
      uint8_t whole[6] = {0, 0, 0, 0, 0, 0};
      chip.spiTransfer(cmd, sizeof(cmd), whole);

      VirtualSX1262 twin;
      twin.setNoiseSeed(7);
      twin.setLastSignal(-73.5f, 4.25f);
      uint8_t shifted[6] = {0, 0, 0, 0, 0, 0};
      twin.beginTransaction();
      for (size_t i = 0; i < sizeof(cmd); ++i) {
        shifted[i] = twin.transferByte(cmd[i]);
      }
      twin.endTransaction();

      char what[96];
      std::snprintf(what, sizeof(what),
                    "opcode 0x%02X answers the same shifted as it does whole", op);
      check(std::memcmp(whole, shifted, sizeof(whole)) == 0, what);
    }
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

  /* ---------------------------------------------------------------- */
  CASE("airtime is the chip's answer, not the host's");
  /* MeshCore's listen-before-talk is built on the firmware's own
   * getEstAirtimeFor, so a host must be able to ask rather than reimplement
   * Semtech's formula beside it and drift. */
  {
    vsx_chip* c = vsx_create();
    uint64_t now = 0;
    bring_up(c, &now);
    const uint32_t a = vsx_est_airtime_ms(c, 119);
    const uint32_t b = vsx_est_airtime_ms(c, 238);
    check(a > 0, "a 119-byte frame has an airtime");
    check(b > a, "a longer frame takes longer");
    vsx_destroy(c);
  }

  /* ---------------------------------------------------------------- */
  CASE("a transmitted frame comes back out once");
  {
    vsx_chip* c = vsx_create();
    uint64_t now = 0;
    bring_up(c, &now);

    const uint8_t payload[] = {0x11, 0x00, 0xaa, 0xbb, 0xcc};
    uint8_t wr[3 + sizeof(payload)] = {0x0E, 0x00, 0};
    std::memcpy(wr + 2, payload, sizeof(payload));
    vsx_spi_transaction(c, wr, nullptr, 2 + sizeof(payload));

    const uint8_t pp[] = {0x8C, 0x00, 0x08, 0x00, (uint8_t)sizeof(payload),
                          0x01, 0x00, 0x00, 0x00};
    vsx_spi_transaction(c, pp, nullptr, sizeof(pp));
    const uint8_t tx[] = {0x83, 0, 0, 0};
    vsx_spi_transaction(c, tx, nullptr, sizeof(tx));

    uint8_t got[64] = {0};
    const size_t n = vsx_take_tx(c, got, sizeof(got));
    check(n == sizeof(payload), "the frame the firmware wrote is the frame that leaves");
    check(std::memcmp(got, payload, sizeof(payload)) == 0, "and its bytes are unchanged");
    check(vsx_take_tx(c, got, sizeof(got)) == 0, "it is not handed out twice");
    vsx_destroy(c);
  }

  /* ---------------------------------------------------------------- */
  CASE("the ABI says which one it is");
  {
    int major = -1, minor = -1;
    vsx_abi_version(&major, &minor);
    check(major == 1 && minor >= 0, "vsx_abi_version reports a version");
  }

  std::printf("\n%s: %d failure(s)\n\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
