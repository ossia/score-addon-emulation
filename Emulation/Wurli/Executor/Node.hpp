// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Emulation/Wurli/Parameters.hpp>
#include <ossia/dataflow/graph_node.hpp>
#include <ossia/dataflow/port.hpp>
#include <score_addon_emulation_export.h>
#include <array>
#include <atomic>
#include <memory>
namespace Emulation::Wurli::Executor
{
class SCORE_ADDON_EMULATION_EXPORT Node final : public ossia::graph_node
{
public:
  explicit Node(const ossia::execution_state&, Settings = {});
  std::string label() const noexcept override { return "Wurlie"; }
  void run(const ossia::token_request&, ossia::exec_state_facade) noexcept override;
  void all_notes_off() noexcept override;
  void update(Settings) noexcept;
  void install(std::shared_ptr<Engine>&) noexcept;
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
  void silence() noexcept;
  void reportError() noexcept;
  const char* m_controlError{};
  const char* m_midiError{};
  const char* m_eventError{};
  void applyControl(int, const ossia::value&) noexcept;
  void applyMidi(const libremidi::ump&, bool paused) noexcept;
  std::shared_ptr<Engine> m_engine;
  Settings m_settings;
  std::atomic<bool> m_ready{};
  std::array<Event, 8192> m_events{};
  int64_t m_tick{-1}, m_consumedUntil{}, m_eventsBefore{};
  bool m_pausedConsumed{};
  const ossia::execution_state& m_state;
  std::atomic<const char*> m_error{};
};
}
