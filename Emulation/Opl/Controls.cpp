#include "Controls.hpp"
#include <Emulation/Volume.hpp>
#include <algorithm>

namespace Emulation::Opl
{
namespace
{
struct RegisterField { unsigned char ADL_Operator::*member; int shift; int mask; };
constexpr RegisterField fields[OperatorFieldCount]{
    {&ADL_Operator::avekf_20, 0, 15}, {&ADL_Operator::ksl_l_40, 0, 63},
    {&ADL_Operator::atdec_60, 4, 15}, {&ADL_Operator::atdec_60, 0, 15},
    {&ADL_Operator::susrel_80, 4, 15}, {&ADL_Operator::susrel_80, 0, 15},
    {&ADL_Operator::waveform_E0, 0, 7}, {&ADL_Operator::ksl_l_40, 6, 3},
    {&ADL_Operator::avekf_20, 4, 1}, {&ADL_Operator::avekf_20, 5, 1},
    {&ADL_Operator::avekf_20, 6, 1}, {&ADL_Operator::avekf_20, 7, 1}};
void bits(ADL_UInt8& reg, int value, int mask, int shift = 0)
{ reg = (reg & ~(mask << shift)) | ((value & mask) << shift); }
}
int instrumentControlValue(const ADL_Instrument& i, int c) noexcept
{
  if(c >= OperatorBase && c < InstrumentControlEnd)
  {
    const int n = c - OperatorBase;
    const auto f = fields[n % OperatorFieldCount];
    return (i.operators[n / OperatorFieldCount].*f.member >> f.shift) & f.mask;
  }
  switch(c)
  {
    case VoiceMode: return i.inst_flags & 3;
    case Feedback1: return (i.fb_conn1_C0 >> 1) & 7;
    case Connection1: return i.fb_conn1_C0 & 1;
    case Feedback2: return (i.fb_conn2_C0 >> 1) & 7;
    case Connection2: return i.fb_conn2_C0 & 1;
    case PercussionKey: return i.percussion_key_number;
    case NoteOffset1: return i.note_offset1;
    case NoteOffset2: return i.note_offset2;
    case VelocityOffset: return i.midi_velocity_offset;
    case Detune: return i.second_voice_detune;
    case DelayOn: return i.delay_on_ms;
    case DelayOff: return i.delay_off_ms;
    case RhythmMode: return (i.inst_flags >> 3) & 7;
    case Blank: return (i.inst_flags >> 2) & 1;
    default: return 0;
  }
}
void setInstrumentControl(ADL_Instrument& i, int c, int v) noexcept
{
  if(c >= OperatorBase && c < InstrumentControlEnd)
  {
    const int n = c - OperatorBase;
    const auto f = fields[n % OperatorFieldCount];
    bits(i.operators[n / OperatorFieldCount].*f.member, v, f.mask, f.shift);
    return;
  }
  switch(c)
  {
    case VoiceMode: bits(i.inst_flags, v, 3); break;
    case Feedback1: bits(i.fb_conn1_C0, v, 7, 1); break;
    case Connection1: bits(i.fb_conn1_C0, v, 1); break;
    case Feedback2: bits(i.fb_conn2_C0, v, 7, 1); break;
    case Connection2: bits(i.fb_conn2_C0, v, 1); break;
    case PercussionKey: i.percussion_key_number = std::clamp(v, 0, 127); break;
    case NoteOffset1: i.note_offset1 = std::clamp(v, -128, 127); break;
    case NoteOffset2: i.note_offset2 = std::clamp(v, -128, 127); break;
    case VelocityOffset: i.midi_velocity_offset = std::clamp(v, -128, 127); break;
    case Detune: i.second_voice_detune = std::clamp(v, -128, 127); break;
    case DelayOn: i.delay_on_ms = std::clamp(v, 0, 65535); break;
    case DelayOff: i.delay_off_ms = std::clamp(v, 0, 65535); break;
    case RhythmMode: bits(i.inst_flags, v, 7, 3); break;
    case Blank: bits(i.inst_flags, v, 1, 2); break;
    default: break;
  }
}
Process::ControlInlet* makeVolumeControl(QObject* parent)
{
  return new Process::FloatSlider{0.f, MaxVolume, 1.f, QStringLiteral("Volume"),
      Id<Process::Port>{Volume + 1}, parent};
}
Process::ControlInlet* makeInstrumentIndexControl(QObject* parent)
{
  return new Process::IntSlider{0, 0, 0, QStringLiteral("Instrument"),
      Id<Process::Port>{InstrumentIndex + 1}, parent};
}
Process::ControlInlet* makeAllocationControl(QObject* parent)
{
  return new Process::ComboBox{
      {{QStringLiteral("Mono"), 0}, {QStringLiteral("Poly"), 1},
       {QStringLiteral("MIDI channel"), 2}}, 1, QStringLiteral("Voice allocation"),
      Id<Process::Port>{VoiceAllocation + 1}, parent};
}
std::vector<Process::ControlInlet*> makeControls(QObject* parent)
{
  std::vector<Process::ControlInlet*> v(ControlCount);
  const auto id = [](int c) { return Id<Process::Port>{c + 1}; };
  const auto integer = [&](int c, int low, int high, int init, QString name) {
    v[c] = new Process::IntSlider{low, high, init, name, id(c), parent};
  };
  const auto toggle = [&](int c, QString name) {
    v[c] = new Process::Toggle{false, name, id(c), parent};
  };
  v[ChipModel] = new Process::ComboBox{
      {{QStringLiteral("OPL2"), 0}, {QStringLiteral("OPL3"), 1}}, 1,
      QStringLiteral("OPL chip model"), id(ChipModel), parent};
  integer(Chips, 1, 16, 1, QStringLiteral("OPL chip count"));
  integer(FourOps, 0, 6, 0, QStringLiteral("OPL four-op channels"));
  toggle(DeepVibrato, QStringLiteral("Deep vibrato"));
  toggle(DeepTremolo, QStringLiteral("Deep tremolo"));
  toggle(SoftPan, QStringLiteral("Soft pan"));
  integer(BankMSB, 0, 127, 0, QStringLiteral("Bank MSB"));
  integer(BankLSB, 0, 255, 0, QStringLiteral("Bank LSB / drum kit"));
  toggle(Percussion, QStringLiteral("Drum bank"));
  integer(ProgramNumber, 0, 127, 0, QStringLiteral("Program / drum note"));
  v[File] = new Process::FileChooser{QString{}, QStringLiteral("OPL banks (*.wopl)"),
      QStringLiteral("Import WOPL"), id(File), parent};
  v[VoiceMode] = new Process::ComboBox{
      {{QStringLiteral("2 operators"), 0}, {QStringLiteral("4 operators"), 1},
       {QStringLiteral("Dual 2-op"), 3}}, 0, QStringLiteral("Voice mode"), id(VoiceMode), parent};
  integer(Feedback1, 0, 7, 0, QStringLiteral("Voice 1 feedback"));
  toggle(Connection1, QStringLiteral("Voice 1 additive connection"));
  integer(Feedback2, 0, 7, 0, QStringLiteral("Voice 2 feedback"));
  toggle(Connection2, QStringLiteral("Voice 2 additive connection"));
  integer(PercussionKey, 0, 127, 60, QStringLiteral("Drum pitch"));
  integer(NoteOffset1, -128, 127, 0, QStringLiteral("Voice 1 semitones"));
  integer(NoteOffset2, -128, 127, 0, QStringLiteral("Voice 2 semitones"));
  integer(VelocityOffset, -128, 127, 0, QStringLiteral("Velocity offset"));
  integer(Detune, -128, 127, 0, QStringLiteral("Voice 2 detune"));
  integer(DelayOn, 0, 65535, 0, QStringLiteral("Key-on lifetime (ms)"));
  integer(DelayOff, 0, 65535, 0, QStringLiteral("Key-off lifetime (ms)"));
  v[RhythmMode] = new Process::ComboBox{
      {{QStringLiteral("Normal"), 0}, {QStringLiteral("Bass drum"), 1},
       {QStringLiteral("Snare"), 2}, {QStringLiteral("Tom"), 3},
       {QStringLiteral("Cymbal"), 4}, {QStringLiteral("Hi-hat"), 5}}, 0,
      QStringLiteral("Rhythm voice"), id(RhythmMode), parent};
  toggle(Blank, QStringLiteral("Instrument blank"));
  const char* names[]{"Multiplier code", "Attenuation (0.75 dB steps)", "Attack rate", "Decay rate",
      "Sustain attenuation", "Release rate", "Waveform", "Key scale level",
      "Key scale rate", "Sustained envelope", "Vibrato", "Tremolo"};
  for(int op = 0; op < 4; ++op)
    for(int f = 0; f < OperatorFieldCount; ++f)
    {
      const int c = operatorControl(op, static_cast<OperatorField>(f));
      const QString name = QStringLiteral("Op %1 %2").arg(op + 1).arg(QString::fromLatin1(names[f]));
      if(fields[f].mask == 1) toggle(c, name);
      else integer(c, 0, fields[f].mask, 0, name);
    }
  v[Volume] = makeVolumeControl(parent);
  v[InstrumentIndex] = makeInstrumentIndexControl(parent);
  v[VoiceAllocation] = makeAllocationControl(parent);
  return v;
}
}
