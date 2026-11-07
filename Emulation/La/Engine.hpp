// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
namespace Emulation::La
{
// Inlet ID = Control + 1; MIDI inlet is 0. Append, never reorder.
enum Control : int
{
  ControlROM, PcmROM, Volume, ReverbMode, ReverbTime, ReverbLevel,
  MasterTune, MasterLevel, ReverbEnabled, ReverseStereo, DacMode, MidiDelay,
  NiceRamp, NicePan, NiceMix, RhythmChannel, RhythmLevel, RhythmPan,
  RhythmReverb, RhythmReserve, PartBase, EditTimbre = PartBase + 8 * 11,
  TimbreBase, VoiceAllocation = TimbreBase + 236, ControlCount
};
enum class Allocation : int { Mono, Poly, MidiChannel };
enum PartField : int { Bank, Program, Channel, Level, Pan, KeyShift, FineTune, BendRange, AssignMode, Reverb, Reserve, PartFieldCount };
constexpr int partControl(int part, PartField field) { return PartBase + part * PartFieldCount + field; }
constexpr int partialControl(int partial, int field) { return TimbreBase + 4 + partial * 58 + field; }
// Native receive assignment: 0..15 are MIDI channels, 16 disables the part.
// Enabling a disabled layer starts on MIDI 1; existing assignments stay native.
constexpr int channelForEnabled(int channel, bool enabled) noexcept
{ return enabled ? (channel == 16 ? 0 : channel) : 16; }
struct Field { const char* name; int maximum; };
extern const std::array<Field, 58> partialFields;
struct RomReferences
{
  std::string controlPath, pcmPath, controlHash, pcmHash;
  bool operator==(const RomReferences&) const = default;
};
// Transient labels obtained from an identified synth during worker preparation.
// These contain names only, never ROM/timbre payloads, and are not patch state.
struct ProgramCatalog
{
  std::array<std::string, 128> factory;
  std::array<std::array<std::string, 64>, 4> timbres; // A, B, memory, rhythm
  int rhythmPrograms{};
};
using Timbre = std::array<uint8_t, 246>;
using RomHashes = std::array<std::string, 2>;
struct Patch
{
  std::shared_ptr<const RomReferences> roms{std::make_shared<const RomReferences>()};
  double volume{1.};
  Allocation allocation{Allocation::Poly};
  std::array<int, TimbreBase> values{};
  // Each destination bank retains its own selection across bank edits / undo.
  std::array<std::array<int, 5>, 8> bankPrograms{};
  // Sound provenance, separate from the machine's local resource references.
  std::shared_ptr<const RomHashes> soundROMHashes{std::make_shared<const RomHashes>()};
  // Original, editable memory-bank sounds only; no ROM-derived timbres.
  std::array<Timbre, 64> timbres{};
  bool operator==(const Patch&) const noexcept;
};
Patch defaultPatch();
const char* validate(const Patch&) noexcept;
bool sameSetup(const Patch&, const Patch&) noexcept;
Field controlField(int) noexcept;
double controlValue(const Patch&, int) noexcept;
bool setControl(Patch&, int, double) noexcept;
std::vector<uint8_t> encodePatch(const Patch&);
Patch decodePatch(std::span<const uint8_t>);
class Engine
{
public:
  Engine(double sampleRate, const Patch&, const std::atomic<bool>* cancelled = nullptr);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  bool setPatch(const Patch&) noexcept;
  bool control(int, double) noexcept;
  void midi(std::span<const uint8_t>) noexcept;
  void render(double*, double*, std::size_t) noexcept;
  void panic() noexcept;
  const Patch& patch() const noexcept;
  const RomReferences& resolvedROMs() const noexcept;
  const std::string& machine() const noexcept;
  const std::shared_ptr<const ProgramCatalog>& programCatalog() const noexcept;
  const char* lastError() const noexcept;
  bool active() const noexcept;
  uint64_t frames() const noexcept;
  // Diagnostic readback uses munt's public memory API, not an emulator snapshot.
  void readMemory(uint32_t linearAddress, std::span<uint8_t>) const noexcept;
  uint32_t playingNotes(int part, std::array<uint8_t, 32>& keys,
      std::array<uint8_t, 32>& velocities) const noexcept;
private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
}
