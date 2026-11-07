// SPDX-License-Identifier: GPL-3.0-or-later
#include "Controls.hpp"
#include <Emulation/Volume.hpp>
namespace Emulation::Wurli
{
void updateControlMetadata(Process::ControlInlet& inlet, int c)
{
  if(c == Volume) inlet.setDomain(ossia::make_domain(0.f, MaxVolume));
  const char* previous[] = {"Volume", "Amplifier", "Tremolo", "Speaker", "Model correction", "Velocity", "Voice allocation"};
  const char* current[] = {"Volume", "Amplifier drive", "Tremolo depth", "Speaker character", "Model correction", "Velocity curve", "Voice allocation"};
  if(inlet.name() == QString::fromLatin1(previous[c]))
    inlet.setName(QString::fromLatin1(current[c]));
  if(c == Amplifier && inlet.description().isEmpty())
    inlet.setDescription(QStringLiteral("Physical amplifier volume pot: changes drive and tone, independently of output Volume."));
}
Process::ComboBox* makeAllocationControl(QObject* parent)
{
  return new Process::ComboBox{
      {{"Mono", 0}, {"Poly", 1}, {"MIDI channel", 2}},
      int(Settings{}.voiceAllocation), "Voice allocation", Id<Process::Port>{VoiceAllocation + 1}, parent};
}
std::vector<Process::ControlInlet*> makeControls(QObject* parent)
{
  const Settings defaults;
  std::vector<Process::ControlInlet*> controls(ControlCount);
  const auto id = [](int c) { return Id<Process::Port>{c + 1}; };
  const char* names[] = {"Volume", "Amplifier", "Tremolo", "Speaker"};
  for(int c = Volume; c <= Speaker; ++c)
    controls[c] = new Process::FloatSlider{0.f, c == Volume ? MaxVolume : 1.f, float(controlValue(defaults, c)), QString::fromUtf8(names[c]), id(c), parent};
  controls[Correction] = new Process::Toggle{false, "Model correction", id(Correction), parent};
  controls[VelocityCurve] = new Process::ComboBox{
      {{"Linear", 0}, {"Soft", 1}, {"Hard", 2}, {"Fixed", 3}},
      defaults.velocityCurve, "Velocity", id(VelocityCurve), parent};
  controls[VoiceAllocation] = makeAllocationControl(parent);
  return controls;
}
}
