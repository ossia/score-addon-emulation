#pragma once
#include <Process/Process.hpp>
#include <Emulation/Psg/Parameters.hpp>
#include <Emulation/Psg/ProcessMetadata.hpp>
#include <score_addon_emulation_export.h>
#include <ossia/network/value/value.hpp>
#include <verdigris>
namespace Emulation::Psg {
class SCORE_ADDON_EMULATION_EXPORT ProcessModel final : public Process::ProcessModel {
  SCORE_SERIALIZE_FRIENDS
  PROCESS_METADATA_IMPL(Emulation::Psg::ProcessModel)
  W_OBJECT(ProcessModel)
public:
  explicit ProcessModel(const TimeVal&, const QString&, const Id<Process::ProcessModel>&, QObject*);
  template<typename Impl> explicit ProcessModel(Impl& vis, QObject* parent)
    : Process::ProcessModel{vis, parent} { vis.writeTo(*this); wireControls(); }
  ~ProcessModel() override;
  QString effect() const noexcept override;
  Settings settings() const;
  void loadPreset(const Process::Preset&) override;
  QString error() const { return m_error; }
  void setEngineError(QString);
  void settingsChanged() E_SIGNAL(SCORE_ADDON_EMULATION_EXPORT, settingsChanged)
  void errorChanged() E_SIGNAL(SCORE_ADDON_EMULATION_EXPORT, errorChanged)
private:
  void wireControls();
  void validatePorts() const;
  void setControlError(QString);
  void refreshError();
  std::array<ossia::value, ControlCount> m_acceptedValues;
  QString m_controlError;
  QString m_engineError;
  QString m_error;
  bool m_loadingPreset{};
};
}
template<> void DataStreamReader::read(const Emulation::Psg::ProcessModel&);
template<> void DataStreamWriter::write(Emulation::Psg::ProcessModel&);
template<> void JSONReader::read(const Emulation::Psg::ProcessModel&);
template<> void JSONWriter::write(Emulation::Psg::ProcessModel&);
