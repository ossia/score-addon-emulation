// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <span>

namespace Emulation::Opl
{
// Musical selection only: libADLMIDI still owns every chip, operator and voice.
enum class AllocationMode : int { Mono = 0, Poly = 1, MidiChannel = 2 };
class Allocation
{
public:
  template <typename Send>
  void setMode(AllocationMode mode, Send&& send) noexcept
  {
    if(mode == m_mode) return;
    stop(send);
    m_mode = mode;
  }

  // Retire eligibility, never outstanding note-off debt. A replacement engine
  // must not let a delayed release from its predecessor kill a new retrigger.
  template <typename Send>
  void stop(Send&& send) noexcept
  {
    for(int channel = 0; channel < 16; ++channel) clear(channel);
    for(int channel = 0; channel < 16; ++channel)
      emit(send, 0xb0 | channel, 120, 0);
  }
  void resetPedals() noexcept { m_sustain.fill(false); m_sostenuto.fill(false); }

  template <typename Send>
  void midi(std::span<const uint8_t> bytes, Send&& send, bool accepted = true) noexcept
  {
    if(bytes.empty()) return;
    if(bytes.size() == 1 && bytes[0] == 0xff)
    {
      for(int channel = 0; channel < 16; ++channel) clear(channel);
      resetPedals();
      send(bytes);
      return;
    }
    const int kind = bytes[0] & 0xf0, channel = bytes[0] & 15;
    if(bytes.size() != 3 || bytes[1] > 127 || bytes[2] > 127)
    { send(bytes); return; }
    const int key = bytes[1], value = bytes[2];
    if(kind == 0x90 && value)
    {
      auto& note = m_notes[channel * 128 + key];
      if(note.debt == std::numeric_limits<uint64_t>::max()) return;
      ++note.debt;
      if(!accepted) return;
      note.held = true;
      note.eligible = true;
      note.velocity = value;
      note.order = ++m_order;
      // Coalescing is an audible retrigger, not an extra native voice.
      if(note.sounding) emit(send, 0x80 | channel, key, 0);
      note.sounding = false;
      reconcile(send, channel, key);
      return;
    }
    if(kind == 0x80 || kind == 0x90)
    {
      auto& note = m_notes[channel * 128 + key];
      if(!note.debt || --note.debt) return;
      note.held = false;
      if(!m_sustain[channel] && !note.latched) note.eligible = false;
      reconcile(send, channel, key);
      return;
    }
    if(kind == 0xb0)
    {
      if(key == 64 || key == 66)
      {
        const bool down = value >= 64;
        if(key == 64) m_sustain[channel] = down;
        else
        {
          if(down && !m_sostenuto[channel])
            for(int k = 0; k < 128; ++k)
            {
              auto& note = m_notes[channel * 128 + k];
              note.latched = note.eligible && note.held;
            }
          m_sostenuto[channel] = down;
          if(!down)
            for(int k = 0; k < 128; ++k) m_notes[channel * 128 + k].latched = false;
        }
        releasePedal(channel);
        reconcile(send, channel);
        // Native pedals must not latch a displaced mono voice. Deferred musical
        // note-offs implement both pedals identically in all allocation modes.
        return;
      }
      if(key == 120)
      {
        clear(channel);
        send(bytes);
        if(m_mode == AllocationMode::Mono) reconcile(send, channel);
        return;
      }
      if(key == 123 || key >= 124)
      {
        for(int k = 0; k < 128; ++k)
          m_notes[channel * 128 + k].held = false;
        releasePedal(channel);
        reconcile(send, channel);
        // CC123 is implemented by the deferred releases above; passing it to
        // a pedal-free native engine would also cut the sustained notes.
        if(key != 123) send(bytes);
        return;
      }
      if(key == 121)
      {
        m_sustain[channel] = m_sostenuto[channel] = false;
        for(int k = 0; k < 128; ++k) m_notes[channel * 128 + k].latched = false;
        releasePedal(channel);
        send(bytes);
        reconcile(send, channel);
        return;
      }
    }
    // Programs, bank selects, bend, pressure and other controllers retain their
    // original channel. In particular, channel 10 remains the native drum part.
    send(bytes);
  }

private:
  struct Note
  {
    uint64_t debt{}, order{};
    uint8_t velocity{};
    bool held{}, eligible{}, latched{}, sounding{};
  };
  template <typename Send>
  static void emit(Send& send, int status, int key, int value) noexcept
  {
    const std::array<uint8_t, 3> bytes{uint8_t(status), uint8_t(key), uint8_t(value)};
    send(std::span<const uint8_t>{bytes});
  }
  void clear(int channel) noexcept
  {
    for(int key = 0; key < 128; ++key)
    {
      auto& note = m_notes[channel * 128 + key];
      note.held = note.eligible = note.latched = note.sounding = false;
    }
  }
  void releasePedal(int channel) noexcept
  {
    for(int key = 0; key < 128; ++key)
    {
      auto& note = m_notes[channel * 128 + key];
      if(!note.held && !m_sustain[channel] && !note.latched) note.eligible = false;
    }
  }
  template <typename Send>
  void reconcile(Send& send, int channel, int key = -1) noexcept
  {
    const bool mono = m_mode == AllocationMode::Mono;
    const bool singleKey = m_mode == AllocationMode::Poly && key >= 0;
    const int begin = mono ? 0 : channel * 128 + (singleKey ? key : 0);
    const int end = mono ? int(m_notes.size()) : singleKey ? begin + 1 : (channel + 1) * 128;
    if(m_mode == AllocationMode::Poly)
    {
      for(int i = begin; i < end; ++i)
      {
        auto& note = m_notes[i];
        if(note.sounding == note.eligible) continue;
        emit(send, (note.eligible ? 0x90 : 0x80) | channel, i % 128,
            note.eligible ? note.velocity : 0);
        note.sounding = note.eligible;
      }
      return;
    }
    int selected = -1;
    for(int i = begin; i < end; ++i)
      if(m_notes[i].eligible && (selected < 0 || m_notes[i].order > m_notes[selected].order))
        selected = i;
    // Release before starting the replacement, irrespective of key ordering.
    for(int i = begin; i < end; ++i)
      if(m_notes[i].sounding && selected != i)
      {
        emit(send, 0x80 | (i / 128), i % 128, 0);
        m_notes[i].sounding = false;
      }
    if(selected >= 0 && !m_notes[selected].sounding)
    {
      emit(send, 0x90 | (selected / 128), selected % 128, m_notes[selected].velocity);
      m_notes[selected].sounding = true;
    }
  }
  std::array<Note, 16 * 128> m_notes{};
  std::array<bool, 16> m_sustain{}, m_sostenuto{};
  uint64_t m_order{};
  AllocationMode m_mode{AllocationMode::Poly};
};
}
