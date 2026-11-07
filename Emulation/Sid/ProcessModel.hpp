// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Process/Process.hpp>
#include "Engine.hpp"
#include "ProcessMetadata.hpp"
#include <score_addon_emulation_export.h>
#include <QByteArray>
#include <verdigris>
namespace Emulation::Sid
{
class SCORE_ADDON_EMULATION_EXPORT ProcessModel final : public Process::ProcessModel
{
  SCORE_SERIALIZE_FRIENDS
  PROCESS_METADATA_IMPL(Emulation::Sid::ProcessModel)
  W_OBJECT(ProcessModel)
public:
  explicit ProcessModel(const TimeVal&, const QString&, const Id<Process::ProcessModel>&, QObject*);
  template<typename Impl>
  explicit ProcessModel(Impl& vis, QObject* parent) : Process::ProcessModel{vis, parent}
  { vis.writeTo(*this); wireControls(); }
  ~ProcessModel() override;
  QString effect() const noexcept override { return {}; }
  const Patch& patch() const noexcept { return m_patch; }
  void loadPreset(const Process::Preset&) override;
  QString error() const
  { return !m_preparationError.isEmpty() ? m_preparationError : !m_engineError.isEmpty() ? m_engineError : m_editError; }
  void setEngineError(QString);
  void setPreparationError(QString);
  void patchChanged() E_SIGNAL(SCORE_ADDON_EMULATION_EXPORT, patchChanged)
  void errorChanged() E_SIGNAL(SCORE_ADDON_EMULATION_EXPORT, errorChanged)
private:
  void setEditError(QString);
  void wireControls();
  void syncControls();
  void controlChanged(int);
  QByteArray patchData() const;
  void restorePatch(const QByteArray&);
  Patch m_patch{defaultPatch()};
  QString m_preparationError, m_engineError, m_editError;
  bool m_syncing{};
};
}
template<> void DataStreamReader::read(const Emulation::Sid::ProcessModel&);
template<> void DataStreamWriter::write(Emulation::Sid::ProcessModel&);
template<> void JSONReader::read(const Emulation::Sid::ProcessModel&);
template<> void JSONWriter::write(Emulation::Sid::ProcessModel&);
