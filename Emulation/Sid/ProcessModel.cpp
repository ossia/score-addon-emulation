// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProcessModel.hpp"
#include "Controls.hpp"
#include <Emulation/Volume.hpp>
#include <Process/Dataflow/Port.hpp>
#include <Process/PresetHelpers.hpp>
#include <QScopedValueRollback>
#include <stdexcept>
#include <wobjectimpl.h>
W_OBJECT_IMPL(Emulation::Sid::ProcessModel)
namespace Emulation::Sid
{
namespace
{
Process::ControlInlet& control(const ProcessModel& p, int c)
{ return *static_cast<Process::ControlInlet*>(p.inlets()[c + 1]); }
}
ProcessModel::ProcessModel(const TimeVal& duration, const QString& data,
    const Id<Process::ProcessModel>& id, QObject* parent)
    : Process::ProcessModel{duration, id, Metadata<ObjectKey_k, ProcessModel>::get(), parent}
{
  metadata().setInstanceName(*this);
  m_inlets.push_back(new Process::MidiInlet{"MIDI In", Id<Process::Port>{0}, this});
  for(auto* c : makeControls(this)) m_inlets.push_back(c);
  auto* out = new Process::AudioOutlet{"Stereo Out", Id<Process::Port>{0}, this};
  out->setPropagate(true);
  m_outlets.push_back(out);
  wireControls();
  if(!data.isEmpty()) setEditError(QStringLiteral("SID does not import GoatTracker INS, SNG, or SID tune files."));
}
ProcessModel::~ProcessModel() = default;
void ProcessModel::setEngineError(QString error)
{
  if(m_engineError == error) return;
  m_engineError = std::move(error);
  errorChanged();
}
void ProcessModel::setPreparationError(QString error)
{
  if(m_preparationError == error) return;
  m_preparationError = std::move(error);
  errorChanged();
}
void ProcessModel::setEditError(QString error)
{
  if(m_editError == error) return;
  m_editError = std::move(error);
  errorChanged();
}
void ProcessModel::wireControls()
{
  if(m_inlets.size() != ControlCount + 1 || m_outlets.size() != 1
      || !qobject_cast<Process::MidiInlet*>(m_inlets[0])
      || !qobject_cast<Process::AudioOutlet*>(m_outlets[0]))
    throw std::runtime_error("Invalid SID port schema.");
  for(int c = 0; c < ControlCount; ++c)
    if(!qobject_cast<Process::ControlInlet*>(m_inlets[c + 1]) || m_inlets[c + 1]->id().val() != c + 1)
      throw std::runtime_error("Invalid SID control schema.");
  // Domain migration leaves serialized values, IDs and automation unchanged.
  control(*this, Volume).setDomain(ossia::make_domain(0.f, MaxVolume));
  for(int c = 0; c < ControlCount; ++c) updateControlMetadata(control(*this, c), c);
  syncControls();
  for(int c = 0; c < ControlCount; ++c)
    connect(&control(*this, c), &Process::ControlInlet::valueChanged, this,
        [this, c](const ossia::value&) { if(!m_syncing) controlChanged(c); });
}
void ProcessModel::syncControls()
{
  QScopedValueRollback guard{m_syncing, true};
  for(int c = 0; c < ControlCount; ++c)
  {
    const auto value = controlValue(m_patch, c);
    if(c == Volume) control(*this, c).setValue(float(value));
    else if(dynamic_cast<Process::Toggle*>(&control(*this, c))) control(*this, c).setValue(bool(value));
    else control(*this, c).setValue(int(value));
  }
}
void ProcessModel::loadPreset(const Process::Preset& preset)
{
  const auto doc = readJson(preset.data);
  if(!doc.IsArray()) return;
  Patch next = m_patch;
  for(const auto& entry : doc.GetArray())
  {
    if(!entry.IsArray() || entry.Size() != 2 || !entry[0].IsInt())
    { setEditError(QStringLiteral("Invalid SID preset control entry.")); return; }
    const int id = entry[0].GetInt();
    if(id <= 0 || id > ControlCount) continue;
    const int c = id - 1;
    const auto value = JsonValue{entry[1]}.to<ossia::value>();
    if(!setControl(next, c, ossia::convert<double>(value)))
    { setEditError(QStringLiteral("Invalid SID preset control value.")); return; }
  }
  if(const char* error = validate(next))
  { setEditError(QString::fromUtf8(error)); return; }
  setEditError({});
  if(next == m_patch) return;
  // Validate before publishing any inlet value: failed recall cannot leak a
  // partial patch to the execution component or UI observers.
  m_patch = next;
  syncControls();
  patchChanged();
}
void ProcessModel::controlChanged(int c)
{
  Patch next = m_patch;
  const bool valid = setControl(next, c, ossia::convert<double>(control(*this, c).value()));
  const char* error = valid ? validate(next) : "Invalid SID control value.";
  if(error) { setEditError(QString::fromUtf8(error)); syncControls(); return; }
  setEditError({});
  if(next == m_patch) return;
  m_patch = next;
  patchChanged();
}
QByteArray ProcessModel::patchData() const
{
  const auto bytes = encodePatch(m_patch);
  return {reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size())};
}
void ProcessModel::restorePatch(const QByteArray& bytes)
{
  m_patch = decodePatch({reinterpret_cast<const uint8_t*>(bytes.constData()), std::size_t(bytes.size())});
}
}
