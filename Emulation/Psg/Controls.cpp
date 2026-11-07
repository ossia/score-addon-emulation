#include "Controls.hpp"
#include <Emulation/Volume.hpp>
namespace Emulation::Psg {
void updateAllocationControl(Process::ControlInlet& inlet)
{
  if(inlet.name() == QStringLiteral("Assignment")) inlet.setName(QStringLiteral("Voice allocation"));
  if(auto* combo = qobject_cast<Process::ComboBox*>(&inlet))
    combo->setAlternatives({{"Mono", 2}, {"Poly", 0}, {"MIDI channel", 1}});
  inlet.setDescription("Mono plays the three tones together with last-note priority. Poly allocates three tones. MIDI channel maps channels 1 to 3 to tones A to C. Sega noise remains on MIDI 10.");
}
std::vector<Process::ControlInlet*> makeControls(QObject* parent)
{
  std::vector<Process::ControlInlet*> v(ControlCount);
  const Settings defaults;
  const auto id = [](int c) { return Id<Process::Port>{c + 1}; };
  const auto integer = [&](int c, int low, int high, const char* name) {
    v[c] = new Process::IntSlider{low, high, int(controlValue(defaults, c)), QString::fromUtf8(name), id(c), parent};
  };
  const auto toggle = [&](int c, const char* name) {
    v[c] = new Process::Toggle{bool(controlValue(defaults, c)), QString::fromUtf8(name), id(c), parent};
  };
  v[ChipModel] = new Process::ComboBox{{{"AY-3-8910", 0}, {"YM2149", 1}, {"Sega YM7101", 2}}, 0, "Model", id(ChipModel), parent};
  integer(Clock, 100000, 4000000, "Clock Hz");
  v[VoiceAssignment] = new Process::ComboBox{{}, 0, "Voice allocation", id(VoiceAssignment), parent};
  updateAllocationControl(*v[VoiceAssignment]);
  v[Volume] = new Process::FloatSlider{0.f, MaxVolume, 1.f, "Volume", id(Volume), parent};
  integer(AyNoisePeriod, 0, 31, "AY noise period");
  integer(AyEnvelopePeriod, 0, 65535, "AY envelope period");
  integer(AyEnvelopeShape, 0, 15, "AY envelope shape");
  toggle(SegaNoiseEnabled, "Noise (MIDI 10)");
  v[SegaNoiseRate] = new Process::ComboBox{{{"Clock / 512", 0}, {"Clock / 1024", 1}, {"Clock / 2048", 2}, {"Tone C", 3}}, 0, "Noise rate", id(SegaNoiseRate), parent};
  toggle(SegaWhiteNoise, "White noise");
  integer(SegaNoiseLevel, 0, 15, "Noise level");
  v[SoftwareEnvelope] = new Process::ComboBox{
      {{"Native", 0}, {"Software ADSR", 1}}, 0, "Envelope source", id(SoftwareEnvelope), parent};
  integer(Attack, 0, 10000, "Attack ms"); integer(Decay, 0, 10000, "Decay ms");
  integer(Sustain, 0, 15, "Sustain"); integer(Release, 0, 10000, "Release ms");
  for(int ch = 0; ch < 3; ++ch) {
    toggle(channelControl(ch, Tone), "Tone"); toggle(channelControl(ch, Noise), "Noise");
    integer(channelControl(ch, Level), 0, 15, "Level"); toggle(channelControl(ch, HardwareEnvelope), "Envelope routing");
    for(auto field : {Tone, Noise, Level, HardwareEnvelope}) {
      auto* inlet = v[channelControl(ch, field)];
      inlet->setName(QStringLiteral("Tone %1 %2").arg(QChar('A' + ch)).arg(inlet->name()));
    }
  }
  return v;
}
}
