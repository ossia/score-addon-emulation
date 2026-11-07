// SPDX-License-Identifier: GPL-3.0-or-later
#include "Node.hpp"
#include <ossia/dataflow/execution_state.hpp>
#include <libremidi/detail/conversion.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
namespace Emulation::Apu::Executor
{
Node::Node(const ossia::execution_state& state, Settings settings)
    : m_settings{settings}, m_modelWave{settings.wave}, m_state{state}
{
  m_inlets.push_back(midi = new ossia::midi_inlet);
  for(auto& inlet : controls) m_inlets.push_back(inlet = new ossia::value_inlet);
  // An address delivers each new wave once, not a copy of the current list on
  // every cycle.
  controls[Wave]->data.is_event=true;
  m_outlets.push_back(audio = new ossia::audio_outlet);
  audio->data.set_channels(2);
  for(auto& c : audio->data.get()) c.resize(state.bufferSize);
}
void Node::silence() noexcept
{
  if(m_engine) {
    m_engine->panic();
    if(ready()) report(nullptr);
    reportMidi(nullptr);
  }
}
void Node::all_notes_off() noexcept { silence(); m_tick = -1; }
void Node::requestConfiguration() noexcept
{
  silence(); m_ready.store(false, std::memory_order_release);
  const auto packed = uint64_t(m_settings.clock) << 3 | uint64_t(m_settings.model) << 1 | 1;
  m_configurationRequest.store(packed, std::memory_order_release);
}
bool Node::takeConfiguration(Model& model, int& clock) noexcept
{
  const auto packed = m_configurationRequest.exchange(0, std::memory_order_acq_rel);
  if(!packed) return false;
  model = Model((packed >> 1) & 3); clock = int(packed >> 3); return true;
}
void Node::update(Settings settings) noexcept
{
  if(auto e = validate(settings)) {
    reportControl(e); return;
  }
  reportControl(nullptr);
  const bool setup = settings.model != m_settings.model || effectiveClock(settings) != effectiveClock(m_settings);
  m_settings = settings;
  if(setup || !m_engine || m_installedRevision != m_revision) { requestConfiguration(); return; }
  if(m_engine->configure(settings)) { m_ready.store(true, std::memory_order_release); report(nullptr); }
  else { requestConfiguration(); report(m_engine->error()); }
}
void Node::modelChanged(Settings settings) noexcept
{
  if(settings.wave == m_modelWave) settings.wave = m_settings.wave;
  else m_modelWave = settings.wave;
  update(settings);
}
void Node::install(std::shared_ptr<Engine>& engine, Model model, int clock, uint64_t revision) noexcept
{
  // Stale setup replies never overwrite newer automation. Captured engines are
  // retired by score's execution queue on the control thread after this swap.
  if(model != m_settings.model || clock != m_settings.clock || revision != m_revision) return;
  m_engine.swap(engine);
  const bool success = m_engine && m_engine->configure(m_settings);
  if(success) {
    m_installedRevision = revision;
    reportMidi(nullptr);
    m_configurationRequest.store(0, std::memory_order_release);
  }
  m_ready.store(success, std::memory_order_release);
  report(m_engine ? m_engine->error() : "APU engine preparation failed.");
}
void Node::applyControl(int c, const ossia::value& value) noexcept
{
  if(c == DmcFile) {
    if(const auto* path=value.target<std::string>(); path && !path->empty())
      reportControl("DMC import is a document/control-thread operation, not an audio automation command.");
    return;
  }
  auto settings = m_settings;
  if(!setControl(settings, c, value)) { reportControl("Invalid APU control value."); return; }
  if(c == Wave) m_receivedWave.publish(settings.wave);
  if(settings == m_settings) {reportControl(nullptr);return;}
  update(settings);
}

void Node::applyMidi(const libremidi::ump& message, bool paused) noexcept
{
  if(message.get_group() != 0) { reportMidi("APU accepts one MIDI port: UMP group 0."); return; }
  if(message.get_type() == libremidi::midi2::message_type::SYSEX7
      || message.get_type() == libremidi::midi2::message_type::SYSEX8_MDS) {
    reportMidi("APU has no SysEx patch format."); return;
  }
  uint8_t bytes[32]{};
  auto copy = message;
  const auto count = cmidi2_convert_single_ump_to_midi1(bytes, sizeof(bytes), copy.data);
  if(count==0) {reportMidi("APU could not decode the MIDI message.");return;}
  for(std::size_t offset = 0; offset < count;) {
    const int status = bytes[offset], kind = status & 0xf0;
    const std::size_t length = status >= 0xf8 ? 1 : (kind == 0xc0 || kind == 0xd0 ? 2 : 3);
    if(status < 0x80 || offset + length > count) {reportMidi("Malformed APU MIDI message.");break;}
    const bool cleanup = status == 0xff || kind == 0x80 || (kind == 0x90 && bytes[offset + 2] == 0)
        || (kind == 0xb0 && (bytes[offset + 1] == 64 || bytes[offset + 1] >= 120));
    if(m_engine && (ready() || cleanup) && (!paused || cleanup)) {
      m_engine->midi({bytes + offset, length});
      reportMidi(m_engine->midiError());
    }
    offset += length;
  }
}
void Node::run(const ossia::token_request& tk, ossia::exec_state_facade estate) noexcept
{
  const auto timing = estate.timings(tk);
  int64_t begin = timing.start_sample;
  const auto end = begin + timing.length;
  if(begin < 0 || end < begin) return;
  const bool firstWindow = m_tick != m_state.samples_since_start;
  if(firstWindow) {
    m_tick = m_state.samples_since_start;
    m_consumedUntil = m_eventsBefore = std::numeric_limits<int64_t>::min();
    m_pausedConsumed = false;
  } else if(timing.length > 0) {
    if(end <= m_consumedUntil) return;
    begin = std::max(begin, m_consumedUntil);
  }
  // The graph recycles channels; their previous size and capacity are not retained.
  audio->data.set_channels(2);
  // Consumers require a complete host buffer even for the first loop slice.
  const auto frames = std::size_t(std::max<int64_t>(m_state.bufferSize, end));
  for(auto& c : audio->data.get()) {
    if(c.size() != frames) c.resize(frames);
    if(firstWindow) std::fill(c.begin(), c.end(), 0.);
    else std::fill(c.begin() + begin, c.begin() + end, 0.);
  }
  const bool paused = !tk.forward() || timing.length == 0;
  if(paused && m_pausedConsumed) return;
  if(tk.start_discontinuous && begin == timing.start_sample) silence();
  if(paused) { m_pausedConsumed = true; silence(); }
  std::size_t count{};
  const auto append = [&](Event event) {
    if(event.time < m_eventsBefore || (paused ? event.time > begin : event.time >= end)) return true;
    if(count == m_events.size()) return false;
    event.time = std::max<int64_t>(event.time, begin); event.order = count;
    m_events[count++] = event; return true;
  };
  bool overflow{};
  for(int c = 0; c < ControlCount && !overflow; ++c) {
    for(const auto& v : controls[c]->data.get_data())
      if(!append({v.timestamp, 0, c, &v.value, nullptr})) { overflow = true; break; }
  }
  for(const auto& msg : midi->data.messages)
    if(!append({msg.timestamp, 0, -1, nullptr, &msg})) { overflow = true; break; }
  m_consumedUntil = std::max<int64_t>(m_consumedUntil, end);
  m_eventsBefore = std::max<int64_t>(m_eventsBefore, paused ? begin + 1 : end);
  if(overflow) { silence(); report("More than 8192 APU events in one token; voices stopped and buffer silenced."); return; }
  std::sort(m_events.begin(), m_events.begin() + count, [](const Event& a, const Event& b) {
    return a.time < b.time || (a.time == b.time && a.order < b.order);
  });
  auto cursor = begin;
  const auto render = [&](int64_t until) {
    if(m_engine && ready() && until > cursor) {
      m_engine->render(audio->data.channel(0).data() + cursor, audio->data.channel(1).data() + cursor, until - cursor);
      const auto error = m_engine->error();
      // MIDI rejection has its own diagnostic slot; publish render recovery
      // as well as failure without promoting a MIDI error into that slot.
      report(error == m_engine->midiError() ? nullptr : error);
    }
    cursor = until;
  };
  for(std::size_t i = 0; i < count; ++i) {
    const auto& event = m_events[i];
    if(!paused) render(event.time);
    if(event.control >= 0) applyControl(event.control, *event.value);
    else applyMidi(*event.midi, paused);
  }
  if(!paused) render(end);
  if(tk.end_discontinuous) silence();
}
}
