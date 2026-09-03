/* The radio: what the part does between commands.
 *
 * Modes, the clock, carrier detection, reception, transmission and Semtech's
 * airtime formula. Nothing here knows how a command reached it; that is spi.cpp
 * next door. The split is not cosmetic - the wire protocol is a transcription
 * of a table and the radio is a model of a physical thing, and the two get
 * changed for different reasons.
 */
#include "VirtualSX1262.h"

#include "registers.h"

#include <vector>

#include <math.h>

VirtualSX1262::VirtualSX1262() {
  // The chip has to introduce itself. RadioLib reads sixteen bytes at
  // REG_VERSION_STRING and compares the first six against the chip type; a chip
  // that answers zeroes is retried ten times and then reported as not present,
  // which is exactly how this first failed to boot.
  //
  // The expected string for an SX1262 is "SX1261". That is not a mistake here
  // or in RadioLib: the part genuinely reports its family that way, and a
  // virtual chip that answered "SX1262" would be the one telling the lie.
  static const char kVersion[] = "SX1261";
  memcpy(&regs_[0x0320], kVersion, 6);

  // Receiver gain, which MeshCore reads back to report whether boosted mode is
  // on. Power-on default is the non-boosted value.
  regs_[0x08AC] = 0x94;

#ifdef VIRTUAL_SX1262_STUCK_IRQ_MS
  // A deliberately misbehaving chip, built as its own firmware variant.
  //
  // Real SX1262s sometimes latch the detection flags and refuse to clear them -
  // the "4 second lock-up" MeshCore's release notes describe. A driver that
  // trusts those flags then believes the channel is busy for ever and stops
  // transmitting; 1.17 exists to time them out and recover. On a chip that
  // behaves, 1.16 and 1.17 are indistinguishable, which is exactly what a
  // twelve-run sweep found. This variant makes the fault reproducible, so the
  // difference between the two versions can be measured rather than assumed.
  stuckIrqMs_ = VIRTUAL_SX1262_STUCK_IRQ_MS;
#endif
}

void VirtualSX1262::tick(uint64_t nowMs) {
  // Time with the busy flags up, which is what the firmware is reacting to.
  if ((irq_ & (kIrqPreambleDetected | kIrqHeaderValid)) && nowMs > lastBusyTickMs_) {
    busyMs_ += (uint32_t)(nowMs - lastBusyTickMs_);
  }
  lastBusyTickMs_ = nowMs;
  nowMs_ = nowMs;

  // When the queue went from empty to occupied, so the grace below can be
  // measured. The hosts push straight into `inbox`, so this is where a new
  // arrival is first seen.
  if (inbox.empty()) {
    inboxSinceMs_ = nowMs_;
  } else if (!inboxWasOccupied_) {
    inboxSinceMs_ = nowMs_;
  }
  inboxWasOccupied_ = !inbox.empty();

  // The fault, on builds compiled to have it: a detection interrupt that fires
  // with nothing on the air and then stays up.
  //
  // Clearing it works perfectly well - which is the whole point. A driver that
  // clears the flag when it has been set implausibly long recovers; one that
  // trusts it believes the channel is busy for ever and stops transmitting.
  // That is the difference between MeshCore 1.16 and 1.17, and it cannot be
  // seen on a chip that never lies.
  if (stuckIrqMs_ > 0 && mode_ == 1 && nowMs_ >= nextSpuriousMs_) {
    irq_ |= kIrqPreambleDetected;
    spuriousRaises_++;
    nextSpuriousMs_ = nowMs_ + stuckIrqMs_;
  }
  // A frame the engine handed over while the chip was not listening used to sit
  // in the inbox until the chip returned to receive, however long that took - so
  // a packet could arrive seconds after the air that carried it went quiet. The
  // engine has already ruled on half duplex before it delivers anything (it
  // withholds what a node transmitted over), so this is the chip's own
  // deafness, and it needs a bound rather than an open queue.
  //
  // The bound is a short grace rather than an immediate drop on purpose. The
  // driver's own re-arm is standby -> configure -> SetRx, so mode_ is briefly 0
  // in the middle of a perfectly healthy receiver, and a frame landing in that
  // gap is receivable. Beyond the grace it is not: nothing that arrives that
  // late was carried by the signal that has already ended.
  settleInbox();

  // Preamble and header detection, in receive mode only. A node cannot hear
  // anything while its own transmitter is keyed - that is half duplex, and the
  // engine already reports the channel as clear to a node that is transmitting.
  if (mode_ != 1) return;

  const double symbolMs = (double)(1u << sf_) / (bwKHz_ > 0 ? bwKHz_ : 250.0);
  if (channelBusy_) {
    const double sinceMs = (double)(nowMs_ - busySinceMs_);
    if (!preambleRaised_ && sinceMs >= kPreambleSymbols * symbolMs) {
      irq_ |= kIrqPreambleDetected;
      preambleRaised_ = true;
      preambleRaises_++;
    }
    if (!headerRaised_ && sinceMs >= kHeaderSymbols * symbolMs) {
      irq_ |= kIrqHeaderValid | kIrqSyncWordValid;
      headerRaised_ = true;
    }
  }
}

void VirtualSX1262::setChannelBusy(bool busy) {
  if (busy && !channelBusy_) busySinceMs_ = nowMs_;
  if (!busy) {
    // The air went quiet: the signal that raised the preamble/header detection
    // flags is gone, so a well-behaved chip's detection state clears with it.
    // RxDone stays - that is a completed packet the firmware still has to read -
    // but the "a carrier is present" flags must not outlive the carrier, or the
    // driver's own channel-activity check (CustomSX1262::isReceiving reads
    // HEADER_VALID) reports the air busy for the ~4 s it takes that check to time
    // the stale flag out. On a mesh whose adverts arrive every few seconds that
    // window never closes, and a repeater that has a packet to forward never
    // sees a clear channel to send it on - which is why an emulated board
    // received every advert and relayed none.
    //
    // The deliberately-misbehaving STUCK_IRQ variant is exempt: leaving its
    // flags latched past the signal is the whole point of it, because that is
    // the real "4 second lock-up" MeshCore 1.17's driver exists to recover from.
    preambleRaised_ = false;
    headerRaised_ = false;
    if (stuckIrqMs_ == 0) {
      irq_ &= (uint16_t)~(kIrqPreambleDetected | kIrqHeaderValid | kIrqSyncWordValid);
    }
  }
  channelBusy_ = busy;
}

void VirtualSX1262::transmitFinished() {
  if (mode_ == 2) {
    irq_ |= kIrqTxDone;
    mode_ = 0;
  }
}

// How long a frame the chip could not take may wait for it to start listening.
//
// Derived from the signal rather than picked: the packet occupied the air for
// its airtime, so a receiver that starts listening inside that window was
// plausibly listening for part of the transmission and can be given it. Past
// it the carrier is definitively gone, and anything delivered later would be a
// packet arriving after the air that carried it went quiet.
//
// A fixed number of milliseconds was the first attempt and it was the wrong
// shape. The hosts advance this clock at their own rate - an emulated board
// runs on its own time, not the simulator's - so a constant that looks generous
// against one host is tight against another, and a frame that was genuinely
// receivable gets dropped.
uint64_t VirtualSX1262::inboxGraceMs() const {
  const uint64_t air = inbox.empty() ? 0 : estAirtimeMs((int)inbox.front().size());
  return air > kInboxGraceFloorMs ? air : kInboxGraceFloorMs;
}

// Whether the frame at the front of the inbox goes to the firmware or goes in
// the bin. Both of its callers ask the same question, so it is asked once here:
// tick(), as the clock moves, and startRx(), as the firmware arms the receiver.
//
// Only the deaf case has a deadline. Once the chip is listening the frame is
// handed over however long it waited, because the wait was the chip's own
// deafness and the grace exists to cover exactly that.
void VirtualSX1262::settleInbox() {
  if (inbox.empty()) return;
  if (mode_ == 1) {
    deliverPending();
    return;
  }
  if (nowMs_ - inboxSinceMs_ > inboxGraceMs()) {
    inbox.clear();
    framesDropped_++;
  }
}

void VirtualSX1262::deliverPending() {
  if (inbox.empty()) return;
  auto& f = inbox.front();
  rxLen_ = (uint8_t)(f.size() > 255 ? 255 : f.size());
  memcpy(&buffer_[rxBase_], f.data(), rxLen_);
  inbox.pop_front();
  irq_ |= kIrqRxDone;
  // Deliberately only RxDone. The detection flags say "a carrier is present",
  // and by the time a frame is delivered the carrier has ended - raising them
  // here asserted the presence of a signal at the moment it stopped, which is
  // backwards. tick() raises them as the signal arrives, which is when they are
  // true, and setChannelBusy(false) clears them when it goes. Nothing needs
  // them at delivery: RadioLib reads the payload on RxDone alone, and the
  // driver's channel check reads them to ask "is something arriving now".
}

void VirtualSX1262::startRx() {
  mode_ = 1;
  // Arming the receiver is itself a delivery point, not just a change of mode.
  //
  // Delivery used to happen only on a tick, which quietly made every reception
  // depend on a tick landing after the firmware armed the receiver. A host that
  // ticks the model in its own process gets one almost immediately; a host whose
  // ticks arrive from a simulator over a socket may not, and the frame sat in
  // the inbox until the grace ran out and then went in the bin. Same model, same
  // seed, different answer depending on how the caller was plumbed, which is not
  // a property a chip is allowed to have.
  settleInbox();
}

void VirtualSX1262::startTx() {
  mode_ = 2;
  // Latched here because this is the instant it decides anything. RadioLib
  // raises the RF switch into transmit before issuing SetTx, so by now the line
  // carries the answer to "does this transmission reach the antenna".
  femAtTx_ = femEnabled_;
  hasTransmitted_ = true;
  pendingTx.assign(&buffer_[txBase_], &buffer_[txBase_] + txLenForSend_);
  hasPendingTx = true;
}

void VirtualSX1262::startCad() {
  mode_ = 3;
  // CAD answers in one go: the chip listens for a couple of symbols and reports.
  irq_ |= kIrqCadDone;
  if (channelBusy_) irq_ |= kIrqCadDetected;
  mode_ = 0;
}

uint32_t VirtualSX1262::estAirtimeMs(int lenBytes) const {
  // Semtech's own airtime formula, from the parameters the firmware programmed.
  const double bwHz = bwKHz_ * 1000.0;
  const double tSym = (double)(1u << sf_) / bwHz;
  const int de = (sf_ >= 11) ? 1 : 0;
  const int crc = 1, header = 0, crDen = cr_;
  double num = 8.0 * lenBytes - 4.0 * sf_ + 28 + 16 * crc - 20 * header;
  double den = 4.0 * (sf_ - 2 * de);
  double payloadSyms = 8 + fmax(ceil(num / den) * crDen, 0.0);
  double t = (preambleSyms_ + 4.25 + payloadSyms) * tSym;
  return (uint32_t)(t * 1000.0 + 0.5);
}

void VirtualSX1262::applyModulation(const uint8_t* p) {
  sf_ = p[0];
  // Bandwidth is an index in the datasheet's table; only the values MeshCore
  // uses are mapped, and anything else keeps the current setting rather than
  // silently becoming zero and making airtime infinite.
  switch (p[1]) {
    case 0x00:
      bwKHz_ = 7.81f;
      break;
    case 0x08:
      bwKHz_ = 10.42f;
      break;
    case 0x01:
      bwKHz_ = 15.63f;
      break;
    case 0x09:
      bwKHz_ = 20.83f;
      break;
    case 0x02:
      bwKHz_ = 31.25f;
      break;
    case 0x0A:
      bwKHz_ = 41.67f;
      break;
    case 0x03:
      bwKHz_ = 62.5f;
      break;
    case 0x04:
      bwKHz_ = 125.0f;
      break;
    case 0x05:
      bwKHz_ = 250.0f;
      break;
    case 0x06:
      bwKHz_ = 500.0f;
      break;
    default:
      break;
  }
  cr_ = p[2] ? (4 + p[2]) : cr_;
}

void VirtualSX1262::applyPacketParams(const uint8_t* p) {
  preambleSyms_ = ((uint32_t)p[0] << 8) | p[1];
  txLenForSend_ = p[3];
}
