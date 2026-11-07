// SPDX-License-Identifier: GPL-3.0-or-later
#include "Controls.hpp"
#include <Emulation/Volume.hpp>
namespace Emulation::Sid
{
void updateControlMetadata(Process::ControlInlet& inlet, int c)
{
  const auto rename = [&](const QString& previous, const QString& current) {
    // Migrate stock serialized names without discarding user-authored port names.
    if(inlet.name() == previous) inlet.setName(current);
  };
  switch(c)
  {
    case ChipModel: rename(QStringLiteral("Chip"), QStringLiteral("Chip model")); break;
    case VoiceMode:
      rename(QStringLiteral("Allocation"), QStringLiteral("Voice allocation"));
      if(auto* combo = qobject_cast<Process::ComboBox*>(&inlet))
        combo->setAlternatives({{"Mono", 0}, {"Poly", 1}, {"MIDI channel", 2}});
      inlet.setDescription("Mono plays all three oscillators with last-note priority. Poly allocates three voices. MIDI channel maps channels 1 to 3 to oscillators 1 to 3. The filter and chip volume remain shared.");
      break;
    case Cutoff: rename(QStringLiteral("Cutoff"), QStringLiteral("Filter cutoff register")); break;
    case Resonance: rename(QStringLiteral("Resonance"), QStringLiteral("Filter resonance")); break;
    case FilterCurve: rename(QStringLiteral("Filter curve"), QStringLiteral("Filter calibration")); break;
    case CombinedStrength: rename(QStringLiteral("Combined waves"), QStringLiteral("Combined waveform strength")); break;
    default: break;
  }
  if(c < OscillatorBase) return;
  const int oscillator = (c - OscillatorBase) / OscillatorFieldCount;
  const auto field = OscillatorField((c - OscillatorBase) % OscillatorFieldCount);
  const auto name = [oscillator](const char* text) {
    return QStringLiteral("Osc %1 %2").arg(oscillator + 1).arg(QString::fromLatin1(text));
  };
  switch(field)
  {
    case Attack: rename(name("attack"), name("attack rate")); break;
    case Decay: rename(name("decay"), name("decay rate")); break;
    case Sustain: rename(name("sustain"), name("sustain level")); break;
    case Release: rename(name("release"), name("release rate")); break;
    case FilterRoute: rename(name("filter"), name("filter routing")); break;
    default: break;
  }
  if(!inlet.description().isEmpty()) return;
  if(field == Attack || field == Decay || field == Release)
    inlet.setDescription(QStringLiteral("SID envelope rate code: 0 fastest, 15 slowest; not milliseconds."));
  else if(field == Sustain)
    inlet.setDescription(QStringLiteral("SID sustain level: 0 silent, 15 full level."));
  else if(field == Sync || field == Ring)
    inlet.setDescription(QStringLiteral("Osc %1 to Osc %2%3").arg((oscillator + 2) % 3 + 1)
        .arg(oscillator + 1).arg(field == Ring ? QStringLiteral(" (triangle ring modulation)") : QStringLiteral(" (hard sync)")));
}
std::vector<Process::ControlInlet*> makeControls(QObject* parent)
{
  std::vector<Process::ControlInlet*> v(ControlCount);
  const auto p = defaultPatch();
  const auto id = [](int c) { return Id<Process::Port>{c + 1}; };
  const auto integer = [&](int c, int lo, int hi, QString name) {
    v[c] = new Process::IntSlider{lo, hi, int(controlValue(p, c)), name, id(c), parent};
  };
  const auto toggle = [&](int c, QString name) {
    v[c] = new Process::Toggle{bool(controlValue(p, c)), name, id(c), parent};
  };
  v[ChipModel] = new Process::ComboBox{{{QStringLiteral("MOS 6581"), 0}, {QStringLiteral("CSG 8580"), 1}}, 0,
      QStringLiteral("Chip"), id(ChipModel), parent};
  v[ClockType] = new Process::ComboBox{{{QStringLiteral("PAL"), 0}, {QStringLiteral("NTSC"), 1}}, 0,
      QStringLiteral("Clock"), id(ClockType), parent};
  v[VoiceMode] = new Process::ComboBox{{}, 0, QStringLiteral("Voice allocation"), id(VoiceMode), parent};
  updateControlMetadata(*v[VoiceMode], VoiceMode);
  integer(FilterCurve, 0, 100, QStringLiteral("Filter curve"));
  v[Volume] = new Process::FloatSlider{0.f, MaxVolume, 1.f, QStringLiteral("Volume"), id(Volume), parent};
  integer(Cutoff, 0, 2047, QStringLiteral("Cutoff"));
  integer(Resonance, 0, 15, QStringLiteral("Resonance"));
  v[FilterMode] = new Process::ComboBox{{{QStringLiteral("Off"), 0}, {QStringLiteral("Low pass"), 1},
      {QStringLiteral("Band pass"), 2}, {QStringLiteral("LP + BP"), 3}, {QStringLiteral("High pass"), 4},
      {QStringLiteral("Notch"), 5}, {QStringLiteral("BP + HP"), 6}, {QStringLiteral("LP + BP + HP"), 7}}, 1,
      QStringLiteral("Filter mode"), id(FilterMode), parent};
  integer(ChipVolume, 0, 15, QStringLiteral("Chip volume"));
  toggle(Voice3Off, QStringLiteral("Voice 3 direct off"));
  v[CombinedStrength] = new Process::ComboBox{{{QStringLiteral("Average"), 0}, {QStringLiteral("Weak"), 1},
      {QStringLiteral("Strong"), 2}}, 0, QStringLiteral("Combined waves"), id(CombinedStrength), parent};
  for(int o = 0; o < 3; ++o)
  {
    const auto c = [o](OscillatorField f) { return oscillatorControl(o, f); };
    const auto name = [o](const char* s) { return QStringLiteral("Osc %1 %2").arg(o + 1).arg(QString::fromLatin1(s)); };
    // Every hardware waveform combination is selectable, including noise combinations.
    v[c(Waveform)] = new Process::ComboBox{{{QStringLiteral("Off"), 0}, {QStringLiteral("Triangle"), 1},
        {QStringLiteral("Saw"), 2}, {QStringLiteral("Tri + saw"), 3}, {QStringLiteral("Pulse"), 4},
        {QStringLiteral("Tri + pulse"), 5}, {QStringLiteral("Saw + pulse"), 6}, {QStringLiteral("Tri + saw + pulse"), 7},
        {QStringLiteral("Noise"), 8}, {QStringLiteral("Noise + tri"), 9}, {QStringLiteral("Noise + saw"), 10},
        {QStringLiteral("Noise + tri + saw"), 11}, {QStringLiteral("Noise + pulse"), 12},
        {QStringLiteral("Noise + tri + pulse"), 13}, {QStringLiteral("Noise + saw + pulse"), 14},
        {QStringLiteral("All"), 15}}, 2, name("wave"), id(c(Waveform)), parent};
    integer(c(PulseWidth), 0, 4095, name("pulse width"));
    toggle(c(Sync), name("sync")); toggle(c(Ring), name("ring"));
    integer(c(Attack), 0, 15, name("attack")); integer(c(Decay), 0, 15, name("decay"));
    integer(c(Sustain), 0, 15, name("sustain")); integer(c(Release), 0, 15, name("release"));
    integer(c(Semitones), -48, 48, name("semitones")); integer(c(Cents), -100, 100, name("cents"));
    toggle(c(FilterRoute), name("filter"));
  }
  return v;
}
}
