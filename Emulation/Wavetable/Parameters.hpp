// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Engine.hpp"
#include <Emulation/Waveform.hpp>
#include <ossia/network/value/value_conversion.hpp>
#include <cmath>
namespace Emulation::Wavetable {
using LevelMaxima = std::array<double, 6>;
inline constexpr LevelMaxima normalizedLevels{1., 1., 1., 1., 1., 1.};
inline ossia::value portValue(const Settings& s, int c) {
  if(!isWave(c)) {
    if(c == Volume || (c >= ChannelBase && (c - ChannelBase) % ChannelStride == Level))
      return float(controlValue(s, c));
    return int(controlValue(s, c));
  }
  return Waveform::fromSamples(s.channels[(c - ChannelBase) / ChannelStride].wave, 0., 1.);
}
inline bool setControl(Settings& s, int c, const ossia::value& value, double levelMaximum = 1.) noexcept {
  if(c < 0 || c >= ControlCount) return false;
  if(isWave(c)) {
    const auto table = Process::wavetableFromValue(value);
    return table && table->frameSize == 32 && table->frameCount() == 1
        && Process::prepareWaveform(*table, s.channels[(c - ChannelBase) / ChannelStride].wave, 0., 1.);
  }
  if(!value.target<int>() && !value.target<float>() && !value.target<bool>()) return false;
  double number = ossia::convert<double>(value);
  if(c >= ChannelBase && (c - ChannelBase) % ChannelStride == Level) number /= levelMaximum;
  return setScalarControl(s, c, number);
}
}
