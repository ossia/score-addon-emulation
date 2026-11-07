// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Emulation/Apu/Parameters.hpp>
#include <ossia/dataflow/graph_node.hpp>
#include <ossia/dataflow/port.hpp>
#include <score_addon_emulation_export.h>
#include <atomic>
#include <memory>
namespace Emulation::Apu::Executor
{
class SCORE_ADDON_EMULATION_EXPORT Node final : public ossia::graph_node
{
public:
  explicit Node(const ossia::execution_state&, Settings = {});
  std::string label() const noexcept override { return "Consolated"; }
  void run(const ossia::token_request&, ossia::exec_state_facade) noexcept override;
  void all_notes_off() noexcept override;
  void update(Settings) noexcept;
  //! A snapshot of the document. A wave the document did not change keeps
  //! what execution last delivered to the wave inlet.
  void modelChanged(Settings) noexcept;
  //! Wave RAM last received through the wave inlet, in [0, 15].
  const Waveform::ReceivedWave<32>& receivedWave() const noexcept { return m_receivedWave; }
  void install(std::shared_ptr<Engine>&, Model, int clock, uint64_t revision = 0) noexcept;
  void assetRevision(uint64_t revision) noexcept { m_revision = revision; requestConfiguration(); }
  bool takeConfiguration(Model&, int& clock) noexcept;
  const char* error() const noexcept {
    if(auto e=m_error.load(std::memory_order_acquire))return e;
    if(auto e=m_controlError.load(std::memory_order_acquire))return e;
    return m_midiError.load(std::memory_order_acquire);
  }
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
  void reportMidi(const char* e) noexcept { m_midiError.store(e,std::memory_order_release); }
  void reportControl(const char* e) noexcept { m_controlError.store(e,std::memory_order_release); }
  void silence() noexcept;
  void applyControl(int, const ossia::value&) noexcept;
  void applyMidi(const libremidi::ump&, bool paused) noexcept;
  void requestConfiguration() noexcept;
  std::shared_ptr<Engine> m_engine;
  Settings m_settings;
  std::array<uint8_t, 32> m_modelWave{};
  Waveform::ReceivedWave<32> m_receivedWave;
  uint64_t m_revision{}, m_installedRevision{};
  std::atomic<bool> m_ready{};
  std::array<Event, 8192> m_events{};
  int64_t m_tick{-1}, m_consumedUntil{}, m_eventsBefore{};
  bool m_pausedConsumed{};
  const ossia::execution_state& m_state;
  std::atomic<const char*> m_error{}, m_midiError{}, m_controlError{};
  std::atomic<uint64_t> m_configurationRequest{};
};
}
