// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
namespace Emulation::Wavetable {
enum class Model : int { Scc, SccPlus, HuC6280 };
enum class Assignment : int { Polyphonic = 0, Fixed = 1, Mono = 2 };
struct Channel {
  bool enabled{true};
  double level{1.};
  int tuning{}, left{15}, right{15};
  bool noise{};
  int noisePeriod{16};
  std::array<double, 32> wave{};
  Channel() noexcept;
  bool operator==(const Channel&) const = default;
};
struct Settings {
  Model model{Model::Scc};
  int clock{3579545};
  Assignment assignment{};
  double volume{1.};
  int tuning{}, left{15}, right{15}, lfoDepth{}, lfoFrequency{1}, lfoPeriod{256};
  std::array<Channel, 6> channels{};
  bool operator==(const Settings&) const = default;
};
enum Control { ChipModel, Clock, VoiceAssignment, Volume, Tuning, GlobalLeft, GlobalRight, LfoDepth, LfoFrequency, LfoPeriod, ChannelBase };
enum ChannelField { Enabled, Level, ChannelTuning, Left, Right, Noise, NoisePeriod, Wave, ChannelStride };
constexpr int ControlCount = ChannelBase + 6 * ChannelStride;
constexpr int channelControl(int ch, ChannelField f) { return ChannelBase + ch * ChannelStride + f; }
constexpr bool isWave(int c) { return c >= ChannelBase && c < ControlCount && (c - ChannelBase) % ChannelStride == Wave; }
// Keep established port identities stable across channel and model selection.
constexpr int portId(int c) {
  if(c < ChannelBase) return c + 1;
  constexpr int fields[]{0, 1, 3, 4, 5, 6, 7, 8};
  return ChannelBase + 10 * ((c - ChannelBase) / ChannelStride)
      + fields[(c - ChannelBase) % ChannelStride] + 1;
}
inline double controlValue(const Settings& s, int c) noexcept {
  switch(c) {
    case ChipModel: return int(s.model); case Clock: return s.clock; case VoiceAssignment: return int(s.assignment);
    case Volume: return s.volume; case Tuning: return s.tuning; case GlobalLeft: return s.left; case GlobalRight: return s.right;
    case LfoDepth: return s.lfoDepth; case LfoFrequency: return s.lfoFrequency; case LfoPeriod: return s.lfoPeriod;
  }
  if(c < ChannelBase || c >= ControlCount) return 0.;
  const auto& v = s.channels[(c - ChannelBase) / ChannelStride];
  switch((c - ChannelBase) % ChannelStride) {
    case Enabled: return v.enabled; case Level: return v.level;
    case ChannelTuning: return v.tuning; case Left: return v.left; case Right: return v.right;
    case Noise: return v.noise; case NoisePeriod: return v.noisePeriod;
  }
  return 0.;
}
const char* validate(const Settings&) noexcept;
const char* modelTag(Model) noexcept;
// Validates and changes one scalar only; never examines or copies wave RAM.
bool setScalarControl(Settings&, int control, double value) noexcept;
class Engine {
public:
  Engine(double sampleRate, Settings);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  bool configure(const Settings&) noexcept;
  bool setScalarControl(int control, double value) noexcept;
  void midi(std::span<const uint8_t>) noexcept;
  void panic() noexcept;
  void render(double*, double*, std::size_t) noexcept;
  const char* error() const noexcept;
  int activeVoices() const noexcept;
  int voiceNote(int) const noexcept;
  int nativeWave(int channel, int sample) const noexcept;
private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
}
