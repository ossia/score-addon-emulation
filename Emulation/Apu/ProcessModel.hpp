// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Parameters.hpp"
#include "ProcessMetadata.hpp"
#include <Process/Process.hpp>
#include <score_addon_emulation_export.h>
#include <atomic>
#include <verdigris>
namespace Emulation::Apu {
class SCORE_ADDON_EMULATION_EXPORT ProcessModel final : public Process::ProcessModel {
  SCORE_SERIALIZE_FRIENDS
  PROCESS_METADATA_IMPL(Emulation::Apu::ProcessModel)
  W_OBJECT(ProcessModel)
public:
  explicit ProcessModel(const TimeVal&,const QString&,const Id<Process::ProcessModel>&,QObject*);
  template<typename Impl> explicit ProcessModel(Impl& vis,QObject* parent)
    : Process::ProcessModel{vis,parent} { vis.writeTo(*this); wireControls(); }
  ~ProcessModel() override;
  QString effect() const noexcept override { return {}; }
  Settings settings() const noexcept { return m_settings; }
  const DmcData& dmc() const noexcept { return m_dmc; }
  QString error() const { return m_error; }
  void setEngineError(QString);
  Process::Preset savePreset() const noexcept override;
  void loadPreset(const Process::Preset&) override;
  QByteArray stateData() const;
  void restoreState(const QByteArray&);
  void importDmc(const QString&);
  bool importDmcBytes(const QByteArray&);
  void settingsChanged() E_SIGNAL(SCORE_ADDON_EMULATION_EXPORT, settingsChanged)
  void assetChanged() E_SIGNAL(SCORE_ADDON_EMULATION_EXPORT, assetChanged)
  void errorChanged() E_SIGNAL(SCORE_ADDON_EMULATION_EXPORT, errorChanged)
private:
  void wireControls();
  void syncControls();
  void controlChanged(int);
  void setControlError(QString);
  void setResourceError(QString);
  void refreshError();
  Settings m_settings;
  DmcData m_dmc{defaultDmc()};
  QString m_error, m_controlError, m_resourceError, m_engineError;
  bool m_syncing{};
  std::shared_ptr<std::atomic<bool>> m_cancel;
};
}
template<> void DataStreamReader::read(const Emulation::Apu::ProcessModel&);
template<> void DataStreamWriter::write(Emulation::Apu::ProcessModel&);
template<> void JSONReader::read(const Emulation::Apu::ProcessModel&);
template<> void JSONWriter::write(Emulation::Apu::ProcessModel&);
