#pragma once
#include <Emulation/Opl/Engine.hpp>
#include <Process/Dataflow/WidgetInlets.hpp>
#include <score_addon_emulation_export.h>
#include <vector>
#include <optional>

namespace Emulation::Opl
{
// Serialized inlet IDs are 1 + Control; MIDI input remains ID 0.
enum Control : int
{
  ChipModel = 0, Chips = 1, FourOps = 2, DeepVibrato = 3, DeepTremolo = 4,
  SoftPan = 5, BankMSB = 6, BankLSB = 7, Percussion = 8, ProgramNumber = 9,
  File = 10, VoiceMode = 11, Feedback1 = 12, Connection1 = 13,
  Feedback2 = 14, Connection2 = 15, PercussionKey = 16, NoteOffset1 = 17,
  NoteOffset2 = 18, VelocityOffset = 19, Detune = 20, DelayOn = 21,
  DelayOff = 22, RhythmMode = 23, Blank = 24, OperatorBase = 25,
  InstrumentControlEnd = 73, Volume = 73, InstrumentIndex = 74,
  VoiceAllocation = 75, ControlCount = 76
};
enum OperatorField : int
{
  Multiplier = 0, Level = 1, Attack = 2, Decay = 3, Sustain = 4, Release = 5,
  Waveform = 6, KeyScaleLevel = 7, KeyScaleRate = 8, EnvelopeSustain = 9,
  Vibrato = 10, Tremolo = 11, OperatorFieldCount = 12
};
constexpr int operatorControl(int op, OperatorField field) noexcept
{ return OperatorBase + op * OperatorFieldCount + field; }
constexpr bool isInstrumentControl(int c) noexcept
{ return c >= VoiceMode && c < InstrumentControlEnd; }
// Missing addresses remain reversible control values for ordinary SetControlValue
// undo. Nonnegative values are bank slots; negatives encode an unresolved address.
constexpr int unresolvedInstrument(Address address) noexcept
{
  return -1 - int((uint32_t(address.percussion) << 24) | (uint32_t(address.msb) << 16)
      | (uint32_t(address.lsb) << 8) | address.program);
}
inline std::optional<Address> instrumentAddress(const Bank* bank, int index) noexcept
{
  if(index >= 0)
  {
    if(bank && std::size_t(index) < bank->programs.size()) return bank->programs[index].address;
    return std::nullopt;
  }
  if(index < -0x2000000) return std::nullopt;
  const auto packed = uint32_t(-index - 1);
  return Address{bool(packed >> 24), uint8_t(packed >> 16), uint8_t(packed >> 8), uint8_t(packed)};
}
SCORE_ADDON_EMULATION_EXPORT int instrumentControlValue(const ADL_Instrument&, int control) noexcept;
void setInstrumentControl(ADL_Instrument&, int control, int value) noexcept;
Process::ControlInlet* makeVolumeControl(QObject* parent);
Process::ControlInlet* makeInstrumentIndexControl(QObject* parent);
Process::ControlInlet* makeAllocationControl(QObject* parent);
std::vector<Process::ControlInlet*> makeControls(QObject* parent);
}
