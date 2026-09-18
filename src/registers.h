/* The SX1262's own tables, transcribed.
 *
 * Separate from both the radio behaviour and the wire protocol because it is
 * neither: it is the datasheet, and the two implementation files below each
 * need about half of it. Kept complete rather than trimmed to what is used
 * today, because a register map with holes in it is harder to check against
 * the part than a full one, and the next opcode this model learns is already
 * named here. That is what [[maybe_unused]] says: these are documentation as
 * much as code, so an unused one is not dead code to be deleted.
 *
 * Source: Semtech SX1261/2 datasheet rev 2.1, tables 11-1 (opcodes) and 13-29
 * (IRQ flags).
 */
#ifndef VIRTUAL_SX1262_REGISTERS_H
#define VIRTUAL_SX1262_REGISTERS_H

#include <stdint.h>

namespace {
[[maybe_unused]] constexpr uint8_t kSetSleep = 0x84;
[[maybe_unused]] constexpr uint8_t kSetStandby = 0x80;
[[maybe_unused]] constexpr uint8_t kSetTx = 0x83;
[[maybe_unused]] constexpr uint8_t kSetRx = 0x82;
[[maybe_unused]] constexpr uint8_t kSetCad = 0xC5;
[[maybe_unused]] constexpr uint8_t kSetRfFrequency = 0x86;
[[maybe_unused]] constexpr uint8_t kSetPacketType = 0x8A;
[[maybe_unused]] constexpr uint8_t kGetPacketType = 0x11;
[[maybe_unused]] constexpr uint8_t kSetTxParams = 0x8E;
[[maybe_unused]] constexpr uint8_t kSetModulationParams = 0x8B;
[[maybe_unused]] constexpr uint8_t kSetPacketParams = 0x8C;
[[maybe_unused]] constexpr uint8_t kSetCadParams = 0x88;
[[maybe_unused]] constexpr uint8_t kCalibrate = 0x89;
[[maybe_unused]] constexpr uint8_t kSetBufferBase = 0x8F;
[[maybe_unused]] constexpr uint8_t kWriteBuffer = 0x0E;
[[maybe_unused]] constexpr uint8_t kReadBuffer = 0x1E;
[[maybe_unused]] constexpr uint8_t kWriteRegister = 0x0D;
[[maybe_unused]] constexpr uint8_t kReadRegister = 0x1D;
[[maybe_unused]] constexpr uint8_t kSetDioIrqParams = 0x08;
[[maybe_unused]] constexpr uint8_t kGetIrqStatus = 0x12;
[[maybe_unused]] constexpr uint8_t kClearIrqStatus = 0x02;
[[maybe_unused]] constexpr uint8_t kGetRxBufferStatus = 0x13;
[[maybe_unused]] constexpr uint8_t kGetPacketStatus = 0x14;
[[maybe_unused]] constexpr uint8_t kGetStatus = 0xC0;
[[maybe_unused]] constexpr uint8_t kGetDeviceErrors = 0x17;
[[maybe_unused]] constexpr uint8_t kClearDeviceErrors = 0x07;
[[maybe_unused]] constexpr uint8_t kGetRssiInst = 0x15;

// IRQ bits.
[[maybe_unused]] constexpr uint16_t kIrqTxDone = 1 << 0;
[[maybe_unused]] constexpr uint16_t kIrqRxDone = 1 << 1;
[[maybe_unused]] constexpr uint16_t kIrqPreambleDetected = 1 << 2;
[[maybe_unused]] constexpr uint16_t kIrqSyncWordValid = 1 << 3;
[[maybe_unused]] constexpr uint16_t kIrqHeaderValid = 1 << 4;
[[maybe_unused]] constexpr uint16_t kIrqHeaderErr = 1 << 5;
[[maybe_unused]] constexpr uint16_t kIrqCrcErr = 1 << 6;
[[maybe_unused]] constexpr uint16_t kIrqCadDone = 1 << 7;
[[maybe_unused]] constexpr uint16_t kIrqCadDetected = 1 << 8;
[[maybe_unused]] constexpr uint16_t kIrqTimeout = 1 << 9;

// The LoRa sync word, which lives in two registers rather than a command.
//
// RadioLib writes it as a pair through WriteRegister, so nothing in the opcode
// table names it and a model that only watches commands never sees it change.
// The reset value is the private network word, 0x1424, which is what the part
// comes up holding and what a firmware that never sets one is using.
[[maybe_unused]] constexpr uint16_t kRegSyncWordMsb = 0x0740;
[[maybe_unused]] constexpr uint16_t kRegSyncWordLsb = 0x0741;
[[maybe_unused]] constexpr uint16_t kSyncWordPrivate = 0x1424;
[[maybe_unused]] constexpr uint16_t kSyncWordPublic = 0x3444;

// What SetModulationParams is allowed to say. Outside these the firmware has
// programmed something the part does not define, which is a fault to report
// rather than a value to adopt.
[[maybe_unused]] constexpr uint8_t kSfMin = 5;
[[maybe_unused]] constexpr uint8_t kSfMax = 12;
[[maybe_unused]] constexpr uint8_t kCrMin = 1;
[[maybe_unused]] constexpr uint8_t kCrMax = 4;

// The symbol duration past which the part wants low data rate optimisation, in
// milliseconds. It is a property of the symbol and not of the spreading factor:
// SF11 crosses it at 125 kHz and does not at 250 kHz, so a rule written in SF
// alone is wrong at every bandwidth but one.
[[maybe_unused]] constexpr double kLowDataRateSymbolMs = 16.0;

// Untested.
// // CAD exit modes, from SetCadParams. CAD_ONLY drops back to standby whatever it
// // found; CAD_RX goes straight on into receive when it found something, which is
// // the mode a driver uses to avoid re-arming between the scan and the packet it
// // scanned for.
// [[maybe_unused]] constexpr uint8_t kCadExitOnly = 0x00;
// [[maybe_unused]] constexpr uint8_t kCadExitRx = 0x01;

// The symbol counts SetCadParams selects between, as 1 << cadSymbolNum: the
// register holds 0 to 4 and the scan lasts 1, 2, 4, 8 or 16 symbols. Anything
// above 4 is not a longer scan, it is a value the part does not define, so it is
// clamped rather than shifted with.
[[maybe_unused]] constexpr uint8_t kCadSymbolNumMax = 4;

// The timeout register's unit, shared by SetRx, SetTx and SetCadParams: one
// count is 15.625 us, which is 1/64 of a millisecond.
[[maybe_unused]] constexpr double kTimeoutStepMs = 1.0 / 64.0;

// A 24-bit timeout of all ones is not a very long timeout, it is continuous
// receive, and a driver that means "listen until I say otherwise" writes this.
[[maybe_unused]] constexpr uint32_t kRxContinuous = 0xFFFFFF;

// How far into a transmission a receiver locks onto the preamble, and how much
// later the header is demodulated. Both are in symbols and become milliseconds
// through the current modem settings, because that is what makes them behave
// like a radio rather than like a constant: at SF12 a preamble takes an age and
// at SF7 it is gone in a blink, and MeshCore's listen-before-talk times exactly
// that.
[[maybe_unused]] constexpr double kPreambleSymbols = 4.0;
[[maybe_unused]] constexpr double kHeaderSymbols = 12.0;
}  // namespace

#endif /* VIRTUAL_SX1262_REGISTERS_H */
