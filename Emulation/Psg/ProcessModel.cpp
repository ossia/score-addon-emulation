#include "ProcessModel.hpp"
#include "Controls.hpp"
#include <Emulation/Volume.hpp>
#include <Process/Dataflow/Port.hpp>
#include <Process/PresetHelpers.hpp>
#include <QScopedValueRollback>
#include <cmath>
#include <stdexcept>
#include <wobjectimpl.h>
W_OBJECT_IMPL(Emulation::Psg::ProcessModel)
namespace Emulation::Psg {
ProcessModel::ProcessModel(const TimeVal& duration, const QString&, const Id<Process::ProcessModel>& id, QObject* parent)
  : Process::ProcessModel{duration, id, Metadata<ObjectKey_k, ProcessModel>::get(), parent}
{
  metadata().setInstanceName(*this);
  m_inlets.push_back(new Process::MidiInlet{"MIDI In", Id<Process::Port>{0}, this});
  for(auto* c : makeControls(this)) m_inlets.push_back(c);
  auto* out = new Process::AudioOutlet{"Stereo Out", Id<Process::Port>{0}, this};
  out->setPropagate(true); m_outlets.push_back(out);
  wireControls();
}
ProcessModel::~ProcessModel() = default;
QString ProcessModel::effect() const noexcept { return {}; }
Settings ProcessModel::settings() const
{
  Settings s;
  for(int c = 0; c < ControlCount; ++c) {
    const double value = ossia::convert<double>(static_cast<Process::ControlInlet*>(inlets()[c + 1])->value());
    if(!validControlValue(c, value)) {
      s.clock = 0; return s;
    }
    setControl(s, c, value);
  }
  return s;
}
void ProcessModel::loadPreset(const Process::Preset& preset)
{
  const auto doc = readJson(preset.data);
  if(!doc.IsArray()) return;
  std::array<ossia::value, ControlCount> values;
  for(int c = 0; c < ControlCount; ++c)
    values[c] = static_cast<Process::ControlInlet*>(inlets()[c + 1])->value();
  for(const auto& entry : doc.GetArray())
  {
    if(!entry.IsArray() || entry.Size() != 2 || !entry[0].IsInt())
    { setControlError(QStringLiteral("Invalid PSG preset control entry.")); return; }
    const int id = entry[0].GetInt();
    if(id > 0 && id <= ControlCount) values[id - 1] = JsonValue{entry[1]}.to<ossia::value>();
  }
  Settings next;
  for(int c = 0; c < ControlCount; ++c)
  {
    const double value = ossia::convert<double>(values[c]);
    if(!validControlValue(c, value))
    { setControlError(QStringLiteral("Invalid PSG preset control value.")); return; }
    setControl(next, c, value);
  }
  if(const char* error = validate(next))
  { setControlError(QString::fromUtf8(error)); return; }
  values[SoftwareEnvelope] = int(next.softwareEnvelope);
  {
    // Native preset recall and undo publish one complete, validated setting.
    QScopedValueRollback guard{m_loadingPreset, true};
    for(int c = 0; c < ControlCount; ++c)
      static_cast<Process::ControlInlet*>(inlets()[c + 1])->setValue(values[c]);
    m_acceptedValues = values;
  }
  setControlError({});
  settingsChanged();
}
void ProcessModel::validatePorts() const
{
  if(m_inlets.size() != ControlCount + 1 || m_outlets.size() != 1
      || !qobject_cast<Process::MidiInlet*>(m_inlets[0])
      || !qobject_cast<Process::AudioOutlet*>(m_outlets[0]))
    throw std::runtime_error("Invalid PSG port layout.");
  for(int c = 0; c < ControlCount; ++c)
    if(!qobject_cast<Process::ControlInlet*>(m_inlets[c + 1]) || m_inlets[c + 1]->id() != Id<Process::Port>{c + 1})
      throw std::runtime_error("Invalid PSG control ID.");
}
void ProcessModel::wireControls()
{
  validatePorts();
  updateAllocationControl(*static_cast<Process::ControlInlet*>(m_inlets[VoiceAssignment + 1]));
  auto& noise = *m_inlets[SegaNoiseEnabled + 1];
  if(noise.name() == QStringLiteral("Noise \u2022 MIDI 10"))
    noise.setName(QStringLiteral("Noise (MIDI 10)"));
  // Domain migration leaves serialized values, IDs and automation unchanged.
  static_cast<Process::ControlInlet*>(m_inlets[Volume + 1])->setDomain(
      ossia::make_domain(0.f, MaxVolume));
  // Schema 1 used a toggle at this stable ID. Keep its values, addresses,
  // publication and cables, but expose the exclusive owner as a native selector.
  auto*& source = m_inlets[SoftwareEnvelope + 1];
  if(qobject_cast<Process::Toggle*>(source)) {
    auto* old = static_cast<Process::ControlInlet*>(source);
    auto* replacement = new Process::ComboBox{
        {{"Native", 0}, {"Software ADSR", 1}}, 0, "Envelope source", old->id(), this};
    replacement->setValue(ossia::convert<int>(old->value()));
    replacement->setInit(ossia::convert<int>(old->init()));
    replacement->setAddress(old->address());
    if(old->hasOwnExposed()) replacement->setExposed(old->exposed());
    replacement->setScriptable(old->scriptable());
    replacement->setDescription(old->description());
    replacement->takeCables(std::move(*old));
    source = replacement;
    delete old;
  }
  for(int ch = 0; ch < 3; ++ch) {
    const std::array<const char*, 4> names{"Tone", "Noise", "Level", "Envelope routing"};
    for(int f = 0; f < 4; ++f)
      m_inlets[channelControl(ch, ChannelField(f)) + 1]->setName(
          QStringLiteral("Tone %1 %2").arg(QChar('A' + ch)).arg(QString::fromLatin1(names[f])));
  }
  if(auto e = validate(settings())) throw std::runtime_error(e);
  for(int c = 0; c < ControlCount; ++c) {
    auto* inlet = static_cast<Process::ControlInlet*>(m_inlets[c + 1]);
    m_acceptedValues[c] = inlet->value();
    connect(inlet, &Process::ControlInlet::valueChanged,
        this, [this, c, inlet](const ossia::value& value) {
      if(m_loadingPreset) return;
      const auto* e = validate(settings());
      if(e) {
        QScopedValueRollback guard{m_loadingPreset, true};
        inlet->setValue(m_acceptedValues[c]);
        setControlError(QString::fromUtf8(e));
        return;
      }
      if(c == SoftwareEnvelope) {
        const int source = ossia::convert<int>(value);
        QScopedValueRollback guard{m_loadingPreset, true};
        inlet->setValue(source);
      }
      m_acceptedValues[c] = inlet->value();
      setControlError({});
      settingsChanged();
    });
  }
}
void ProcessModel::setEngineError(QString error)
{
  m_engineError = std::move(error);
  refreshError();
}
void ProcessModel::setControlError(QString error)
{
  m_controlError = std::move(error);
  refreshError();
}
void ProcessModel::refreshError()
{
  auto error = m_engineError.isEmpty() ? m_controlError : m_engineError;
  if(error == m_error) return;
  m_error = std::move(error); errorChanged();
}
}
