// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Emulation/Wavetable/Parameters.hpp>
#include <ossia/dataflow/graph_node.hpp>
#include <ossia/dataflow/port.hpp>
#include <score_addon_emulation_export.h>
#include <atomic>
#include <memory>
namespace Emulation::Wavetable::Executor
{
class SCORE_ADDON_EMULATION_EXPORT Node final : public ossia::graph_node
{
public:
  explicit Node(const ossia::execution_state&, Settings = {}, LevelMaxima = normalizedLevels);
  std::string label() const noexcept override { return "SiCC"; }
  void run(const ossia::token_request&, ossia::exec_state_facade) noexcept override;
  void all_notes_off() noexcept override;
  void update(Settings) noexcept;
  //! A snapshot of the document. A wave the document did not change keeps
  //! what execution last delivered to its inlet.
  void modelChanged(Settings) noexcept;
  //! Wave RAM last received through a channel's wave inlet, in [0, 1].
  const Waveform::ReceivedWave<32>& receivedWave(int channel) const noexcept
  {
    return m_receivedWaves[channel];
  }
  void install(std::shared_ptr<Engine>&, Model, int clock) noexcept;
  bool takeConfiguration(Model&, int& clock) noexcept;
  const char* error() const noexcept { return m_error.load(std::memory_order_acquire); }
  bool ready() const noexcept { return m_ready.load(std::memory_order_acquire); }
  ossia::midi_inlet* midi{};
  std::array<ossia::value_inlet*, ControlCount> controls{};
  ossia::audio_outlet* audio{};
private:
  struct Event {
    int64_t time{};
    std::size_t order{};
    int control{-1};
    const ossia::value* value{};
    const libremidi::ump* midi{};
  };
  void report(const char* e) noexcept { m_error.store(e, std::memory_order_release); }
  void silence() noexcept;
  void applyControl(int, const ossia::value&) noexcept;
  void applyMidi(const libremidi::ump&, bool paused) noexcept;
  void requestConfiguration() noexcept;
  std::shared_ptr<Engine> m_engine;
  Settings m_settings;
  std::array<std::array<double, 32>, 6> m_modelWaves{};
  std::array<Waveform::ReceivedWave<32>, 6> m_receivedWaves{};
  LevelMaxima m_levelScales;
  std::atomic<bool> m_ready{};
  std::array<Event, 8192> m_events{};
  int64_t m_tick{-1}, m_consumedUntil{}, m_eventsBefore{};
  bool m_pausedConsumed{};
  const ossia::execution_state& m_state;
  std::atomic<const char*> m_error{};
  std::atomic<uint64_t> m_configurationRequest{};
};
}
