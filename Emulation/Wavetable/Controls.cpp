#include "Controls.hpp"
#include <Emulation/Volume.hpp>
#include <Emulation/WaveformInlet.hpp>
namespace Emulation::Wavetable {
void updateAllocationControl(Process::ControlInlet& inlet) {
  if(inlet.name() == QStringLiteral("Assignment")) inlet.setName(QStringLiteral("Voice allocation"));
  if(auto* combo = qobject_cast<Process::ComboBox*>(&inlet))
    combo->setAlternatives({{"Mono", 2}, {"Poly", 0}, {"MIDI channel", 1}});
  inlet.setDescription("Mono plays enabled voices together with last-note priority. Poly allocates enabled voices. MIDI channel maps 1 to 5 on SCC/SCC+ or 1 to 6 on HuC6280; disabled voices and the HuC LFO source are unavailable.");
}
std::vector<Process::ControlInlet*> makeControls(QObject* parent) {
  std::vector<Process::ControlInlet*> v(ControlCount); const Settings defaults;
  const auto id = [](int c) { return Id<Process::Port>{portId(c)}; };
  const auto integer = [&](int c, int low, int high, QString name) {
    v[c] = new Process::IntSlider{low, high, int(controlValue(defaults, c)), name, id(c), parent};
  };
  v[ChipModel] = new Process::ComboBox{{{"SCC", 0}, {"SCC+", 1}, {"HuC6280", 2}}, 0, "Model", id(ChipModel), parent};
  integer(Clock, 100000, 8000000, "Clock Hz");
  v[VoiceAssignment] = new Process::ComboBox{{}, 0, "Voice allocation", id(VoiceAssignment), parent};
  updateAllocationControl(*v[VoiceAssignment]);
  v[Volume] = new Process::FloatSlider{0.f, MaxVolume, 1.f, "Volume", id(Volume), parent};
  integer(Tuning, -2400, 2400, "Tuning cents"); integer(GlobalLeft, 0, 15, "Master left"); integer(GlobalRight, 0, 15, "Master right");
  integer(LfoDepth, 0, 3, "LFO depth"); integer(LfoFrequency, 0, 255, "LFO multiplier"); integer(LfoPeriod, 0, 4095, "LFO channel 2 period");
  for(int ch = 0; ch < 6; ++ch) {
    const auto prefix = QStringLiteral("Ch %1 ").arg(ch + 1);
    const auto c = [ch](ChannelField f) { return channelControl(ch, f); };
    v[c(Enabled)] = new Process::Toggle{true, prefix + "Enable", id(c(Enabled)), parent};
    v[c(Level)] = new Process::FloatSlider{0.f, 1.f, 1.f, prefix + "Level", id(c(Level)), parent};
    integer(c(ChannelTuning), -2400, 2400, prefix + "Tune cents");
    integer(c(Left), 0, 15, prefix + "Left"); integer(c(Right), 0, 15, prefix + "Right");
    v[c(Noise)] = new Process::Toggle{false, prefix + "Noise", id(c(Noise)), parent};
    integer(c(NoisePeriod), 0, 31, prefix + "Noise rate");
    v[c(Wave)] = makeWaveformInlet(Waveform::triangle(), 256, prefix + "Wave", id(c(Wave)), parent);
  }
  return v;
}
}
