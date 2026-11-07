// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
namespace Emulation::Wurli
{
enum class Allocation { Mono, Poly, MidiChannel };
// The amplifier pot is part of the circuit; Volume is independent linear output gain.
struct Settings {
  float volume{0.7f};
  float amplifier{0.8f};
  float tremolo{0.5f};
  float speaker{0.f};
  bool correction{false};
  int velocityCurve{0}; // Linear MIDI input; the source model retains its physical response.
  Allocation voiceAllocation{Allocation::Poly};
  bool operator==(const Settings&) const = default;
};
const char* validate(const Settings&) noexcept;
class Engine final
{
public:
  explicit Engine(double sampleRate, Settings = {});
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  bool configure(Settings) noexcept;
  void midi(std::span<const uint8_t>) noexcept;
  void render(double* left, double* right, std::size_t frames) noexcept;
  void panic() noexcept;
  void reset() noexcept;
  const char* error() const noexcept;
  std::size_t activeVoices() const noexcept;
  std::size_t heldVoices() const noexcept;
  std::size_t sustainedVoices() const noexcept;
  double sampleRate() const noexcept;
private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
}
