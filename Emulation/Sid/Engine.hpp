// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Emulation::Sid
{
enum class Model : int { Mos6581, Csg8580 };
enum class Clock : int { Pal, Ntsc };
enum class Allocation : int { Mono, Poly, Fixed };
// Serialized inlet IDs are 1 + Control. Never reorder.
enum Control : int
{
  ChipModel = 0, ClockType = 1, VoiceMode = 2, FilterCurve = 3, Volume = 4,
  Cutoff = 5, Resonance = 6, FilterMode = 7, ChipVolume = 8, Voice3Off = 9,
  CombinedStrength = 10, OscillatorBase = 11, ControlCount = 44
};
enum OscillatorField : int
{
  Waveform = 0, PulseWidth = 1, Sync = 2, Ring = 3, Attack = 4, Decay = 5,
  Sustain = 6, Release = 7, Semitones = 8, Cents = 9, FilterRoute = 10,
  OscillatorFieldCount = 11
};
constexpr int oscillatorControl(int voice, OscillatorField field) noexcept
{ return OscillatorBase + voice * OscillatorFieldCount + field; }
struct Oscillator
{
  int waveform{2}; // SID waveform nibble: triangle=1, saw=2, pulse=4, noise=8.
  int pulseWidth{2048};
  bool sync{}, ring{};
  int attack{2}, decay{6}, sustain{12}, release{5};
  int semitones{}, cents{};
  bool filterRoute{};
  bool operator==(const Oscillator&) const = default;
};
struct Patch
{
  Model model{Model::Mos6581};
  Clock clock{Clock::Pal};
  Allocation allocation{Allocation::Mono};
  int filterCurve{50}; // Per-instance curve only; upstream range/caps are global.
  double volume{1.}; // Post-render gain, independent of the chip volume and CC7.
  int cutoff{1024}, resonance{}, filterMode{1}, chipVolume{15};
  bool voice3Off{};
  int combinedStrength{};
  std::array<Oscillator, 3> oscillators{};
  bool operator==(const Patch&) const = default;
};
const char* validate(const Patch&) noexcept;
bool sameSetup(const Patch&, const Patch&) noexcept;
double controlValue(const Patch&, int) noexcept;
bool setControl(Patch&, int, double) noexcept;
constexpr Patch defaultPatch() noexcept
{
  Patch p;
  p.oscillators[1].cents = -7;
  p.oscillators[2].cents = 7;
  return p;
}
// Little-endian, explicitly versioned semantic fields, never emulator snapshots.
std::vector<uint8_t> encodePatch(const Patch&);
Patch decodePatch(std::span<const uint8_t>);
class Engine
{
public:
  explicit Engine(double sampleRate, const Patch&);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  bool setPatch(const Patch&) noexcept; // Setup changes require a prepared replacement.
  void midi(std::span<const uint8_t>) noexcept;
  void render(double* left, double* right, std::size_t frames) noexcept;
  void panic() noexcept;
  const Patch& patch() const noexcept;
  const char* lastError() const noexcept;
  int activeVoices() const noexcept;
  int voiceNote(int voice) const noexcept;
  uint16_t voiceFrequency(int voice) const noexcept;
  uint8_t voiceControl(int voice) const noexcept;
  uint64_t clocks() const noexcept;
private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
}
