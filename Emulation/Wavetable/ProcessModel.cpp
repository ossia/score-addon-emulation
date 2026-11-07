#include "ProcessModel.hpp"
#include "Controls.hpp"
#include <Emulation/Volume.hpp>
#include <Emulation/WaveformInlet.hpp>
#include <Process/Dataflow/Port.hpp>
#include <score/model/EntitySerialization.hpp>
#include <QScopedValueRollback>
#include <stdexcept>
#include <wobjectimpl.h>
W_OBJECT_IMPL(Emulation::Wavetable::ProcessModel)
namespace Emulation::Wavetable {
namespace {
using Values = std::array<ossia::value, ControlCount>;
void writeValues(JSONReader& r, const Values& values) {
  r.stream.StartArray();
  for(int c = 0; c < ControlCount; ++c) {
    r.stream.StartArray(); r.stream.Int(portId(c)); r.readFrom(values[c]); r.stream.EndArray();
  }
  r.stream.EndArray();
}
QByteArray presetData(const Values& values, const LevelMaxima& levels) {
  JSONReader r;
  r.stream.StartObject();
  r.stream.Key("Schema"); r.stream.Int(4);
  r.stream.Key("Controls"); writeValues(r, values);
  r.stream.Key("LevelMaximums"); r.stream.StartArray();
  for(double level : levels) r.stream.Double(level);
  r.stream.EndArray();
  r.stream.EndObject();
  return r.toByteArray();
}
void validateValues(const Values& values, const LevelMaxima& levels) {
  Settings settings;
  for(int c = 0; c < ControlCount; ++c)
    if(!setControl(settings, c, values[c], c < ChannelBase ? 1. : levels[(c - ChannelBase) / ChannelStride]))
      throw std::runtime_error("Invalid wavetable control value.");
  if(auto error = validate(settings)) throw std::runtime_error(error);
}
}
ProcessModel::ProcessModel(const TimeVal& duration, const QString&, const Id<Process::ProcessModel>& id, QObject* parent)
  : Process::ProcessModel{duration, id, Metadata<ObjectKey_k, ProcessModel>::get(), parent} {
  metadata().setInstanceName(*this);
  m_inlets.push_back(new Process::MidiInlet{"MIDI In", Id<Process::Port>{0}, this});
  for(auto* c : makeControls(this)) m_inlets.push_back(c);
  auto* out = new Process::AudioOutlet{"Stereo Out", Id<Process::Port>{0}, this}; out->setPropagate(true); m_outlets.push_back(out);
  wireControls();
}
ProcessModel::~ProcessModel() = default;
QString ProcessModel::effect() const noexcept { return {}; }
LevelMaxima ProcessModel::levelMaxima() const {
  LevelMaxima levels;
  for(int ch = 0; ch < 6; ++ch) {
    const auto& domain = static_cast<Process::ControlInlet*>(m_inlets[channelControl(ch, Level) + 1])->domain().get();
    const auto low = domain.convert_min<double>(), high = domain.convert_max<double>();
    if(low != 0. || !std::isfinite(high) || high <= 0.)
      throw std::runtime_error("Invalid wavetable musical level domain.");
    levels[ch] = high;
  }
  return levels;
}
Settings ProcessModel::settings() const {
  Settings s;
  const auto levels = levelMaxima();
  for(int c = 0; c < ControlCount; ++c) {
    bool valid{};
    if(isWave(c)) {
      const auto& inlet = *static_cast<Dataflow::WaveformInlet*>(m_inlets[c + 1]);
      valid = Process::prepareWaveform(inlet.wavetable(), s.channels[(c - ChannelBase) / ChannelStride].wave, 0., 1.);
    } else {
      valid = setControl(s, c, static_cast<Process::ControlInlet*>(m_inlets[c + 1])->value(),
          c < ChannelBase ? 1. : levels[(c - ChannelBase) / ChannelStride]);
    }
    if(!valid) { s.clock = 0; break; }
  }
  return s;
}
Process::Preset ProcessModel::savePreset() const noexcept {
  Process::Preset preset;
  preset.name = metadata().getName(); preset.key.key = concreteKey();
  Values values;
  for(int c = 0; c < ControlCount; ++c) values[c] = static_cast<Process::ControlInlet*>(m_inlets[c + 1])->value();
  preset.data = presetData(values, levelMaxima());
  return preset;
}
void ProcessModel::loadPreset(const Process::Preset& preset) {
  try {
    const auto doc = readJson(preset.data);
    if(!doc.IsObject() || !doc.HasMember("Schema") || !doc["Schema"].IsInt() || doc["Schema"].GetInt() != 4
        || !doc.HasMember("Controls") || !doc["Controls"].IsArray())
      throw std::runtime_error("Unsupported wavetable preset schema.");
    Values values;
    const auto destinationLevels = levelMaxima();
    auto sourceLevels = normalizedLevels;
    if(doc.HasMember("LevelMaximums")) {
      const auto& levels = doc["LevelMaximums"];
      if(!levels.IsArray() || levels.Size() != 6) throw std::runtime_error("Invalid wavetable preset level domains.");
      for(int ch = 0; ch < 6; ++ch) {
        if(!levels[ch].IsNumber() || !std::isfinite(levels[ch].GetDouble()) || levels[ch].GetDouble() <= 0.)
          throw std::runtime_error("Invalid wavetable preset level domain.");
        sourceLevels[ch] = levels[ch].GetDouble();
      }
    }
    for(int c = 0; c < ControlCount; ++c) values[c] = static_cast<Process::ControlInlet*>(m_inlets[c + 1])->value();
    for(const auto& entry : doc["Controls"].GetArray()) {
      if(!entry.IsArray() || entry.Size() != 2 || !entry[0].IsInt()) throw std::runtime_error("Invalid wavetable preset control entry.");
      int c{}; while(c < ControlCount && portId(c) != entry[0].GetInt()) ++c;
      if(c == ControlCount) throw std::runtime_error("Unknown wavetable preset control.");
      values[c] = JsonValue{entry[1]}.to<ossia::value>();
      if(c >= ChannelBase && (c - ChannelBase) % ChannelStride == Level) {
        const int ch = (c - ChannelBase) / ChannelStride;
        const auto value = Process::waveformNumber(values[c]);
        if(!value) throw std::runtime_error("Invalid wavetable preset level.");
        values[c] = float(*value * destinationLevels[ch] / sourceLevels[ch]);
      }
    }
    validateValues(values, destinationLevels);
    {
      QScopedValueRollback guard{m_loadingPreset, true};
      for(int c = 0; c < ControlCount; ++c) static_cast<Process::ControlInlet*>(m_inlets[c + 1])->setValue(values[c]);
    }
    updateWaveformLevels(); setEngineError({}); settingsChanged();
  } catch(const std::exception& error) { setEngineError(QString::fromUtf8(error.what())); }
}
void ProcessModel::validatePorts() const {
  if(m_inlets.size() != ControlCount + 1 || m_outlets.size() != 1 || !qobject_cast<Process::MidiInlet*>(m_inlets[0]) || !qobject_cast<Process::AudioOutlet*>(m_outlets[0]))
    throw std::runtime_error("Invalid wavetable port layout.");
  for(int c = 0; c < ControlCount; ++c) {
    if(!qobject_cast<Process::ControlInlet*>(m_inlets[c + 1]) || m_inlets[c + 1]->id() != Id<Process::Port>{portId(c)})
      throw std::runtime_error("Invalid wavetable control ID.");
    if(isWave(c)) {
      const auto* inlet = dynamic_cast<Dataflow::WaveformInlet*>(m_inlets[c + 1]);
      if(!inlet || inlet->supportsFrames() || inlet->frameSizeConstraint() != 32
          || inlet->wavetable().frameSize != 32 || inlet->wavetable().frameCount() != 1)
        throw std::runtime_error("Invalid wavetable waveform inlet.");
    }
  }
  if(auto e = validate(settings())) throw std::runtime_error(e);
}
void ProcessModel::updateWaveformLevels() {
  const int levels = ossia::convert<int>(static_cast<Process::ControlInlet*>(m_inlets[ChipModel + 1])->value()) == int(Model::HuC6280) ? 32 : 256;
  for(int ch = 0; ch < 6; ++ch)
    static_cast<Dataflow::WaveformInlet*>(m_inlets[channelControl(ch, Wave) + 1])->setAmplitudeLevels(levels);
}
void ProcessModel::wireControls() {
  validatePorts();
  updateAllocationControl(*static_cast<Process::ControlInlet*>(m_inlets[VoiceAssignment + 1]));
  static_cast<Process::ControlInlet*>(m_inlets[Volume + 1])->setDomain(ossia::make_domain(0.f, MaxVolume));
  updateWaveformLevels();
  for(int c = 0; c < ControlCount; ++c) {
    m_inlets[c + 1]->stableIdentity = true;
    connect(static_cast<Process::ControlInlet*>(m_inlets[c + 1]), &Process::ControlInlet::valueChanged, this, [this, c](const ossia::value&) {
      if(m_loadingPreset) return;
      if(c == ChipModel) updateWaveformLevels();
      auto s = settings(); const auto* e = validate(s); setEngineError(e ? QString::fromUtf8(e) : QString{}); settingsChanged();
    });
  }
}
void ProcessModel::setEngineError(QString error) { if(error == m_error) return; m_error = std::move(error); errorChanged(); }
}
