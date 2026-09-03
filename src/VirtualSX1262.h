#pragma once

// A virtual SX1262: what answers RadioLib's SPI transactions.
//
// Everything above this line is the real software stack - MeshCore's
// Dispatcher, its CustomSX1262 driver, and RadioLib itself. This is where that
// stack meets the simulator, and it is deliberately the only place that knows
// both.
//
// The interesting part is not the command table, it is the IRQ flags. MeshCore
// decides whether the channel is busy by reading PREAMBLE_DETECTED and
// HEADER_VALID and timing how long they have been set - that is what 1.17's
// listen-before-talk rewrite changed, and it cannot be exercised by a shim that
// never sets the bits. So the flags are driven from the engine's view of what
// is on the air at this node, at the simulated instant each becomes true.
//
// What is modelled, and what is not:
//   * modelled: the command set RadioLib issues for a LoRa link, the IRQ
//     register, the data buffer, RSSI/SNR from the engine, CAD.
//   * not modelled: BUSY (see SimHal), calibration timing, the analogue front
//     end. A virtual chip is a model of a chip; the stack above it is real, the
//     silicon is not.

#include <stdint.h>
#include <string.h>

#include <deque>
#include <vector>

class VirtualSX1262 {
 public:
  VirtualSX1262();

  // ---- the simulator's side, called by the bridge ----

  // Frames the firmware has handed to the radio, waiting to go on the air.
  bool hasPendingTx = false;
  std::vector<uint8_t> pendingTx;

  // Frames the engine has delivered to this node.
  std::deque<std::vector<uint8_t>> inbox;

  // The engine says our waveform has left the antenna. The node cannot know
  // this: how long a transmission occupied the channel is a property of the
  // samples the engine generated.
  void transmitFinished();

  // Is another station on the air here, loud enough to detect? The engine is
  // the only thing that can answer, and this is where that answer becomes
  // something a driver can read.
  void setChannelBusy(bool busy);

  // What the engine measured for the last frame it delivered.
  void setLastSignal(float rssiDbm, float snrDb) {
    rssi_ = rssiDbm;
    snr_ = snrDb;
  }

  // ---- what the firmware has configured this radio to be ----
  //
  // The board profile says what the hardware can do; these say what the
  // firmware actually asked for, and the two diverge whenever the firmware has
  // a bug. MeshCore 1.17.1 fixed one of each kind - receive gain reverting
  // after an AGC reset, and a transmit-enable line that never went high - and
  // neither was visible from outside the chip because nothing here was read.
  //
  // Reported raw rather than interpreted. Deciding that a given gain register
  // value is worth 2 dB is the engine's business; the chip's business is to say
  // what the register holds.
  uint8_t rxGainReg() const { return regs_[kRegRxGain]; }
  int8_t txPowerDbm() const { return txPowerDbm_; }

  // The front-end module's transmit-enable line, driven by the firmware as an
  // ordinary GPIO rather than through the chip.
  //
  // Defaults to false because that is what a pin nobody has driven reads as. A
  // board with no FEM has no such pin, and the engine knows which boards those
  // are from the profile - answering true here to be kind would hide exactly
  // the fault this exists to catch.
  bool femEnabled() const { return femEnabled_; }
  void setFemEnabled(bool on) { femEnabled_ = on; }

  // Whether the module was switched in at the moment transmission started.
  //
  // This, not femEnabled(), is what decides how much power left the board. The
  // line is supposed to be low while the node listens - RadioLib drives it from
  // its RF-switch table and only raises it just before SetTx - so reading the
  // live level would dock a node for the ordinary state of receiving. What the
  // T096 fault broke was the line being high at the moment it mattered, and
  // that is a property of the transmission rather than of the node.
  // Until the node has transmitted once there is no answer, and "the module was
  // out" is the wrong one to invent: it would dock a node for not having spoken
  // yet. hasTransmitted separates the two.
  bool femAtTx() const { return femAtTx_; }
  bool hasTransmitted() const { return hasTransmitted_; }

  // The rest of what this radio has been configured to be. Nothing above the
  // chip can see any of it today, which is how a node can be set to the wrong
  // spreading factor, the wrong bandwidth or a gain mode its operator did not
  // choose, and look identical from outside to one that is right.
  //
  // Reported wholesale rather than field by field as each becomes interesting:
  // the expensive part is the wire format, and widening it once costs less than
  // widening it five times.
  uint8_t mode() const { return mode_; }  // 0 standby, 1 rx, 2 tx, 3 cad
  int sf() const { return sf_; }
  float bwKHz() const { return bwKHz_; }
  int cr() const { return cr_; }
  uint32_t preambleSyms() const { return preambleSyms_; }
  uint32_t freqHz() const { return freqHz_; }
  uint16_t irqMask() const { return irqMask_; }
  // What is wired out to the DIO1 pin, which is a narrower set than the above.
  uint16_t dio1Mask() const { return dio1Mask_; }
  uint16_t irqFlags() const { return irq_; }

  // Receive gain, at the address the datasheet gives it, with the two values
  // RadioLib writes. Power saving is the reset default and what this chip comes
  // up holding.
  //
  // Worth knowing why watching this register is enough. MeshCore does not use
  // RadioLib's own resetAGC(), which restores the mode from cached runtime
  // state; it has its own in helpers/radiolib/SX126xReset.h, which re-applies
  // the compile-time SX126X_RX_BOOSTED_GAIN macro and so discards whatever the
  // operator set at runtime. The firmware therefore writes the register itself
  // after every AGC reset - we do not have to model what calibration does to
  // silicon to see the fault, only to record what was written.
  // The random number registers. Noise on the part, and the only entropy a
  // firmware can get from a radio, so they are generated rather than stored.
  static constexpr uint16_t kRegRandomNumber = 0x0819;
  static constexpr uint16_t kRegRxGain = 0x08AC;
  static constexpr uint8_t kRxGainBoosted = 0x96;
  static constexpr uint8_t kRxGainPowerSaving = 0x94;

  // Advance internal timers to this simulated instant.
  void tick(uint64_t nowMs);

  // Seed this chip's receiver noise.
  //
  // Noise is where a radio's entropy comes from, and firmware helps itself to
  // it: RadioLib's randomByte() reads the instantaneous RSSI eight times and
  // keeps the low bit of each, and MeshCore seeds its PRNG from that and then
  // derives its identity from the PRNG. A chip that answers the same number
  // every time hands every node the same "random" keypair.
  //
  // Seeded by the host rather than sampled from the machine, because
  // determinism is a feature: the same run must produce the same noise. One
  // seed per node gives every node its own stream.
  void setNoiseSeed(uint64_t seed) { noiseSeed_ = seed; }

  // DIO1's level. Gated on the DIO1 mask, which is not the same thing as the
  // IRQ enable mask, and getting those two the wrong way round is what made an
  // emulated board relay about a third of the time.
  //
  // RadioLib's startReceive enables RxDone, Timeout, CrcErr, HeaderValid and
  // HeaderErr in the status register but routes only RxDone to DIO1. Gate the
  // pin on the enable mask instead and HeaderValid raises DIO1 twelve symbols
  // into the carrier, well before the frame is handed over. DIO1 is therefore
  // already high when RxDone arrives, so there is no rising edge - and RadioLib
  // attaches this pin with GpioInterruptRising. MeshCore's recvRaw is gated on
  // the flag that interrupt sets, so the packet decodes perfectly and is never
  // read out of the chip.
  bool irqAsserted() const { return (irq_ & dio1Mask_) != 0; }

  // Frames dropped because the chip was not listening when they were handed
  // over and did not start listening within the grace. A host that sees this
  // climbing is delivering into a deaf receiver.
  uint32_t framesDropped() const { return framesDropped_; }

  // ---- what the firmware's channel decisions look like from below ----
  //
  // MeshCore decides whether to defer by reading the IRQ register. Counting
  // those reads, and how long the busy flags were up, is the only way to tell
  // "the mesh is genuinely busy" from "our chip cries busy too readily" - and
  // the second is a fault in the simulator that would look exactly like a
  // finding about the firmware.
  uint32_t irqReads() const { return irqReads_; }
  uint32_t busyReads() const { return busyReads_; }
  uint32_t busyMs() const { return busyMs_; }
  uint32_t preambleRaises() const { return preambleRaises_; }
  uint32_t spuriousRaises() const { return spuriousRaises_; }

  // Latch a flag once raised, as a misbehaving chip does.
  //
  // This is the fault MeshCore 1.17 exists to survive: a preamble or header
  // flag that sets and never clears, so a driver that trusts it believes the
  // channel is busy for ever and stops transmitting. 1.16 trusts it; 1.17 times
  // it out. Without a way to reproduce the fault, the difference between them
  // cannot be observed at all - which is exactly what twelve runs showed.
  void setStuckIrqMs(uint32_t ms) { stuckIrqMs_ = ms; }

  // Airtime, from the parameters the firmware actually programmed.
  uint32_t estAirtimeMs(int lenBytes) const;

  int spreadingFactor() const { return sf_; }
  float bandwidthKHz() const { return bwKHz_; }
  int codingRate() const { return cr_; }

  // ---- RadioLib's side ----

  void spiTransfer(const uint8_t* out, size_t len, uint8_t* in);

  // The same chip, clocked a byte at a time.
  //
  // For an emulated MCU the SPI controller moves single bytes and the chip
  // select delimits a command, so there is no buffer to hand over. These three
  // rebuild one: the bytes are accumulated between the chip select falling and
  // rising, answered from the same command decoder, and acted on once at the
  // end - which is when a real chip acts on a command it has finished
  // receiving.
  void beginTransaction();
  uint8_t transferByte(uint8_t out);
  void endTransaction();

 private:
  void runCommand(const uint8_t* out, size_t len, uint8_t* in);
  void applyModulation(const uint8_t* p);
  void applyPacketParams(const uint8_t* p);
  void startRx();
  void startTx();
  void startCad();
  uint64_t inboxGraceMs() const;
  void settleInbox();
  void deliverPending();

  // Chip state.
  uint8_t buffer_[256] = {0};
  uint8_t rxLen_ = 0;
  uint16_t irq_ = 0;       // IRQ status register
  uint16_t irqMask_ = 0;   // what is recorded in the IRQ status register
  uint16_t dio1Mask_ = 0;  // which of those are wired out to the DIO1 pin
  uint8_t mode_ = 0;       // 0 standby, 1 rx, 2 tx, 3 cad
  uint8_t txBase_ = 0, rxBase_ = 0;
  // How many bytes the firmware said the next transmission is, from
  // SetPacketParams. The buffer is 256 bytes and only this many are on the air.
  uint8_t txLenForSend_ = 0;
  uint8_t regs_[0x1000] = {0};

  // Modem parameters, as programmed by the firmware.
  int sf_ = 10;
  float bwKHz_ = 250;
  int cr_ = 5;
  uint32_t preambleSyms_ = 16;
  uint32_t freqHz_ = 869525000;
  // What SetTxParams asked the PA for. Not the same as what leaves the
  // antenna, which is this plus the board's front end - and the front end only
  // contributes if the firmware remembered to switch it on.
  //
  // INT8_MIN until the firmware has said, because 0 dBm is a level a radio can
  // legitimately be set to and a node that has not configured itself yet must
  // not be read as one that chose silence.
  int8_t txPowerDbm_ = -128;
  bool femEnabled_ = false;
  bool femAtTx_ = false;
  bool hasTransmitted_ = false;

  // What the air is doing here, from the engine.
  bool channelBusy_ = false;
  uint64_t busySinceMs_ = 0;
  // The transaction in flight on the streaming path, and whether anything
  // arrived in it: a chip select that falls and rises with no bytes is not a
  // command and must not be applied.
  std::vector<uint8_t> txn_;
  bool inTxn_ = false;

  bool preambleRaised_ = false;
  bool inboxWasOccupied_ = false;
  uint64_t inboxSinceMs_ = 0;
  uint32_t framesDropped_ = 0;
  // The floor under that bound, for a frame so short its airtime is measured in
  // single milliseconds. Comfortably longer than a standby -> configure -> SetRx
  // re-arm and still nowhere near the seconds-scale staleness being bounded.
  static constexpr uint64_t kInboxGraceFloorMs = 50;
  bool headerRaised_ = false;

  float rssi_ = -100, snr_ = 0;
  // Receiver noise: a counter-based stream, so it is reproducible and has no
  // state shared with anything else. Zero until the host seeds it.
  uint64_t noiseSeed_ = 0;
  uint64_t noiseCounter_ = 0;
  // The noise for the transaction in progress. Held rather than drawn per read
  // because the byte-at-a-time path re-evaluates a growing prefix on every
  // byte, and that is only safe while evaluating changes nothing: a value drawn
  // fresh each time would advance six times down one path and once down the
  // other, and the same command would answer differently depending on how the
  // host frames it.
  uint32_t noiseNow_ = 0;
  void refreshNoise();
  uint8_t noiseBits(int bits) const { return (uint8_t)(noiseNow_ & ((1u << bits) - 1)); }
  uint64_t nowMs_ = 0;

  // Instrumentation.
  uint32_t irqReads_ = 0;
  uint32_t busyReads_ = 0;
  uint32_t busyMs_ = 0;
  uint32_t preambleRaises_ = 0;
  uint64_t lastBusyTickMs_ = 0;

  // Fault injection: how long a raised flag refuses to clear. 0 is a chip that
  // behaves.
  uint32_t stuckIrqMs_ = 0;
  uint64_t nextSpuriousMs_ = 0;
  uint32_t spuriousRaises_ = 0;
};
