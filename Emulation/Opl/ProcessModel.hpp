#pragma once
#include <Process/Process.hpp>
#include <Emulation/Opl/Engine.hpp>
#include <Emulation/Opl/ProcessMetadata.hpp>
#include <score_addon_emulation_export.h>
#include <QByteArray>
#include <atomic>
#include <memory>
#include <verdigris>

namespace Emulation::Opl
{
class SCORE_ADDON_EMULATION_EXPORT ProcessModel final : public Process::ProcessModel
{
  SCORE_SERIALIZE_FRIENDS
  PROCESS_METADATA_IMPL(Emulation::Opl::ProcessModel)
  W_OBJECT(ProcessModel)
public:
  explicit ProcessModel(const TimeVal&, const QString&, const Id<Process::ProcessModel>&, QObject*);
  template <typename Impl>
  explicit ProcessModel(Impl& vis, QObject* parent) : Process::ProcessModel{vis, parent}
  { vis.writeTo(*this); wireControls(); }
  ~ProcessModel() override;
  QString effect() const noexcept override;
  Settings settings() const;
  Process::Preset savePreset() const noexcept override;
  void loadPreset(const Process::Preset&) override;
  bool presetMatches(const Process::Preset&) const noexcept override;
  std::shared_ptr<const Bank> bank() const { return m_bank; }
  Address selectedAddress() const;
  QString error() const;
  QString selectedName() const;
  QString filePath() const { return m_filePath; }
  void loadFile(const QString&);
  void setEngineError(QString);
  void setRuntimeError(QString);
  void bankChanged() E_SIGNAL(SCORE_ADDON_EMULATION_EXPORT, bankChanged)
  void instrumentChanged() E_SIGNAL(SCORE_ADDON_EMULATION_EXPORT, instrumentChanged)
  void selectedAddressChanged() E_SIGNAL(SCORE_ADDON_EMULATION_EXPORT, selectedAddressChanged)
  void depthsChanged() E_SIGNAL(SCORE_ADDON_EMULATION_EXPORT, depthsChanged)
  void errorChanged() E_SIGNAL(SCORE_ADDON_EMULATION_EXPORT, errorChanged)
private:
  void wireControls();
  void syncPatchControls();
  void syncDomains();
  void setEditError(QString);
  void setResourceError(QString);
  void controlChanged(int);
  QByteArray bankData() const;
  void restoreBank(const QByteArray&);
  QString storedPath() const;
  std::shared_ptr<Bank> m_bank;
  std::shared_ptr<std::atomic<bool>> m_cancelToken;
  QString m_filePath;
  QString m_engineError;
  QString m_runtimeError;
  QString m_editError;
  QString m_resourceError;
  Settings m_acceptedSettings;
  float m_acceptedVolume{1.f};
  bool m_syncing{};
};
}
template <> void DataStreamReader::read(const Emulation::Opl::ProcessModel&);
template <> void DataStreamWriter::write(Emulation::Opl::ProcessModel&);
template <> void JSONReader::read(const Emulation::Opl::ProcessModel&);
template <> void JSONWriter::write(Emulation::Opl::ProcessModel&);
