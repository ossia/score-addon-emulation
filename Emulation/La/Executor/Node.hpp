// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Emulation/La/Engine.hpp>
#include <ossia/dataflow/graph_node.hpp>
#include <ossia/dataflow/port.hpp>
#include <score_addon_emulation_export.h>
#include <array>
#include <atomic>
#include <memory>
namespace Emulation::La::Executor
{
class SCORE_ADDON_EMULATION_EXPORT Node final : public ossia::graph_node
{
public:
  explicit Node(const ossia::execution_state&);
  std::string label() const noexcept override { return "LAdrenaline"; }
  void run(const ossia::token_request&, ossia::exec_state_facade) noexcept override;
  void all_notes_off() noexcept override;
  void suspend() noexcept;
  void install(std::shared_ptr<Engine>&) noexcept;
  void edit(const Patch&) noexcept;
  bool ready() const noexcept { return m_ready.load(std::memory_order_acquire); }
  const char* error() const noexcept { return m_error.load(std::memory_order_acquire); }
  ossia::midi_inlet* midi{};
  std::array<ossia::value_inlet*, ControlCount> controls{};
  ossia::audio_outlet* audio{};
private:
  struct Event
  {
    int64_t time{};
    std::size_t order{};
    int control{-1};
    const ossia::value* value{};
    const libremidi::ump* midi{};
  };
  void silence() noexcept;
  void applyControl(int, const ossia::value&) noexcept;
  void applyMidi(const libremidi::ump&, bool) noexcept;
  void report(const char* e) noexcept { m_error.store(e, std::memory_order_release); }
  const ossia::execution_state& m_state;
  std::shared_ptr<Engine> m_engine;
  Patch m_patch{defaultPatch()};
  // Subsequent ROM references are retained by Component's queued snapshots;
  // retain the construction-time reference too, so its last release is off audio.
  std::shared_ptr<const RomReferences> m_initialROMs{m_patch.roms};
  std::array<Event, 8192> m_events{};
  std::array<uint8_t, 65536> m_sysex{};
  std::size_t m_sysexSize{};
  int64_t m_tick{-1}, m_consumedUntil{}, m_eventsBefore{};
  bool m_pausedConsumed{};
  std::atomic<bool> m_ready{};
  std::atomic<const char*> m_error{};
};
}
