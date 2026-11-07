// SPDX-License-Identifier: GPL-3.0-or-later
#include "Node.hpp"
#include <Emulation/Volume.hpp>
#include <ossia/dataflow/execution_state.hpp>
#include <libremidi/detail/conversion.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace Emulation::Opl::Executor
{
Node::Node(const ossia::execution_state& state) : m_state{state}
{
  m_inlets.push_back(midi = new ossia::midi_inlet);
  for(auto& inlet : controls)
    m_inlets.push_back(inlet = new ossia::value_inlet);
  m_outlets.push_back(audio = new ossia::audio_outlet);
  audio->data.set_channels(2);
  for(auto& channel : audio->data.get())
    channel.resize(state.bufferSize);
}
void Node::report(const char* text) noexcept
{ m_error.store(text, std::memory_order_release); }
void Node::all_notes_off() noexcept
{
  silence();
  m_tick = -1;
  m_wasPaused = true;
}
void Node::silence() noexcept
{
  m_allocation.stop([&](std::span<const uint8_t> bytes) {
    if(m_engine) m_engine->midi(bytes);
  });
  if(m_engine) m_engine->panic();
  m_sysexSize = 0;
  m_sysexActive = false;
}
void Node::suspend() noexcept
{
  silence();
  m_ready.store(false, std::memory_order_release);
}
void Node::install(std::shared_ptr<Engine>& engine, std::shared_ptr<const Bank>& bank,
                   Settings settings, Address selected) noexcept
{
  // in_exec retires its captured arguments on the UI thread after this swap.
  silence();
  m_allocation.resetPedals();
  m_engine.swap(engine);
  m_bank.swap(bank);
  m_settings = settings;
  m_blockedPart.fill(false);
  m_sysexSize = 0;
  m_sysexActive = false;
  report(nullptr);
  m_ready.store(bool(m_engine), std::memory_order_release);
  select(selected);
}
void Node::select(Address address) noexcept
{
  m_selected = address;
  m_instrument = {};
  m_instrument.inst_flags = ADLMIDI_Ins_IsBlank;
  if(!m_engine || !ready()) return;
  const uint8_t channel = address.percussion ? 9 : 0;
  const auto block = [&] {
    m_blockedPart[channel] = true;
    const std::array<uint8_t, 3> off{uint8_t(0xB0 | channel), 120, 0};
    m_allocation.midi(off, [&](std::span<const uint8_t> bytes) { m_engine->midi(bytes); });
  };
  if((!address.percussion && address.lsb > 127) || (address.percussion && address.msb != 0))
  {
    block();
    report("Melodic banks use seven-bit MSB/LSB; drum banks use MSB 0 and kit LSB 0 to 255.");
    return;
  }
  if(!m_engine->getInstrument(address, m_instrument))
  {
    block();
    report("No patch at the selected bank/program address.");
    return;
  }
  // Editor selection targets the selected editing part, not all sixteen MIDI parts.
  m_blockedPart[channel] = false;
  report(nullptr);
  const std::array<uint8_t, 3> msb{uint8_t(0xB0 | channel), 0,
      address.percussion ? uint8_t(address.lsb >= 128 ? 0x7e : 0x7f) : address.msb};
  const std::array<uint8_t, 3> lsb{uint8_t(0xB0 | channel), 32, uint8_t(address.lsb & 127)};
  const std::array<uint8_t, 2> program{uint8_t(0xC0 | channel),
      address.percussion ? uint8_t(address.lsb & 127) : address.program};
  m_engine->midi(msb);
  if(!address.percussion) m_engine->midi(lsb);
  m_engine->midi(program);
}
void Node::edit(Address address, const ADL_Instrument& instrument) noexcept
{
  if(m_engine && ready())
  {
    m_engine->setInstrument(address, instrument);
    report(m_engine->lastError());
    if(!m_engine->lastError() && address == m_selected) m_instrument = instrument;
  }
}
void Node::depths(bool vibrato, bool tremolo) noexcept
{
  m_settings.deepVibrato = vibrato;
  m_settings.deepTremolo = tremolo;
  if(m_engine && ready()) m_engine->setDepths(vibrato, tremolo);
}
void Node::configure(Settings next) noexcept
{
  const auto* error = m_engine ? m_engine->configurationError(next) : settingsError(next);
  if(error) { report(error); return; }
  m_settings = next;
  report(nullptr);
  requestConfiguration();
}
void Node::requestConfiguration() noexcept
{
  const uint64_t packed = uint64_t(m_settings.model == Model::Opl3)
      | (uint64_t(m_settings.chips) << 1)
      | (uint64_t(m_settings.fourOpChannels) << 9)
      | (uint64_t(m_settings.deepVibrato) << 19)
      | (uint64_t(m_settings.deepTremolo) << 20)
      | (uint64_t(m_settings.softPan) << 21);
  m_configurationRequest.store((packed << 1) | 1, std::memory_order_release);
}
bool Node::takeConfiguration(Settings& out) noexcept
{
  const auto request = m_configurationRequest.exchange(0, std::memory_order_acq_rel);
  if(!request) return false;
  const auto value = request >> 1;
  out = {value & 1 ? Model::Opl3 : Model::Opl2, int((value >> 1) & 255),
      int((value >> 9) & 1023), bool((value >> 19) & 1),
      bool((value >> 20) & 1), bool((value >> 21) & 1)};
  return true;
}
void Node::setVolume(double volume) noexcept
{
  if(std::isfinite(volume) && volume >= 0. && volume <= MaxVolume) m_volume = volume;
  else report("Volume must be finite and in [0, 32].");
}
void Node::setAllocation(int value) noexcept
{
  if(value < 0 || value > 2) { report("Invalid voice allocation mode."); return; }
  m_allocation.setMode(static_cast<AllocationMode>(value),
      [&](std::span<const uint8_t> bytes) { if(m_engine) m_engine->midi(bytes); });
}
void Node::applyControl(int c, const ossia::value& value) noexcept
{
  if(c == Volume)
  {
    setVolume(ossia::convert<double>(value));
    return;
  }
  if(c == File)
  {
    report("WOPL imports are setup actions: use the File control in the editor.");
    return;
  }
  const int v = ossia::convert<int>(value);
  switch(c)
  {
    case VoiceAllocation: setAllocation(v); return;
    case ChipModel:
    {
      const auto model = v == 0 ? Model::Opl2 : Model::Opl3;
      if(model != m_settings.model) { auto next = m_settings; next.model = model; configure(next); }
      return;
    }
    case Chips:
    {
      const int chips = std::clamp(v, 1, 16);
      if(chips != m_settings.chips) { auto next = m_settings; next.chips = chips; configure(next); }
      return;
    }
    case FourOps:
    {
      const int channels = std::clamp(v, 0, 96);
      if(channels != m_settings.fourOpChannels) { auto next = m_settings; next.fourOpChannels = channels; configure(next); }
      return;
    }
    case SoftPan:
      if(bool(v) != m_settings.softPan) { auto next = m_settings; next.softPan = bool(v); configure(next); }
      return;
    case DeepVibrato: depths(bool(v), m_settings.deepTremolo); return;
    case DeepTremolo: depths(m_settings.deepVibrato, bool(v)); return;
    case BankMSB: { auto a = m_selected; a.msb = std::clamp(v, 0, 127); select(a); return; }
    case BankLSB: { auto a = m_selected; a.lsb = std::clamp(v, 0, 255); select(a); return; }
    case Percussion: { auto a = m_selected; a.percussion = bool(v); select(a); return; }
    case ProgramNumber: { auto a = m_selected; a.program = std::clamp(v, 0, 127); select(a); return; }
    case InstrumentIndex:
      if(const auto address = instrumentAddress(m_bank.get(), v))
        select(*address);
      else
        report("Instrument index is outside the loaded WOPL bank.");
      return;
    case RhythmMode:
      report("Rhythm allocation is a setup action: use the patch editor.");
      return;
    default: break;
  }
  if(isInstrumentControl(c))
  {
    auto next = m_instrument;
    setInstrumentControl(next, c, v);
    if(instrumentControlValue(next, c) == instrumentControlValue(m_instrument, c)) return;
    edit(m_selected, next);
  }
}
void Node::shortMidi(std::span<const uint8_t> bytes, bool paused) noexcept
{
  if(bytes.empty()) return;
  const auto status = bytes[0] & 0xf0;
  if(paused && !(status == 0x80 || (status == 0x90 && bytes.size() == 3 && bytes[2] == 0)
      || (status == 0xb0 && bytes.size() == 3
          && (bytes[1] == 64 || bytes[1] == 66 || bytes[1] >= 120))))
    return;
  const auto channel = bytes[0] & 15;
  // An explicit incoming MIDI program change resumes that part's independent
  // native MIDI selection, rather than the absent editor address.
  if(status == 0xC0) { m_blockedPart[channel] = false; report(nullptr); }
  m_allocation.midi(bytes, [&](std::span<const uint8_t> message) {
    if(m_engine && ready()) m_engine->midi(message);
  }, m_engine && ready() && !m_blockedPart[channel]);
  if(m_engine && ready()) report(m_engine->lastError());
}
void Node::applyMidi(const libremidi::ump& message, bool paused) noexcept
{
  if(message.get_group() != 0)
  {
    report("OPL accepts one MIDI port (UMP group 0).");
    return;
  }
  if(message.get_type() == libremidi::midi2::message_type::SYSEX8_MDS)
  {
    report("OPL accepts SysEx7, not SysEx8.");
    return;
  }
  if(message.get_type() == libremidi::midi2::message_type::SYSEX7)
  {
    if(paused) return;
    const auto status = (message.data[0] >> 20) & 15;
    const auto count = (message.data[0] >> 16) & 15;
    if(status > 3 || count > 6)
    {
      m_sysexActive = false;
      report("Invalid UMP SysEx7 packet.");
      return;
    }
    if(status == 0 || status == 1)
    {
      m_sysexSize = 1;
      m_sysex[0] = 0xf0;
      m_sysexActive = true;
    }
    if(!m_sysexActive)
    {
      report("SysEx7 continuation without a start packet.");
      return;
    }
    if(m_sysexSize + count + 1 > m_sysex.size())
    {
      m_sysexActive = false;
      report("SysEx exceeds the 64 KiB input limit.");
      return;
    }
    for(unsigned i = 0; i < count; ++i)
    {
      const unsigned byte = i + 2;
      const auto data = uint8_t(message.data[byte / 4] >> (24 - 8 * (byte % 4)));
      if(data > 127)
      {
        m_sysexActive = false;
        report("SysEx7 contains a non-seven-bit data byte.");
        return;
      }
      m_sysex[m_sysexSize++] = data;
    }
    if(status == 0 || status == 3)
    {
      m_sysex[m_sysexSize++] = 0xf7;
      shortMidi({m_sysex.data(), m_sysexSize}, false);
      m_sysexActive = false;
    }
    return;
  }
  // Conversion can emit bank-select + program or RPN controller sequences.
  uint8_t bytes[32]{};
  auto copy = message;
  const auto count = cmidi2_convert_single_ump_to_midi1(bytes, sizeof(bytes), copy.data);
  for(std::size_t offset = 0; offset < count;)
  {
    const uint8_t status = bytes[offset];
    const auto kind = status & 0xf0;
    const std::size_t length = status >= 0xf8 ? 1
        : (kind == 0xc0 || kind == 0xd0 ? 2 : 3);
    if(status < 0x80 || offset + length > count) break;
    shortMidi({bytes + offset, length}, paused);
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
  if(firstWindow)
  {
    m_tick = m_state.samples_since_start;
    m_consumedUntil = std::numeric_limits<int64_t>::min();
    m_eventsBefore = std::numeric_limits<int64_t>::min();
    m_pausedConsumed = false;
  }
  else if(timing.length > 0)
  {
    if(end <= m_consumedUntil) return;
    begin = std::max(begin, m_consumedUntil);
  }
  if(tk.start_discontinuous && begin == timing.start_sample) silence();
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
  if(paused && !m_wasPaused) silence();
  m_wasPaused = paused;
  if(paused && m_pausedConsumed) return;
  if(paused) m_pausedConsumed = true;

  std::size_t count{};
  auto append = [&](Event event) {
    if(event.time < m_eventsBefore || (paused ? event.time > begin : event.time >= end)) return true;
    if(count == m_events.size()) return false;
    event.time = std::max<int64_t>(event.time, begin);
    event.order = count;
    m_events[count++] = event;
    return true;
  };
  bool overflow = false;
  for(int c = 0; c < ControlCount && !overflow; ++c)
    for(const auto& v : controls[c]->data.get_data())
      if(!append({v.timestamp, 0, c, &v.value, nullptr})) { overflow = true; break; }
  for(const auto& m : midi->data.messages)
    if(!append({m.timestamp, 0, -1, nullptr, &m})) { overflow = true; break; }
  m_consumedUntil = std::max<int64_t>(m_consumedUntil, end);
  m_eventsBefore = std::max(m_eventsBefore, paused ? begin + 1 : end);
  if(overflow)
  {
    silence();
    report("More than 8192 events in one token: all voices stopped; buffer silenced.");
    return;
  }
  std::sort(m_events.begin(), m_events.begin() + count, [](const Event& a, const Event& b) {
    return a.time < b.time || (a.time == b.time && a.order < b.order);
  });
  auto cursor = begin;
  auto render = [&](int64_t until) {
    if(m_engine && ready() && until > cursor)
    {
      m_engine->render(audio->data.channel(0).data() + cursor,
                       audio->data.channel(1).data() + cursor, until - cursor);
      if(m_volume != 1.)
        for(auto& channel : audio->data.get())
          for(auto i = cursor; i < until; ++i) channel[i] *= m_volume;
      if(const auto* error = m_engine->lastError()) report(error);
    }
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
