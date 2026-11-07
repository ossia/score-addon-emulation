// SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine.hpp"
#include <Emulation/Volume.hpp>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace Emulation::Sid
{
const char* validate(const Patch& p) noexcept
{
  if(int(p.model) < 0 || int(p.model) > 1) return "SID supports MOS 6581 and CSG 8580 only.";
  if(int(p.clock) < 0 || int(p.clock) > 1) return "SID supports PAL and NTSC C64 clocks only.";
  if(int(p.allocation) < 0 || int(p.allocation) > 2) return "Unsupported SID voice allocation.";
  if(p.filterCurve < 0 || p.filterCurve > 100 || p.combinedStrength < 0 || p.combinedStrength > 2)
    return "Invalid SID filter curve or combined-waveform strength.";
  if(!std::isfinite(p.volume) || p.volume < 0. || p.volume > MaxVolume) return "SID Volume must be finite and in [0, 32].";
  if(p.cutoff < 0 || p.cutoff > 2047 || p.resonance < 0 || p.resonance > 15
      || p.filterMode < 0 || p.filterMode > 7 || p.chipVolume < 0 || p.chipVolume > 15)
    return "SID filter or chip volume is outside its hardware range.";
  for(const auto& o : p.oscillators)
  {
    if(o.waveform < 0 || o.waveform > 15 || o.pulseWidth < 0 || o.pulseWidth > 4095
        || o.attack < 0 || o.attack > 15 || o.decay < 0 || o.decay > 15
        || o.sustain < 0 || o.sustain > 15 || o.release < 0 || o.release > 15
        || o.semitones < -48 || o.semitones > 48 || o.cents < -100 || o.cents > 100)
      return "SID oscillator parameter is outside its supported range.";
  }
  return nullptr;
}
bool sameSetup(const Patch& a, const Patch& b) noexcept
{
  return a.model == b.model && a.clock == b.clock && a.filterCurve == b.filterCurve
      && a.combinedStrength == b.combinedStrength;
}
double controlValue(const Patch& p, int c) noexcept
{
  switch(c)
  {
    case ChipModel: return int(p.model);
    case ClockType: return int(p.clock);
    case VoiceMode: return int(p.allocation);
    case FilterCurve: return p.filterCurve;
    case Volume: return p.volume;
    case Cutoff: return p.cutoff;
    case Resonance: return p.resonance;
    case FilterMode: return p.filterMode;
    case ChipVolume: return p.chipVolume;
    case Voice3Off: return p.voice3Off;
    case CombinedStrength: return p.combinedStrength;
    default: break;
  }
  if(c < OscillatorBase || c >= ControlCount) return 0.;
  const auto& o = p.oscillators[(c - OscillatorBase) / OscillatorFieldCount];
  switch((c - OscillatorBase) % OscillatorFieldCount)
  {
    case Waveform: return o.waveform;
    case PulseWidth: return o.pulseWidth;
    case Sync: return o.sync;
    case Ring: return o.ring;
    case Attack: return o.attack;
    case Decay: return o.decay;
    case Sustain: return o.sustain;
    case Release: return o.release;
    case Semitones: return o.semitones;
    case Cents: return o.cents;
    case FilterRoute: return o.filterRoute;
  }
  return 0.;
}
bool setControl(Patch& p, int c, double value) noexcept
{
  if(c < 0 || c >= ControlCount || !std::isfinite(value)) return false;
  if(c == Volume) {
    if(value < 0. || value > MaxVolume) return false;
    p.volume = value; return true;
  }
  if(value < -100. || value > 4095. || value != std::trunc(value)) return false;
  const int v = int(value);
  const auto boolean = [&] { return v == 0 || v == 1; };
  switch(c)
  {
    case ChipModel: p.model = Model(v); return true;
    case ClockType: p.clock = Clock(v); return true;
    case VoiceMode: p.allocation = Allocation(v); return true;
    case FilterCurve: p.filterCurve = v; return true;
    case Cutoff: p.cutoff = v; return true;
    case Resonance: p.resonance = v; return true;
    case FilterMode: p.filterMode = v; return true;
    case ChipVolume: p.chipVolume = v; return true;
    case Voice3Off: if(!boolean()) return false; p.voice3Off = bool(v); return true;
    case CombinedStrength: p.combinedStrength = v; return true;
    default: break;
  }
  auto& o = p.oscillators[(c - OscillatorBase) / OscillatorFieldCount];
  switch((c - OscillatorBase) % OscillatorFieldCount)
  {
    case Waveform: o.waveform = v; break;
    case PulseWidth: o.pulseWidth = v; break;
    case Sync: if(!boolean()) return false; o.sync = bool(v); break;
    case Ring: if(!boolean()) return false; o.ring = bool(v); break;
    case Attack: o.attack = v; break;
    case Decay: o.decay = v; break;
    case Sustain: o.sustain = v; break;
    case Release: o.release = v; break;
    case Semitones: o.semitones = v; break;
    case Cents: o.cents = v; break;
    case FilterRoute: if(!boolean()) return false; o.filterRoute = bool(v); break;
  }
  return true;
}
std::vector<uint8_t> encodePatch(const Patch& p)
{
  if(const char* e = validate(p)) throw std::runtime_error(e);
  std::vector<uint8_t> bytes;
  bytes.reserve(8 + ControlCount * 8);
  auto put = [&](uint64_t v) { for(int b = 0; b < 8; ++b) bytes.push_back(uint8_t(v >> (8 * b))); };
  put(1);
  for(int c = 0; c < ControlCount; ++c) put(std::bit_cast<uint64_t>(controlValue(p, c)));
  return bytes;
}
Patch decodePatch(std::span<const uint8_t> bytes)
{
  if(bytes.size() != 8 + ControlCount * 8) throw std::runtime_error("Invalid SID semantic patch size.");
  std::size_t pos{};
  auto get = [&] { uint64_t v{}; for(int b = 0; b < 8; ++b) v |= uint64_t(bytes[pos++]) << (8 * b); return v; };
  if(get() != 1) throw std::runtime_error("Unsupported SID semantic patch version.");
  Patch p;
  for(int c = 0; c < ControlCount; ++c)
    if(!setControl(p, c, std::bit_cast<double>(get()))) throw std::runtime_error("Invalid SID semantic patch parameter.");
  if(const char* e = validate(p)) throw std::runtime_error(e);
  return p;
}
}
