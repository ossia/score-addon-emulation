// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Emulation/Opl/Controls.hpp>
#include <Emulation/Opl/Allocation.hpp>
#include <Emulation/Opl/Engine.hpp>
#include <ossia/dataflow/graph_node.hpp>
#include <ossia/dataflow/port.hpp>
#include <score_addon_emulation_export.h>
#include <array>
#include <atomic>
#include <memory>

namespace Emulation::Opl::Executor
{
class SCORE_ADDON_EMULATION_EXPORT Node final : public ossia::graph_node
{
public:
  explicit Node(const ossia::execution_state&);
  std::string label() const noexcept override { return "Opale"; }
  void run(const ossia::token_request&, ossia::exec_state_facade) noexcept override;
  void all_notes_off() noexcept override;
  void suspend() noexcept;
  void install(std::shared_ptr<Engine>&, std::shared_ptr<const Bank>&,
               Settings, Address) noexcept;
  void edit(Address, const ADL_Instrument&) noexcept;
  void select(Address) noexcept;
  void depths(bool vibrato, bool tremolo) noexcept;
  void setVolume(double) noexcept;
  void setAllocation(int) noexcept;

  bool ready() const noexcept { return m_ready.load(std::memory_order_acquire); }
  const char* error() const noexcept { return m_error.load(std::memory_order_acquire); }
  bool takeConfiguration(Settings&) noexcept;
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
  void applyControl(int, const ossia::value&) noexcept;
  void applyMidi(const libremidi::ump&, bool paused) noexcept;
  void shortMidi(std::span<const uint8_t>, bool paused) noexcept;
  void requestConfiguration() noexcept;
  void configure(Settings) noexcept;
  void silence() noexcept;
  void report(const char*) noexcept;

  std::shared_ptr<Engine> m_engine;
  std::shared_ptr<const Bank> m_bank;
  Settings m_settings;
  Address m_selected;
  ADL_Instrument m_instrument{};
  double m_volume{1.};
  Allocation m_allocation;
  std::array<bool, 16> m_blockedPart{};
  std::array<Event, 8192> m_events{};
  std::array<uint8_t, 65536> m_sysex{};
  std::size_t m_sysexSize{};
  bool m_sysexActive{};
  int64_t m_tick{-1};
  int64_t m_consumedUntil{};
  int64_t m_eventsBefore{};
  bool m_wasPaused{};
  bool m_pausedConsumed{};
  const ossia::execution_state& m_state;
  std::atomic<bool> m_ready{};
  std::atomic<const char*> m_error{};
  std::atomic<uint64_t> m_configurationRequest{};
};
}
