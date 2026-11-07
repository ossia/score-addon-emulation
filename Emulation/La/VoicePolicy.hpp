// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Engine.hpp"
#include <Emulation/HeldNotes.hpp>

namespace Emulation::La
{
// Musical notes, not hardware voices: parts and the shared partial pool remain
// owned by munt. Native rhythm messages bypass this melodic selector.
struct VoicePolicy
{
  HeldNotes notes;
  std::array<bool, 16> sustain{};
  std::array<HeldNotes::Selection, 16> selected{};
  Allocation mode{Allocation::Poly};

  template<typename Emit>
  void refresh(int channel, Emit&& emit) noexcept
  {
    if(mode == Allocation::Poly) return;
    const int line = mode == Allocation::Mono ? 0 : channel;
    const auto next = notes.latest(mode == Allocation::Mono ? -1 : channel);
    auto& previous = selected[line];
    if(previous.order == next.order) return;
    if(previous.channel >= 0) emit(previous.channel, previous.note, 0);
    previous = next;
    if(next.channel >= 0)
      emit(next.channel, next.note, notes.keys[next.channel][next.note].velocity);
  }

  template<typename Emit>
  void note(int channel, int key, int velocity, bool routed, Emit&& emit) noexcept
  {
    if(velocity)
    {
      const bool repeated = notes.keys[channel][key].order != 0;
      if(!notes.press(channel, key, velocity, routed) || !routed) return;
      if(mode == Allocation::Poly)
      {
        if(repeated) emit(channel, key, 0);
        emit(channel, key, velocity);
      }
      else refresh(channel, emit);
    }
    else if(notes.release(channel, key, sustain[channel]))
    {
      if(mode == Allocation::Poly) emit(channel, key, 0);
      else refresh(channel, emit);
    }
  }

  template<typename Emit>
  void pedal(int channel, bool down, Emit&& emit) noexcept
  {
    sustain[channel] = down;
    if(!down) notes.releasePedal(channel);
    refresh(channel, emit);
  }

  template<typename Emit>
  void stopChannel(int channel, bool hold, Emit&& emit) noexcept
  {
    notes.stopChannel(channel, hold && sustain[channel]);
    refresh(channel, emit);
  }

  // Keep outstanding FIFO lifetimes; mode changes must not make an old key-up
  // release a new retrigger. Controller state belongs to the MIDI channel.
  void stop() noexcept
  {
    notes.stop();
    selected = {};
  }
};
}
