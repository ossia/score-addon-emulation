// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace Emulation::Psg
{
enum class Model : int { Ay8910, Ym2149, SegaYm7101 };
enum class Assignment : int { Polyphonic = 0, Fixed = 1, Mono = 2 };
struct Channel
{
  bool tone{true};
  bool noise{};
  int level{15};
  // Retained AY routing; active only when softwareEnvelope is false.
  bool hardwareEnvelope{};
};
struct Settings
{
  Model model{Model::Ay8910};
  int clock{1773400};
  Assignment assignment{};
  std::array<Channel, 3> channels{};
  int noisePeriod{16};
  int envelopePeriod{512};
  int envelopeShape{9};
  bool segaNoise{};
  int segaNoiseRate{0}; // clock / 512, / 1024, / 2048, or tone C
  bool segaWhiteNoise{true};
  int segaNoiseLevel{12};
  // One global owner: false = native channel level / AY routing, true = software ADSR.
  // Legacy patches containing both retain their AY routing for a later source switch.
  bool softwareEnvelope{};
  int attackMs{5}, decayMs{100}, sustain{10}, releaseMs{150};
  double volume{1.};
};
const char* validate(const Settings&) noexcept;
const char* modelTag(Model) noexcept;
class Engine
{
public:
  Engine(double sampleRate, Settings);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  // Model / clock are setup-only. Other edits preserve native clock phase.
  bool configure(const Settings&) noexcept;
  void midi(std::span<const uint8_t>) noexcept;
  void panic() noexcept;
  void render(double* left, double* right, std::size_t frames) noexcept;
  const char* error() const noexcept;
  int activeVoices() const noexcept;
  int voiceNote(int slot) const noexcept;
  int registerValue(int reg) const noexcept;
private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
}
