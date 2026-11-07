// SPDX-License-Identifier: GPL-3.0-or-later
#include "Controls.hpp"
#include <Emulation/Volume.hpp>
#include <QStringList>
#include <cstdlib>
namespace Emulation::La
{
namespace
{
// Keep the native FileChooser port and widget, but let an explicit same-path
// command request a retry after a missing file has been restored.
class RomFileChooser final : public Process::FileChooser
{
public:
  using Process::FileChooser::FileChooser;
  void setValue(const ossia::value& value) override
  {
    if(value == this->value()) valueChanged(value);
    else Process::FileChooser::setValue(value);
  }
};
std::vector<std::pair<QString, ossia::value>> labelledValues(int c)
{
  const int partField = c >= PartBase && c < EditTimbre ? (c - PartBase) % PartFieldCount : -1;
  const int partialField = c >= TimbreBase + 4 && c < VoiceAllocation ? (c - TimbreBase - 4) % 58 : -1;
  std::vector<std::pair<QString, ossia::value>> choices;
  const auto centered = [&](int zero, const QString& unit) {
    for(int v = 0; v <= controlField(c).maximum; ++v)
      choices.emplace_back(QStringLiteral("%1%2%3").arg(v > zero ? "+" : "").arg(v - zero).arg(unit), v);
  };
  if(c == VoiceAllocation)
    choices = {{QStringLiteral("Mono"), 0}, {QStringLiteral("Poly"), 1}, {QStringLiteral("MIDI channel"), 2}};
  else if(partField == AssignMode)
    choices = {{QStringLiteral("POLY1"), 0}, {QStringLiteral("POLY2"), 1},
        {QStringLiteral("POLY3"), 2}, {QStringLiteral("POLY4"), 3}};
  else if(c == RhythmChannel || partField == Channel)
  {
    for(int v = 0; v < 16; ++v) choices.emplace_back(QString::number(v + 1), v);
    choices.emplace_back(QStringLiteral("Off"), 16);
  }
  else if(c == RhythmPan || partField == Pan)
  {
    // Roland's native pan codes run from right to left, not left to right.
    for(int v = 0; v <= 14; ++v)
      choices.emplace_back(v == 7 ? QStringLiteral("Center")
          : QStringLiteral("%1 %2").arg(v < 7 ? "R" : "L").arg(std::abs(v - 7)), v);
  }
  else if(partField == KeyShift) centered(24, QStringLiteral(" st"));
  else if(partField == FineTune || partialField == 1) centered(50, QStringLiteral(" cents"));
  else if(c == MasterTune) centered(64, QStringLiteral(" steps"));
  else if(partialField == 7 || partialField == 27) centered(7, {});
  else if(partialField >= 15 && partialField <= 19) centered(50, {});
  else if(partialField == 44 || partialField == 46) centered(12, {});
  else if(c == TimbreBase + 2)
  {
    for(int mask = 0; mask < 16; ++mask)
    {
      QStringList enabled;
      for(int bit = 0; bit < 4; ++bit) if(mask & (1 << bit)) enabled.push_back(QString::number(bit + 1));
      choices.emplace_back(enabled.isEmpty() ? QStringLiteral("None") : enabled.join(QStringLiteral(" + ")), mask);
    }
  }
  return choices;
}
}
std::vector<std::pair<QString, ossia::value>> programChoices(
    const Patch& patch, int part, const ProgramCatalog* catalog)
{
  const auto bank = patch.values[partControl(part, Bank)];
  const int count = bank == 0 ? 128 : 64;
  std::vector<std::pair<QString, ossia::value>> choices;
  choices.reserve(count);
  for(int number = 0; number < count; ++number)
  {
    auto label = QString::number(number);
    QString name;
    if(bank == 1)
      name = QString::fromLatin1(reinterpret_cast<const char*>(patch.timbres[number].data()), 10).trimmed();
    else if(catalog)
    {
      if(bank == 0) name = QString::fromStdString(catalog->factory[number]);
      else name = QString::fromStdString(catalog->timbres[bank == 2 ? 0 : bank == 3 ? 1 : 3][number]);
      if(bank == 4 && number >= catalog->rhythmPrograms) label += QStringLiteral(" (unsupported)");
    }
    if(!name.isEmpty()) label += QStringLiteral("  ") + name;
    choices.emplace_back(std::move(label), number);
  }
  return choices;
}
std::vector<Process::ControlInlet*> makeControls(QObject* parent)
{
  std::vector<Process::ControlInlet*> result(ControlCount);
  const auto patch = defaultPatch();
  const auto id = [](int c) { return Id<Process::Port>{c + 1}; };
  for(int c = 0; c < ControlCount; ++c)
  {
    const auto field = controlField(c);
    auto name = QString::fromUtf8(field.name);
    if(c == ControlROM || c == PcmROM)
      result[c] = new RomFileChooser{QString{}, QStringLiteral("ROM images (*.rom *.bin);;All files (*)"), name, id(c), parent};
    else if(c == Volume)
      result[c] = new Process::FloatSlider{0.f, MaxVolume, 1.f, name, id(c), parent};
    else
    {
      if(c >= PartBase && c < EditTimbre) name = QStringLiteral("Part %1 / %2").arg(1 + (c - PartBase) / PartFieldCount).arg(name);
      if(c >= TimbreBase && c < TimbreBase + 4) name = QStringLiteral("Memory timbre / %1").arg(name);
      if(c >= TimbreBase + 4 && c < VoiceAllocation) name = QStringLiteral("Memory timbre / Partial %1 / %2").arg(1 + (c - TimbreBase - 4) / 58).arg(name);
      auto labelled = labelledValues(c);
      if(!labelled.empty())
        result[c] = new Process::ComboBox{std::move(labelled), int(controlValue(patch, c)), name, id(c), parent};
      else if(c >= PartBase && c < EditTimbre && (c - PartBase) % PartFieldCount == Bank)
        result[c] = new Process::ComboBox{{{QStringLiteral("Factory patches"),0},{QStringLiteral("Memory"),1},
          {QStringLiteral("ROM A"),2},{QStringLiteral("ROM B"),3},{QStringLiteral("Rhythm timbres"),4}}, 0, name, id(c), parent};
      else if(c >= PartBase && c < EditTimbre && (c - PartBase) % PartFieldCount == Program)
        result[c] = new Process::ComboBox{programChoices(patch, (c - PartBase) / PartFieldCount, nullptr),
            0, name, id(c), parent};
      else if(c == ReverbMode)
        result[c] = new Process::ComboBox{{{QStringLiteral("Room"),0},{QStringLiteral("Hall"),1},
          {QStringLiteral("Plate"),2},{QStringLiteral("Tap delay"),3}}, 0, name, id(c), parent};
      else if(c == DacMode)
        result[c] = new Process::ComboBox{{{QStringLiteral("Clean"),0},{QStringLiteral("Pure"),1},
          {QStringLiteral("MT-32 early"),2},{QStringLiteral("MT-32 late / CM"),3}}, 0, name, id(c), parent};
      else if(c == MidiDelay)
        result[c] = new Process::ComboBox{{{QStringLiteral("Immediate"),0},{QStringLiteral("Short messages"),1},
          {QStringLiteral("All messages"),2}}, 0, name, id(c), parent};
      else if(c == TimbreBase || c == TimbreBase + 1)
        result[c] = new Process::ComboBox{
          {{QStringLiteral("1: Synth + Synth"),0},{QStringLiteral("2: Synth ring + dry"),1},
           {QStringLiteral("3: PCM + Synth"),2},{QStringLiteral("4: PCM / Synth ring + dry"),3},
           {QStringLiteral("5: Synth / PCM ring + dry"),4},{QStringLiteral("6: PCM + PCM"),5},
           {QStringLiteral("7: PCM ring + dry"),6},{QStringLiteral("8: Synth stereo"),7},
           {QStringLiteral("9: PCM stereo"),8},{QStringLiteral("10: Synth ring"),9},
           {QStringLiteral("11: PCM / Synth ring"),10},{QStringLiteral("12: Synth / PCM ring"),11},
           {QStringLiteral("13: PCM ring"),12}}, 0, name, id(c), parent};
      else if(c >= TimbreBase + 4 && c < VoiceAllocation && (c - TimbreBase - 4) % 58 == 4)
        result[c] = new Process::ComboBox{{{QStringLiteral("Square / PCM 1"),0},{QStringLiteral("Saw / PCM 1"),1},
          {QStringLiteral("Square / PCM 2"),2},{QStringLiteral("Saw / PCM 2"),3}}, 1, name, id(c), parent};
      else if(field.maximum == 1)
        result[c] = new Process::Toggle{bool(controlValue(patch, c)), name, id(c), parent};
      else
        result[c] = new Process::IntSlider{0, field.maximum, int(controlValue(patch, c)), name, id(c), parent};
    }
  }
  result[VoiceAllocation]->setDescription(QStringLiteral(
      "Mono selects the last held melodic note globally; MIDI channel selects one per input channel. "
      "Native part receive assignments and layers are unchanged. Rhythm remains native polyphonic percussion; all parts share 32 partials."));
  result[MidiDelay]->setDescription(QStringLiteral(
      "Native MIDI interface delay. Mono and MIDI-channel notes use queued part-addressed delivery, "
      "including native voice-stealing retries; each layered part message is serialized."));
  return result;
}
}
