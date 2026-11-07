// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Engine.hpp"
#include <Emulation/Volume.hpp>
#include <cmath>
namespace Emulation::Wurli
{
// Stable inlet IDs: enum + 1, with MIDI on zero.
enum Control { Volume, Amplifier, Tremolo, Speaker, Correction, VelocityCurve, VoiceAllocation, ControlCount };
inline double controlValue(const Settings& s, int c) noexcept {
  switch(c) {
    case Volume: return s.volume; case Amplifier: return s.amplifier;
    case Tremolo: return s.tremolo; case Speaker: return s.speaker;
    case Correction: return s.correction; case VelocityCurve: return s.velocityCurve;
    case VoiceAllocation: return int(s.voiceAllocation);
    default: return 0.;
  }
}
inline bool setControl(Settings& s, int c, double v) noexcept {
  if(!std::isfinite(v) || c < 0 || c >= ControlCount) return false;
  if(c == VelocityCurve) { if(v < 0 || v > 3 || std::trunc(v) != v) return false; s.velocityCurve = int(v); }
  else if(c == VoiceAllocation) { if(v < 0 || v > 2 || std::trunc(v) != v) return false; s.voiceAllocation = Allocation(int(v)); }
  else {
    if(v < 0 || v > (c == Volume ? MaxVolume : 1.f)) return false;
    switch(c) {
      case Volume: s.volume = float(v); break; case Amplifier: s.amplifier = float(v); break;
      case Tremolo: s.tremolo = float(v); break; case Speaker: s.speaker = float(v); break;
      case Correction: if(v != 0. && v != 1.) return false; s.correction = bool(v); break;
    }
  }
  return true;
}
}
