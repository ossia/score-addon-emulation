// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Engine.hpp"
#include <Emulation/Volume.hpp>
#include <cmath>
namespace Emulation::Psg
{
// Stable inlet IDs are enum + 1; zero remains the MIDI inlet.
enum Control : int {
  ChipModel = 0, Clock, VoiceAssignment, Volume,
  AyNoisePeriod, AyEnvelopePeriod, AyEnvelopeShape,
  SegaNoiseEnabled, SegaNoiseRate, SegaWhiteNoise, SegaNoiseLevel,
  SoftwareEnvelope, Attack, Decay, Sustain, Release,
  ChannelBase, ControlCount = ChannelBase + 3 * 4
};
enum ChannelField : int { Tone, Noise, Level, HardwareEnvelope };
constexpr int channelControl(int channel, ChannelField field) { return ChannelBase + channel * 4 + field; }
inline bool ayOnly(int c) { return (c >= AyNoisePeriod && c <= AyEnvelopeShape)
    || (c >= ChannelBase && ((c - ChannelBase) % 4 == Noise || (c - ChannelBase) % 4 == HardwareEnvelope)); }
inline bool segaOnly(int c) { return c >= SegaNoiseEnabled && c <= SegaNoiseLevel; }
inline bool validControlValue(int c, double value) noexcept
{
  if(!std::isfinite(value) || value < 0.) return false;
  if(c == Volume) return value <= MaxVolume;
  if(std::floor(value) != value) return false;
  if(c >= ChannelBase && c < ControlCount)
    return value <= ((c - ChannelBase) % 4 == Level ? 15 : 1);
  switch(c) {
    case ChipModel: case VoiceAssignment: return value <= 2.;
    case Clock: return value >= 100000. && value <= 4000000.;
    case AyNoisePeriod: return value <= 31.;
    case AyEnvelopePeriod: return value <= 65535.;
    case AyEnvelopeShape: case SegaNoiseLevel: case Sustain: return value <= 15.;
    case SegaNoiseRate: return value <= 3.;
    case Attack: case Decay: case Release: return value <= 10000.;
    default: return value <= 1.;
  }
}
inline double controlValue(const Settings& s, int c) noexcept
{
  if(c >= ChannelBase && c < ControlCount) {
    const auto& ch = s.channels[(c - ChannelBase) / 4];
    switch((c - ChannelBase) % 4) { case Tone: return ch.tone; case Noise: return ch.noise;
      case Level: return ch.level; case HardwareEnvelope: return ch.hardwareEnvelope; }
  }
  switch(c) {
    case ChipModel: return int(s.model); case Clock: return s.clock;
    case VoiceAssignment: return int(s.assignment); case Volume: return s.volume;
    case AyNoisePeriod: return s.noisePeriod; case AyEnvelopePeriod: return s.envelopePeriod;
    case AyEnvelopeShape: return s.envelopeShape; case SegaNoiseEnabled: return s.segaNoise;
    case SegaNoiseRate: return s.segaNoiseRate; case SegaWhiteNoise: return s.segaWhiteNoise;
    case SegaNoiseLevel: return s.segaNoiseLevel; case SoftwareEnvelope: return s.softwareEnvelope;
    case Attack: return s.attackMs; case Decay: return s.decayMs; case Sustain: return s.sustain;
    case Release: return s.releaseMs; default: return 0.;
  }
}
inline void setControl(Settings& s, int c, double value) noexcept
{
  const int v = static_cast<int>(value);
  if(c >= ChannelBase && c < ControlCount) {
    auto& ch = s.channels[(c - ChannelBase) / 4];
    switch((c - ChannelBase) % 4) { case Tone: ch.tone = bool(v); break; case Noise: ch.noise = bool(v); break;
      case Level: ch.level = v; break; case HardwareEnvelope: ch.hardwareEnvelope = bool(v); break; }
    return;
  }
  switch(c) {
    case ChipModel: s.model = Model(v); break; case Clock: s.clock = v; break;
    case VoiceAssignment: s.assignment = Assignment(v); break; case Volume: s.volume = value; break;
    case AyNoisePeriod: s.noisePeriod = v; break; case AyEnvelopePeriod: s.envelopePeriod = v; break;
    case AyEnvelopeShape: s.envelopeShape = v; break; case SegaNoiseEnabled: s.segaNoise = bool(v); break;
    case SegaNoiseRate: s.segaNoiseRate = v; break; case SegaWhiteNoise: s.segaWhiteNoise = bool(v); break;
    case SegaNoiseLevel: s.segaNoiseLevel = v; break; case SoftwareEnvelope: s.softwareEnvelope = bool(v); break;
    case Attack: s.attackMs = v; break; case Decay: s.decayMs = v; break; case Sustain: s.sustain = v; break;
    case Release: s.releaseMs = v; break; default: break;
  }
}
}
