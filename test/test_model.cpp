/* The model's own tests, through the C ABI its hosts use.
 *
 * Every case here is a bug that reached a four-minute emulator run before this
 * library had a bench of its own. The names say what went wrong, not what the
 * function is called.
 */
#include "virtual_sx1262.h"

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
 * IRQ unmasked onto DIO1, and receiving. Mirrors what RadioLib's startReceive
 * leaves behind. */
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
