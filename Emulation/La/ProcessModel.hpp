// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Process/Process.hpp>
#include "Engine.hpp"
#include "ProcessMetadata.hpp"
#include <score_addon_emulation_export.h>
#include <QByteArray>
#include <verdigris>
namespace Emulation::La
{
class SCORE_ADDON_EMULATION_EXPORT ProcessModel final : public Process::ProcessModel
{
  SCORE_SERIALIZE_FRIENDS
  PROCESS_METADATA_IMPL(Emulation::La::ProcessModel)
  W_OBJECT(ProcessModel)
public:
  ProcessModel(const TimeVal&, const QString&, const Id<Process::ProcessModel>&, QObject*);
  template<typename Impl>
  ProcessModel(Impl& vis, QObject* parent) : Process::ProcessModel{vis, parent}
  { vis.writeTo(*this); wireControls(); }
  ~ProcessModel() override;
  QString effect() const noexcept override { return {}; }
  const Patch& patch() const noexcept { return m_patch; }
  Process::Preset savePreset() const noexcept override;
  void loadPreset(const Process::Preset&) override;
  void mapExternalFiles(Process::ExternalFileMap&) override;
  void acceptROMIdentity(const RomReferences&, QString machine,
      std::shared_ptr<const ProgramCatalog> = {});
  QString machine() const { return m_machine; }
  QString error() const;
  void setEngineError(QString);
  void setRuntimeError(QString);
  void setSelectedPart(int part) noexcept { m_selectedPart = part; }
  int selectedPart() const noexcept { return m_selectedPart; }
  void patchChanged() E_SIGNAL(SCORE_ADDON_EMULATION_EXPORT, patchChanged)
  void romPreparationRequested() E_SIGNAL(SCORE_ADDON_EMULATION_EXPORT, romPreparationRequested)
  void errorChanged() E_SIGNAL(SCORE_ADDON_EMULATION_EXPORT, errorChanged)
private:
  void wireControls();
  void migrateControls();
  void setParameterError(QString);
  void refreshCompatibility();
  void syncControls();
  void controlChanged(int);
  void refreshProgramChoices(int part = -1);
  void normalizeROMReferences(Patch&) const;
  QByteArray patchData() const;
  void restorePatch(const QByteArray&);
  Patch m_patch{defaultPatch()};
  QString m_engineError, m_parameterError, m_runtimeError, m_compatibilityError, m_machine;
  int m_selectedPart{};
  std::shared_ptr<const ProgramCatalog> m_catalog;
  bool m_syncing{};
};
}
template<> void DataStreamReader::read(const Emulation::La::ProcessModel&);
template<> void DataStreamWriter::write(Emulation::La::ProcessModel&);
template<> void JSONReader::read(const Emulation::La::ProcessModel&);
template<> void JSONWriter::write(Emulation::La::ProcessModel&);
