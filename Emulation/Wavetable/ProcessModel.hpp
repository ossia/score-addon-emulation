#pragma once
#include <Process/Process.hpp>
#include <Emulation/Wavetable/Parameters.hpp>
#include <Emulation/Wavetable/ProcessMetadata.hpp>
#include <score_addon_emulation_export.h>
#include <verdigris>
namespace Emulation::Wavetable {
class SCORE_ADDON_EMULATION_EXPORT ProcessModel final : public Process::ProcessModel {
  SCORE_SERIALIZE_FRIENDS
  PROCESS_METADATA_IMPL(Emulation::Wavetable::ProcessModel)
  W_OBJECT(ProcessModel)
public:
  explicit ProcessModel(const TimeVal&, const QString&, const Id<Process::ProcessModel>&, QObject*);
  template<typename Impl> explicit ProcessModel(Impl& vis, QObject* parent)
    : Process::ProcessModel{vis, parent} { vis.writeTo(*this); wireControls(); }
  ~ProcessModel() override;
  QString effect() const noexcept override;
  Settings settings() const;
  LevelMaxima levelMaxima() const;
  void loadPreset(const Process::Preset&) override;
  Process::Preset savePreset() const noexcept override;
  QString error() const { return m_error; }
  void setEngineError(QString);
  void settingsChanged() E_SIGNAL(SCORE_ADDON_EMULATION_EXPORT, settingsChanged)
  void errorChanged() E_SIGNAL(SCORE_ADDON_EMULATION_EXPORT, errorChanged)
private:
  void wireControls();
  void updateWaveformLevels();
  void validatePorts() const;
  QString m_error;
  bool m_loadingPreset{};
};
}
template<> void DataStreamReader::read(const Emulation::Wavetable::ProcessModel&);
template<> void DataStreamWriter::write(Emulation::Wavetable::ProcessModel&);
template<> void JSONReader::read(const Emulation::Wavetable::ProcessModel&);
template<> void JSONWriter::write(Emulation::Wavetable::ProcessModel&);
