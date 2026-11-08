// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>
namespace Emulation::Apu {
enum class Model : int { NesNtsc, NesPal, Dmg, Cgb };
// Serialized Routing values and inlet IDs remain compatible with older presets.
// Instrument is labelled Mono; NativeMidi is labelled MIDI channel. Hardware
// roles are not interchangeable, so there is deliberately no Poly allocation.
enum class RoutingMode : int { Instrument=0, NativeMidi=1 };
enum class SoundRole : int { Pulse1, Pulse2, TriangleWave, Noise, Dmc };
struct Role {
  int duty{2}, length{0}, level{12}, envelopePeriod{0};
  bool lengthEnable{}, envelopeUp{}, envelopeEnable{};
  int sweepPeriod{0}, sweepShift{0}; bool sweepNegate{}, sweepEnable{};
  int rate{4}, linear{127}; bool noiseShort{}, left{true}, right{true};
  bool operator==(const Role&) const = default;
};
struct Settings {
  Model model{Model::NesNtsc}; int clock{0}; double volume{0.7};
  RoutingMode routing{RoutingMode::Instrument};
  SoundRole sound{SoundRole::Pulse1}, inactiveSound{SoundRole::Pulse1};
  // Listed element by element: when GCC 14-16 constant-evaluate a value-initialised
  // std::array<Role,N> whose first edit is at index 2 or more, that element and
  // every one after it lose Role's default member initialisers.
  std::array<Role,5> roles{Role{},Role{},Role{.level=1},Role{},Role{}};
  // The editor and automation address roles; switching architectures exchanges
  // these banks without interpreting a NES table index as a Game Boy preload.
  std::array<Role,5> inactiveRoles{roles};
  std::array<uint8_t,32> wave = [] {
    std::array<uint8_t,32> samples{};
    for(int i=0;i<32;++i) samples[i]=(15*(i<=16?i:32-i)+8)/16;
    return samples;
  }();
  int dmcAddress{0}, dmcLength{16}, dmcLevel{64}; bool dmcLoop{};
  bool operator==(const Settings&) const = default;
};
// Raw user-authored bytes occupy CPU $C000..$FFFF. Unprovided memory is zero;
// native DMC wrapping into $8000..$BFFF reads zero. No ROM data is supplied.
struct DmcData {
  static constexpr int capacity=16384;
  std::array<uint8_t,capacity> bytes{}; int size{};
  bool operator==(const DmcData&) const = default;
};
DmcData defaultDmc() noexcept;
const char* validate(const Settings&) noexcept;
const char* validate(const DmcData&) noexcept;
int defaultClock(Model) noexcept;
int effectiveClock(const Settings&) noexcept;
class Engine {
public:
  Engine(); ~Engine(); Engine(const Engine&)=delete; Engine& operator=(const Engine&)=delete;
  bool prepare(double sampleRate, const Settings&, const DmcData& = defaultDmc());
  bool configure(const Settings&) noexcept;
  void midi(std::span<const uint8_t>) noexcept;
  void render(double* left,double* right,int frames) noexcept;
  void panic() noexcept;
  const char* error() const noexcept;
  const char* midiError() const noexcept;
  int activeVoices() const noexcept;
  const Settings& settings() const noexcept;
private:
  struct Impl; std::unique_ptr<Impl> impl;
};
}
