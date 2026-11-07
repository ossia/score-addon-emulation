// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProcessModel.hpp"
#include "Controls.hpp"
#include <Emulation/Volume.hpp>
#include <Process/Dataflow/Port.hpp>
#include <Process/ExternalFiles.hpp>
#include <score/document/DocumentInterface.hpp>
#include <score/tools/ProjectFiles.hpp>
#include <core/document/Document.hpp>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopedValueRollback>
#include <QStringList>
#include <stdexcept>
#include <wobjectimpl.h>
W_OBJECT_IMPL(Emulation::La::ProcessModel)
namespace Emulation::La
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
  out->setPropagate(true); m_outlets.push_back(out);
  wireControls();
  setEngineError(data.isEmpty() ? QStringLiteral("Select matching user-provided control and PCM ROMs in ROM settings.")
      : QStringLiteral("Select control and PCM ROMs separately in ROM settings; drop native score presets to restore a sound."));
}
ProcessModel::~ProcessModel() = default;
QString ProcessModel::error() const
{
  QStringList errors;
  for(const auto& message : {m_engineError, m_compatibilityError, m_parameterError, m_runtimeError})
    if(!message.isEmpty()) errors.push_back(message);
  return errors.join(QLatin1Char('\n'));
}
void ProcessModel::setEngineError(QString error)
{
  if(m_engineError == error) return;
  m_engineError = std::move(error); errorChanged();
}
void ProcessModel::setRuntimeError(QString error)
{
  if(m_runtimeError == error) return;
  m_runtimeError = std::move(error); errorChanged();
}
void ProcessModel::setParameterError(QString error)
{
  if(m_parameterError == error) return;
  m_parameterError = std::move(error); errorChanged();
}
void ProcessModel::refreshCompatibility()
{
  QString message;
  const std::array<std::string, 2> actual{m_patch.roms->controlHash, m_patch.roms->pcmHash};
  for(int i = 0; i < 2; ++i)
  {
    const auto& expected = (*m_patch.soundROMHashes)[i];
    if(expected.empty()) continue;
    if(actual[i].empty())
    {
      if(message.isEmpty()) message = QStringLiteral("Sound preset ROM identity has not been checked; prepare the local ROM pair.");
    }
    else if(expected != actual[i])
      message = QStringLiteral("Sound preset was authored with a different ROM identity; the local ROM pair is retained and factory/PCM sounds may differ.");
  }
  if(message != m_compatibilityError) { m_compatibilityError = std::move(message); errorChanged(); }
}
void ProcessModel::acceptROMIdentity(const RomReferences& refs, QString machine,
    std::shared_ptr<const ProgramCatalog> catalog)
{
  if(refs.controlPath != m_patch.roms->controlPath || refs.pcmPath != m_patch.roms->pcmPath) return;
  m_patch.roms = std::make_shared<const RomReferences>(refs);
  m_catalog = std::move(catalog);
  m_machine = std::move(machine); refreshProgramChoices(); refreshCompatibility(); errorChanged();
}
void ProcessModel::normalizeROMReferences(Patch& patch) const
{
  auto refs = std::make_shared<RomReferences>(*patch.roms);
  const auto* document = score::IDocument::try_documentFromObject(*this);
  const auto roots = document ? score::pathRoots(document->context()) : score::pathRoots();
  for(auto* path : {&refs->controlPath, &refs->pcmPath})
  {
    if(path->empty()) continue;
    const auto stored = QString::fromStdString(*path);
    // Read-only assets cannot participate in Save As reanchoring. Resolve them
    // at ingress, while the original document's roots are still available.
    // An unconfigured library token is independent of the document location;
    // retain it rather than inventing a path under an unrelated directory.
    if(score::isLibraryRelativePath(stored) && roots.library.isEmpty()) continue;
    *path = score::locateFilePath(stored, roots).toStdString();
  }
  if(*refs != *patch.roms) patch.roms = std::move(refs);
}
void ProcessModel::mapExternalFiles(Process::ExternalFileMap& map)
{
  // Deliberately do not visit the base FileChooser traversal: collecting these
  // proprietary assets would copy them into a distributable project/archive.
  const auto owner = map.owner;
  QScopedValueRollback guard{map.owner, owner};
  const auto report = [&](const std::string& reference, const char* role) {
    const auto path = QString::fromStdString(reference);
    map.owner = owner + QStringLiteral(" / ") + QString::fromLatin1(role);
    if(!path.isEmpty() && !QFileInfo::exists(path)) map.owner += QStringLiteral(" (missing)");
    map.readOnly(path, score::FileKind::Unknown);
  };
  report(m_patch.roms->controlPath, "LA control ROM");
  report(m_patch.roms->pcmPath, "LA PCM ROM");
}
void ProcessModel::migrateControls()
{
  const bool legacy = m_inlets.size() == VoiceAllocation + 1;
  if(!legacy && m_inlets.size() != ControlCount + 1) throw std::runtime_error("Invalid LA port schema.");
  for(int c = 0; c < (legacy ? VoiceAllocation : ControlCount); ++c)
    if(!qobject_cast<Process::ControlInlet*>(m_inlets[c + 1]) || m_inlets[c + 1]->id().val() != c + 1)
      throw std::runtime_error("Invalid LA control schema.");
  auto definitions = makeControls(this);
  if(legacy) m_inlets.push_back(definitions[VoiceAllocation]);
  for(int c = 0; c < ControlCount; ++c)
  {
    auto* old = qobject_cast<Process::ControlInlet*>(m_inlets[c + 1]);
    auto* next = definitions[c];
    if(old == next) continue;
    if(c == ControlROM || c == PcmROM || old->concreteKey() != next->concreteKey())
    {
      // Restore native widgets and intentional ROM-reselection behavior without
      // changing stable IDs, cables, addresses or authored exposed bindings.
      next->takeCables(std::move(*old));
      next->setAddress(old->address());
      if(old->hasOwnExposed()) next->setExposed(old->exposed());
      next->setScriptable(old->scriptable());
      if(!old->description().isEmpty()) next->setDescription(old->description());
      next->setInit(old->init());
      m_inlets[c + 1] = next;
      delete old;
    }
    else
    {
      old->setName(next->name());
      if(old->description().isEmpty()) old->setDescription(next->description());
      if(auto* combo = qobject_cast<Process::ComboBox*>(old))
        combo->setAlternatives(static_cast<Process::ComboBox*>(next)->alternatives);
      delete next;
    }
  }
}
void ProcessModel::wireControls()
{
  if(m_inlets.size() != ControlCount + 1 || m_outlets.size() != 1
      || !qobject_cast<Process::MidiInlet*>(m_inlets[0]) || !qobject_cast<Process::AudioOutlet*>(m_outlets[0]))
    throw std::runtime_error("Invalid LA port schema.");
  for(int c = 0; c < ControlCount; ++c)
    if(!qobject_cast<Process::ControlInlet*>(m_inlets[c + 1]) || m_inlets[c + 1]->id().val() != c + 1)
      throw std::runtime_error("Invalid LA control schema.");
  control(*this, Volume).setDomain(ossia::make_domain(0.f, MaxVolume));
  syncControls();
  setEngineError(QStringLiteral("LA ROM pair is not prepared. Select matching user-provided ROMs in ROM settings."));
  refreshCompatibility();
  for(int c = 0; c < ControlCount; ++c)
    connect(&control(*this, c), &Process::ControlInlet::valueChanged, this,
        [this, c](const ossia::value&) { if(!m_syncing) controlChanged(c); });
}
void ProcessModel::syncControls()
{
  QScopedValueRollback guard{m_syncing, true};
  control(*this, ControlROM).setValue(m_patch.roms->controlPath);
  control(*this, PcmROM).setValue(m_patch.roms->pcmPath);
  for(int c = Volume; c < ControlCount; ++c)
  {
    const auto value = controlValue(m_patch, c);
    if(c == Volume) control(*this, c).setValue(float(value));
    else if(dynamic_cast<Process::Toggle*>(&control(*this, c))) control(*this, c).setValue(bool(value));
    else control(*this, c).setValue(int(value));
  }
  refreshProgramChoices();
}
void ProcessModel::refreshProgramChoices(int part)
{
  QScopedValueRollback guard{m_syncing, true};
  const int first = part < 0 ? 0 : part, last = part < 0 ? 8 : part + 1;
  for(int i = first; i < last; ++i)
    if(auto* selector = qobject_cast<Process::ComboBox*>(&control(*this, partControl(i, Program))))
      selector->setAlternatives(programChoices(m_patch, i, m_catalog.get()));
}
Process::Preset ProcessModel::savePreset() const noexcept
{
  Process::Preset preset;
  preset.name = metadata().getName();
  preset.key = {Metadata<ConcreteKey_k, ProcessModel>::get(), {}};
  Patch sound = m_patch;
  auto identity = std::make_shared<RomReferences>(*m_patch.roms);
  identity->controlPath.clear(); identity->pcmPath.clear();
  if(!(*sound.soundROMHashes)[0].empty()) identity->controlHash = (*sound.soundROMHashes)[0];
  if(!(*sound.soundROMHashes)[1].empty()) identity->pcmHash = (*sound.soundROMHashes)[1];
  sound.roms = std::move(identity);
  const auto bytes = encodePatch(sound);
  const QByteArray data{reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size())};
  preset.data = QJsonDocument{QJsonObject{{QStringLiteral("Schema"), 2},
      {QStringLiteral("Backend"), QStringLiteral("munt")},
      {QStringLiteral("Patch"), QString::fromLatin1(data.toBase64())}}}.toJson(QJsonDocument::Compact);
  return preset;
}
void ProcessModel::loadPreset(const Process::Preset& preset)
{
  try
  {
    const auto json = QJsonDocument::fromJson(preset.data);
    const auto object = json.object();
    const int schema = object.value(QStringLiteral("Schema")).toInt();
    if(!json.isObject() || (schema != 1 && schema != 2)
        || object.value(QStringLiteral("Backend")).toString() != QStringLiteral("munt")
        || !object.value(QStringLiteral("Patch")).isString())
      throw std::runtime_error("Unsupported LA native preset schema.");
    const auto data = QByteArray::fromBase64(object.value(QStringLiteral("Patch")).toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    auto next = decodePatch({reinterpret_cast<const uint8_t*>(data.constData()), std::size_t(data.size())});
    // Both legacy and current sound presets retain the user's local ROM pair.
    // Legacy absolute paths are deliberately not imported or redistributed.
    auto hashes = std::make_shared<RomHashes>(*next.soundROMHashes);
    if((*hashes)[0].empty()) (*hashes)[0] = next.roms->controlHash;
    if((*hashes)[1].empty()) (*hashes)[1] = next.roms->pcmHash;
    next.soundROMHashes = std::move(hashes);
    next.roms = m_patch.roms;
    setParameterError({});
    if(next == m_patch) return;
    m_patch = std::move(next);
    if(m_patch.values[partControl(m_selectedPart, Bank)] != 1
        || m_patch.values[partControl(m_selectedPart, Program)] != m_patch.values[EditTimbre])
      for(int part = 0; part < 8; ++part)
        if(m_patch.values[partControl(part, Bank)] == 1
            && m_patch.values[partControl(part, Program)] == m_patch.values[EditTimbre])
        {
          m_selectedPart = part;
          break;
        }
    syncControls(); refreshCompatibility(); patchChanged();
  }
  catch(const std::exception& e) { setParameterError(QString::fromUtf8(e.what())); }
}
void ProcessModel::controlChanged(int c)
{
  Patch next = m_patch;
  if(c == ControlROM || c == PcmROM)
  {
    auto refs = std::make_shared<RomReferences>(*m_patch.roms);
    auto path = ossia::convert<std::string>(control(*this, c).value());
    if(c == ControlROM) { refs->controlPath = std::move(path); refs->controlHash.clear(); }
    else { refs->pcmPath = std::move(path); refs->pcmHash.clear(); }
    next.roms = std::move(refs); m_machine.clear(); m_catalog.reset();
    normalizeROMReferences(next);
  }
  else if(!setControl(next, c, ossia::convert<double>(control(*this, c).value())))
  { setParameterError(QStringLiteral("Invalid LA control value.")); syncControls(); return; }
  if(const char* error = validate(next)) { setParameterError(QString::fromUtf8(error)); syncControls(); return; }
  setParameterError({});
  if(next == m_patch)
  {
    syncControls();
    if(c == ControlROM || c == PcmROM) romPreparationRequested();
    return;
  }
  const bool selection = c >= PartBase && c < EditTimbre && (c - PartBase) % PartFieldCount <= Program;
  if(selection && (c - PartBase) / PartFieldCount == m_selectedPart
      && next.values[partControl(m_selectedPart, Bank)] == 1)
    next.values[EditTimbre] = next.values[partControl(m_selectedPart, Program)];
  m_patch = std::move(next);
  if(c == ControlROM || c == PcmROM)
  {
    setEngineError(QStringLiteral("LA ROM pair changed and is not yet prepared."));
    refreshCompatibility();
  }
  if(selection || c == EditTimbre || c == ControlROM || c == PcmROM) syncControls();
  patchChanged();
  if(c == ControlROM || c == PcmROM) romPreparationRequested();
}
QByteArray ProcessModel::patchData() const
{
  const auto bytes = encodePatch(m_patch);
  return {reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size())};
}
void ProcessModel::restorePatch(const QByteArray& bytes)
{
  m_patch = decodePatch({reinterpret_cast<const uint8_t*>(bytes.constData()), std::size_t(bytes.size())});
  normalizeROMReferences(m_patch);
}
}
