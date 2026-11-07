// SPDX-License-Identifier: GPL-3.0-or-later
#include "Node.hpp"
#include <ossia/dataflow/execution_state.hpp>
#include <libremidi/detail/conversion.hpp>
#include <algorithm>
#include <limits>
namespace Emulation::Sid::Executor
{
Node::Node(const ossia::execution_state& state) : m_state{state}
{
  m_inlets.push_back(midi = new ossia::midi_inlet);
  for(auto& inlet : controls) m_inlets.push_back(inlet = new ossia::value_inlet);
  m_outlets.push_back(audio = new ossia::audio_outlet);
  audio->data.set_channels(2);
  for(auto& channel : audio->data.get()) channel.resize(state.bufferSize);
}
void Node::reportError() noexcept
{
  const auto* engineError = m_engine ? m_engine->lastError() : nullptr;
  m_error.store(m_eventError ? m_eventError : engineError ? engineError
      : m_controlError ? m_controlError : m_midiError, std::memory_order_release);
}
void Node::silence() noexcept { if(m_engine) m_engine->panic(); reportError(); }
void Node::all_notes_off() noexcept { silence(); m_tick = -1; }
void Node::suspend() noexcept { silence(); m_ready.store(false, std::memory_order_release); }
void Node::install(std::shared_ptr<Engine>& engine) noexcept
{
  // Captured old engine is retired with the execution command on the UI thread.
  if(engine && !sameSetup(m_patch, engine->patch())) return;
  m_engine.swap(engine);
  if(!m_engine || !sameSetup(m_patch, m_engine->patch()))
  { m_ready.store(false, std::memory_order_release); reportError(); return; }
  m_engine->setPatch(m_patch);
  reportError();
  m_ready.store(true, std::memory_order_release);
}
void Node::edit(const Patch& p) noexcept
{
  if(const char* e = validate(p)) { m_controlError = e; reportError(); return; }
  m_controlError = nullptr;
  m_patch = p;
  if(m_engine && ready())
  {
    if(!sameSetup(p, m_engine->patch())) suspend();
    else m_engine->setPatch(p);
  }
  reportError();
}
bool Node::takeConfiguration(Patch& p) noexcept
{
  const auto bits = m_configuration.exchange(0, std::memory_order_acq_rel);
  if(!bits) return false;
  p.model = Model((bits >> 1) & 1);
  p.clock = Clock((bits >> 2) & 1);
  p.filterCurve = (bits >> 3) & 127;
  p.combinedStrength = (bits >> 10) & 3;
  return true;
}
void Node::applyControl(int c, const ossia::value& value) noexcept
{
  auto next = m_patch;
  if(!setControl(next, c, ossia::convert<double>(value))) { m_controlError = "Invalid SID automation value."; reportError(); return; }
  if(const char* e = validate(next)) { m_controlError = e; reportError(); return; }
  m_controlError = nullptr;
  if(next == m_patch) { reportError(); return; }
  const bool setup = !sameSetup(next, m_patch);
  edit(next);
  if(setup)
  {
    suspend();
    m_configuration.store(1u | (uint32_t(next.model) << 1) | (uint32_t(next.clock) << 2)
        | (uint32_t(next.filterCurve) << 3) | (uint32_t(next.combinedStrength) << 10), std::memory_order_release);
  }
}
void Node::applyMidi(const libremidi::ump& message, bool paused) noexcept
{
  if(message.get_group() != 0) { m_midiError = "SID accepts one MIDI port (UMP group 0)."; reportError(); return; }
  if(message.get_type() == libremidi::midi2::message_type::SYSEX7
      || message.get_type() == libremidi::midi2::message_type::SYSEX8_MDS)
  { m_midiError = "SID SysEx and GoatTracker macro imports are unsupported."; reportError(); return; }
  if(!m_engine || !ready()) return;
  uint8_t bytes[32]{};
  auto copy = message;
  const auto count = cmidi2_convert_single_ump_to_midi1(bytes, sizeof(bytes), copy.data);
  for(std::size_t offset = 0; offset < count;)
  {
    const uint8_t status = bytes[offset];
    const auto kind = status & 0xf0;
    const std::size_t size = status >= 0xf8 ? 1 : (kind == 0xc0 || kind == 0xd0 ? 2 : 3);
    if(status < 0x80 || offset + size > count) break;
    if(!paused || status == 0xff || kind == 0x80
        || (kind == 0x90 && bytes[offset + 2] == 0)
        || (kind == 0xb0 && (bytes[offset + 1] == 64 || bytes[offset + 1] >= 120)))
    {
      m_midiError = nullptr;
      m_engine->midi({bytes + offset, size});
      reportError();
    }
    offset += size;
  }
}
void Node::run(const ossia::token_request& tk, ossia::exec_state_facade estate) noexcept
{
  const auto timing = estate.timings(tk);
  int64_t begin = timing.start_sample;
  const auto end = begin + timing.length;
  if(begin < 0 || end < begin) return;
  const bool firstWindow = m_tick != m_state.samples_since_start;
  if(firstWindow)
  {
    m_tick = m_state.samples_since_start;
    m_consumedUntil = m_eventsBefore = std::numeric_limits<int64_t>::min();
    m_pausedConsumed = false;
  }
  else if(timing.length > 0)
  {
    if(end <= m_consumedUntil) return;
    begin = std::max(begin, m_consumedUntil);
  }
  // The graph recycles channels; their previous size and capacity are not retained.
  audio->data.set_channels(2);
  // Consumers require a complete host buffer even for the first loop slice.
  const auto frames = std::size_t(std::max<int64_t>(m_state.bufferSize, end));
  for(auto& channel : audio->data.get())
  {
    if(channel.size() != frames) channel.resize(frames);
    if(firstWindow) std::fill(channel.begin(), channel.end(), 0.);
    else std::fill(channel.begin() + begin, channel.begin() + end, 0.);
  }
  const bool paused = !tk.forward() || timing.length == 0;
  if(paused && m_pausedConsumed) return;
  if(tk.start_discontinuous && begin == timing.start_sample) silence();
  if(paused) { m_pausedConsumed = true; silence(); }
  std::size_t count{};
  auto append = [&](Event event) {
    if(event.time < m_eventsBefore || (paused ? event.time > begin : event.time >= end)) return true;
    if(count == m_events.size()) return false;
    event.time = std::max<int64_t>(event.time, begin);
    event.order = count;
    m_events[count++] = event;
    return true;
  };
  bool overflow{};
  for(int c = 0; c < ControlCount && !overflow; ++c)
    for(const auto& v : controls[c]->data.get_data())
      if(!append({v.timestamp, 0, c, &v.value, nullptr})) { overflow = true; break; }
  for(const auto& m : midi->data.messages)
    if(!append({m.timestamp, 0, -1, nullptr, &m})) { overflow = true; break; }
  m_consumedUntil = std::max<int64_t>(m_consumedUntil, end);
  m_eventsBefore = std::max<int64_t>(m_eventsBefore, paused ? begin + 1 : end);
  if(overflow) { m_eventError = "More than 8192 SID events in one token; voices stopped and token silenced."; silence(); return; }
  m_eventError = nullptr;
  std::sort(m_events.begin(), m_events.begin() + count, [](const Event& a, const Event& b) {
    return a.time < b.time || (a.time == b.time && a.order < b.order);
  });
  auto cursor = begin;
  const auto render = [&](int64_t until) {
    if(m_engine && ready() && until > cursor)
      m_engine->render(audio->data.channel(0).data() + cursor, audio->data.channel(1).data() + cursor, until - cursor);
    cursor = until;
  };
  for(std::size_t i = 0; i < count; ++i)
  {
    const auto& event = m_events[i];
    if(!paused) render(event.time);
    if(event.control >= 0) applyControl(event.control, *event.value);
    else applyMidi(*event.midi, paused);
  }
  if(!paused) render(end);
  if(tk.end_discontinuous) silence();
  reportError();
}
}
