// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <limits>

namespace Emulation
{
// MIDI has no note-instance IDs. Pair repeated notes FIFO, retaining old note-off
// debt when routing or controllers stop their sound. One entry per channel/key
// is sufficient for coalesced retriggers and last-note-priority mono lines.
struct HeldNotes
{
  struct Key
  {
    uint64_t order{}, outstanding{};
    int velocity{};
    bool held{};
  };
  struct Selection
  {
    int channel{-1}, note{-1};
    uint64_t order{};
  };
  std::array<std::array<Key, 128>, 16> keys{};
  uint64_t sequence{};

  bool press(int channel, int note, int velocity, bool accepted = true) noexcept
  {
    auto& key = keys[channel][note];
    if(key.outstanding == std::numeric_limits<uint64_t>::max()
        || sequence == std::numeric_limits<uint64_t>::max()) return false;
    ++key.outstanding;
    if(accepted) { key.order = ++sequence; key.velocity = velocity; key.held = true; }
    return true;
  }
  bool release(int channel, int note, bool pedal) noexcept
  {
    auto& key = keys[channel][note];
    if(key.outstanding == 0 || --key.outstanding != 0 || !key.held) return false;
    key.held = false;
    if(!pedal) key.order = 0;
    return true;
  }
  void releasePedal(int channel) noexcept
  {
    for(auto& key : keys[channel]) if(!key.held) key.order = 0;
  }
  void stopChannel(int channel, bool pedal) noexcept
  {
    for(auto& key : keys[channel])
    {
      key.held = false;
      if(!pedal) key.order = 0;
    }
  }
  void stop() noexcept
  {
    for(int channel = 0; channel < 16; ++channel) stopChannel(channel, false);
  }
  Selection latest(int channel = -1, int excludedChannel = -1) const noexcept
  {
    Selection result;
    const int first = channel < 0 ? 0 : channel;
    const int end = channel < 0 ? 16 : channel + 1;
    for(int ch = first; ch < end; ++ch)
      if(ch != excludedChannel)
        for(int note = 0; note < 128; ++note)
          if(keys[ch][note].order > result.order)
            result = {ch, note, keys[ch][note].order};
    return result;
  }
};
}
