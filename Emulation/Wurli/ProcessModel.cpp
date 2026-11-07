// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProcessModel.hpp"
#include "Controls.hpp"
#include <Process/Preset.hpp>
#include <score/serialization/JSONValueVisitor.hpp>
#include <score/serialization/JSONVisitor.hpp>
#include <QScopedValueRollback>
#include <array>
#include <stdexcept>
#include <wobjectimpl.h>
W_OBJECT_IMPL(Emulation::Wurli::ProcessModel)
namespace Emulation::Wurli
{
namespace {
Process::ControlInlet& control(const ProcessModel& p, int c)
{ return *static_cast<Process::ControlInlet*>(p.inlets()[c + 1]); }
bool readValue(Settings& s, int c, const ossia::value& value)
{
  if(c == Correction && value.get_type() != ossia::val_type::BOOL) return false;
  if((c == VelocityCurve || c == VoiceAllocation) && value.get_type() != ossia::val_type::INT) return false;
  if(c < Correction && value.get_type() != ossia::val_type::FLOAT) return false;
  return setControl(s, c, ossia::convert<double>(value));
}
}
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
void ProcessModel::validatePorts() const
{
  if(m_inlets.size() != ControlCount + 1 || m_outlets.size() != 1
      || !qobject_cast<Process::MidiInlet*>(m_inlets[0]) || m_inlets[0]->id().val() != 0
      || !qobject_cast<Process::AudioOutlet*>(m_outlets[0]) || m_outlets[0]->id().val() != 0)
    throw std::runtime_error("Invalid Wurlitzer port schema.");
  for(int c = 0; c < ControlCount; ++c) {
    if(!qobject_cast<Process::ControlInlet*>(m_inlets[c + 1]) || m_inlets[c + 1]->id().val() != c + 1)
      throw std::runtime_error("Invalid Wurlitzer control ID.");
    if((c < Correction && !qobject_cast<Process::FloatSlider*>(m_inlets[c + 1]))
        || (c == Correction && !qobject_cast<Process::Toggle*>(m_inlets[c + 1]))
        || ((c == VelocityCurve || c == VoiceAllocation) && !qobject_cast<Process::ComboBox*>(m_inlets[c + 1])))
      throw std::runtime_error("Invalid Wurlitzer control type.");
  }
}
void ProcessModel::restoreControls()
{
  validatePorts();
  Settings next;
  for(int c = 0; c < ControlCount; ++c)
    if(!readValue(next, c, control(*this, c).value())) throw std::runtime_error("Invalid Wurlitzer semantic control.");
  if(auto error = validate(next)) throw std::runtime_error(error);
  m_settings = next;
}
void ProcessModel::wireControls()
{
  restoreControls();
  for(int c = 0; c < ControlCount; ++c) updateControlMetadata(control(*this, c), c);
  for(int c = 0; c < ControlCount; ++c)
    connect(&control(*this, c), &Process::ControlInlet::valueChanged, this,
        [this, c](const ossia::value&) { if(!m_syncing) controlChanged(c); });
}
void ProcessModel::syncControls()
{
  QScopedValueRollback guard{m_syncing, true};
  for(int c = 0; c < ControlCount; ++c) {
    const auto v = controlValue(m_settings, c);
    if(c == Correction) control(*this, c).setValue(bool(v));
    else if(c == VelocityCurve || c == VoiceAllocation) control(*this, c).setValue(int(v));
    else control(*this, c).setValue(float(v));
  }
}
void ProcessModel::controlChanged(int c)
{
  auto next = m_settings;
  if(!readValue(next, c, control(*this, c).value())) {
    setEditError(QStringLiteral("Invalid Wurlitzer control value.")); syncControls(); return;
  }
  setEditError({});
  if(next == m_settings) return;
  m_settings = next; settingsChanged();
}
void ProcessModel::loadPreset(const Process::Preset& preset)
{
  // Parse and validate the entire native control array before changing any port.
  const auto doc = readJson(preset.data);
  auto next = m_settings;
  // Presets predating the appended port retain their original Poly64 behavior.
  next.voiceAllocation = Allocation::Poly;
  std::array<bool, ControlCount> seen{};
  const auto fail = [this] { setEditError(QStringLiteral("Invalid Wurlitzer native preset.")); };
  if(!doc.IsArray()) { fail(); return; }
  for(const auto& entry : doc.GetArray()) {
    if(!entry.IsArray() || entry.Size() != 2 || !entry[0].IsInt() || !entry[1].IsObject()) { fail(); return; }
    const int id = entry[0].GetInt();
    if(id < 1 || id > ControlCount) { fail(); return; }
    const int c = id - 1;
    if(seen[c]) { fail(); return; }
    // Require the native value discriminant, not a permissive numeric conversion.
    const bool integer = c == VelocityCurve || c == VoiceAllocation;
    const char* key = c == Correction ? "Bool" : integer ? "Int" : "Float";
    const auto it = entry[1].FindMember(key);
    if(entry[1].MemberCount() != 1 || it == entry[1].MemberEnd()
        || (c == Correction ? !it->value.IsBool() : integer ? !it->value.IsInt() : !it->value.IsNumber())) { fail(); return; }
    const double v = c == Correction ? double(it->value.GetBool()) : it->value.GetDouble();
    if(!setControl(next, c, v)) { fail(); return; }
    seen[c] = true;
  }
  if(validate(next)) { fail(); return; }
  setEditError({});
  if(next == m_settings) return;
  m_settings = next;
  syncControls();
  settingsChanged();
}
void ProcessModel::setEngineError(QString error)
{
  if(error == m_engineError) return;
  m_engineError = std::move(error); errorChanged();
}
void ProcessModel::setPreparationError(QString error)
{
  if(error == m_preparationError) return;
  m_preparationError = std::move(error); errorChanged();
}
void ProcessModel::setEditError(QString error)
{
  if(error == m_editError) return;
  m_editError = std::move(error); errorChanged();
}
}
