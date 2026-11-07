#pragma once

#include <adlmidi.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Emulation::Opl
{
enum class Model : int { Opl2 = 0, Opl3 = 1 };

struct Settings
{
  Model model{Model::Opl3};
  int chips{1};
  int fourOpChannels{0}; // Total across all chips, not per chip.
  bool deepVibrato{};
  bool deepTremolo{};
  bool softPan{};
};

struct Address
{
  bool percussion{};
  std::uint8_t msb{}, lsb{}, program{};
  bool operator==(const Address&) const = default;
};

struct Program
{
  Address address;
  std::string name;
  ADL_Instrument instrument{};
};

struct BankMetadata
{
  bool percussion{};
  std::uint8_t msb{}, lsb{};
  std::string name;
};

struct Bank
{
  std::vector<Program> programs;
  std::uint8_t flags{};
  std::uint8_t volumeModel{};
  std::uint16_t version{3};
  std::vector<BankMetadata> metadata;
};

Bank factoryBank();
// Preserve blank slots, native flags/registers, names and bank metadata. Versions
// 1--3 are accepted. Encoding upgrades old versions only when edits require it.
Bank decodeBank(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encodeBank(const Bank& bank);
const Program* findProgram(const Bank&, Address) noexcept;
Program* findProgram(Bank&, Address) noexcept;
// Shared by native editing and engine preparation: never publish an unplayable edit.
const char* instrumentError(const ADL_Instrument&, Address, const Settings&) noexcept;
const char* settingsError(const Settings&) noexcept;
// Whole-bank validation belongs to preparation / UI transactions, not audio ticks.
const char* bankError(const Bank&, const Settings&) noexcept;

// One thread owns each Engine. Construction/destruction and codec operations
// belong off the audio thread. MIDI timestamps belong to the caller: render up
// to an event frame, dispatch midi(), then render the remaining span.
class Engine
{
public:
  Engine(double sampleRate, const Settings&, const Bank&);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  void midi(std::span<const std::uint8_t> message) noexcept;
  void render(double* left, double* right, std::size_t frames) noexcept;
  void panic() noexcept;
  bool getInstrument(Address, ADL_Instrument&) const noexcept;
  // Constant-time validation using requirements cached at preparation / patch edits.
  const char* configurationError(const Settings&) const noexcept;
  // Existing slots only. Successful edits stop sounding voices to safely change
  // voice topology and invalidate the pinned upstream's pointer-based cache.
  void setInstrument(Address, const ADL_Instrument&) noexcept;
  void setDepths(bool vibrato, bool tremolo) noexcept;
  // Static diagnostic text, no allocation; cleared by the next control/event
  // operation. render() preserves an event error unless rendering itself fails.
  const char* lastError() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
}
