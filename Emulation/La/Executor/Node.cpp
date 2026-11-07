// SPDX-License-Identifier: GPL-3.0-or-later
#include "Node.hpp"
#include <ossia/dataflow/execution_state.hpp>
#include <libremidi/detail/conversion.hpp>
#include <algorithm>
#include <limits>
namespace Emulation::La::Executor
{
Node::Node(const ossia::execution_state& state) : m_state{state}
{
  m_inlets.push_back(midi = new ossia::midi_inlet);
  for(auto& inlet : controls) m_inlets.push_back(inlet = new ossia::value_inlet);
  m_outlets.push_back(audio = new ossia::audio_outlet);
  audio->data.set_channels(2);
  for(auto& channel : audio->data.get()) channel.resize(state.bufferSize);
}
void Node::silence() noexcept { m_sysexSize = 0; if(m_engine) m_engine->panic(); }
void Node::all_notes_off() noexcept { silence(); m_tick = -1; }
void Node::suspend() noexcept { silence(); m_ready.store(false, std::memory_order_release); }
void Node::install(std::shared_ptr<Engine>& engine) noexcept
{
  // Reject obsolete preparations without replacing the current instance. The captured
  // engine retires with the command on score's UI-thread execution garbage queue.
  if(engine && !sameSetup(m_patch, engine->patch())) return;
  m_engine.swap(engine);
  m_sysexSize = 0;
  const bool ok = m_engine && m_engine->setPatch(m_patch);
  report(m_engine ? m_engine->lastError() : nullptr);
  m_ready.store(ok, std::memory_order_release);
}
void Node::edit(const Patch& patch) noexcept
{
  if(const char* error = validate(patch)) { report(error); return; }
  m_patch = patch;
  if(m_engine)
  {
    if(!sameSetup(patch, m_engine->patch())) suspend();
    else
    {
      const bool ok = m_engine->setPatch(patch);
      report(m_engine->lastError());
      m_ready.store(ok, std::memory_order_release);
    }
  }
}
void Node::applyControl(int c, const ossia::value& value) noexcept
{
  const auto number = ossia::convert<double>(value);
  if(m_engine && ready())
  {
    if(!m_engine->control(c, number)) { report(m_engine->lastError()); return; }
  }
  if(!setControl(m_patch, c, number)) { report("Invalid LA automation value."); return; }
  if(m_engine && sameSetup(m_patch, m_engine->patch()))
  {
    if(!ready()) m_ready.store(m_engine->setPatch(m_patch), std::memory_order_release);
    report(m_engine->lastError());
  }
  else report(nullptr);
}
void Node::applyMidi(const libremidi::ump& message, bool paused) noexcept
{
  if(message.get_group() != 0) { report("LA accepts one MIDI port (UMP group 0)."); return; }
  if(message.get_type() == libremidi::midi2::message_type::SYSEX8_MDS)
  { report("LA hardware accepts MIDI 1 SysEx7, not SysEx8 or mixed data sets."); return; }
  if(!m_engine) { report(nullptr); return; }
  if(!ready()) { report(m_engine->lastError()); return; }
  if(message.get_type() == libremidi::midi2::message_type::SYSEX7)
  {
    if(paused) { m_sysexSize = 0; return; }
    const unsigned status = (message.data[0] >> 20) & 15;
    const unsigned count = (message.data[0] >> 16) & 15;
    if(status > 3 || count > 6) { m_sysexSize = 0; report("Malformed LA UMP SysEx7 packet."); return; }
    if(status == 0 || status == 1) { m_sysex[0] = 0xf0; m_sysexSize = 1; }
    else if(!m_sysexSize) { report("LA SysEx continuation without a start packet."); return; }
    if(m_sysexSize + count + 1 > m_sysex.size())
    { m_sysexSize = 0; report("LA SysEx exceeds the 65536-byte message limit; split the bank into native DT1 messages."); return; }
    const uint8_t bytes[] = {uint8_t(message.data[0] >> 8),uint8_t(message.data[0]),
      uint8_t(message.data[1] >> 24),uint8_t(message.data[1] >> 16),uint8_t(message.data[1] >> 8),uint8_t(message.data[1])};
    for(unsigned i = 0; i < count; ++i)
    {
      if(bytes[i] >= 128) { m_sysexSize = 0; report("Non-seven-bit LA SysEx data."); return; }
      m_sysex[m_sysexSize++] = bytes[i];
    }
    if(status == 0 || status == 3)
    { m_sysex[m_sysexSize++] = 0xf7; m_engine->midi({m_sysex.data(), m_sysexSize}); m_sysexSize = 0; }
  }
  else
  {
    uint8_t bytes[32]{};
    auto copy = message;
    const auto count = cmidi2_convert_single_ump_to_midi1(bytes, sizeof(bytes), copy.data);
    for(std::size_t offset = 0; offset < count;)
    {
      const uint8_t status = bytes[offset];
      const auto kind = status & 0xf0;
      const std::size_t size = status >= 0xf8 ? 1 : (kind == 0xc0 || kind == 0xd0 ? 2 : 3);
      if(status < 0x80 || offset + size > count) break;
      if(!paused || status == 0xff || kind == 0x80 || (kind == 0x90 && bytes[offset + 2] == 0)
          || (kind == 0xb0 && (bytes[offset + 1] == 64 || bytes[offset + 1] >= 120)))
        m_engine->midi({bytes + offset, size});
      offset += size;
    }
  }
  report(m_engine->lastError());
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
  for(int c = Volume; c < ControlCount && !overflow; ++c)
    for(const auto& v : controls[c]->data.get_data())
      if(!append({v.timestamp, 0, c, &v.value, nullptr})) { overflow = true; break; }
  for(const auto& m : midi->data.messages)
    if(!append({m.timestamp, 0, -1, nullptr, &m})) { overflow = true; break; }
  m_consumedUntil = std::max<int64_t>(m_consumedUntil, end);
  m_eventsBefore = std::max<int64_t>(m_eventsBefore, paused ? begin + 1 : end);
  if(overflow) { silence(); report("More than 8192 LA events in one token; voices stopped and token silenced."); return; }
  std::sort(m_events.begin(), m_events.begin() + count, [](const Event& a, const Event& b) {
    if(a.time != b.time) return a.time < b.time;
    const auto isBank = [](int c) {
      return c >= PartBase && c < EditTimbre && (c - PartBase) % PartFieldCount == Bank;
    };
    // A timestamp is one automation selection: restore its destination bank
    // before applying Program, independently of how producers queued events.
    // Standalone Program events still edit only the currently active bank.
    const bool aBank = isBank(a.control), bBank = isBank(b.control);
    if(aBank != bBank) return aBank;
    return a.order < b.order;
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
}
}
