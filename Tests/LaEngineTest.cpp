// SPDX-License-Identifier: GPL-3.0-or-later
#include <Emulation/La/Engine.hpp>
#include <Emulation/La/VoicePolicy.hpp>
#include <Emulation/Volume.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
namespace
{
using namespace Emulation::La;
void send(Engine& engine, std::initializer_list<uint8_t> bytes) { engine.midi({bytes.begin(), bytes.size()}); }
Patch lawfulPatch()
{
  const char* control = std::getenv("SCORE_EMULATION_LA_CONTROL_ROM");
  const char* pcm = std::getenv("SCORE_EMULATION_LA_PCM_ROM");
  if(!control && !pcm) SKIP("Real LA engine verification requires lawful matching SCORE_EMULATION_LA_CONTROL_ROM and SCORE_EMULATION_LA_PCM_ROM; no substitute ROMs are used.");
  REQUIRE(control); REQUIRE(pcm);
  auto p = defaultPatch();
  auto refs = std::make_shared<RomReferences>(); refs->controlPath = control; refs->pcmPath = pcm; p.roms = refs;
  p.values[partControl(0, Bank)] = 1; // Original user memory timbre, never a redistributed manufacturer patch.
  p.values[ReverbEnabled] = 0;
  return p;
}
struct Audio
{
  std::vector<double> left, right;
  explicit Audio(std::size_t size) : left(size), right(size) { }
};
Audio render(Engine& engine, std::size_t frames, std::size_t block = 256)
{
  Audio audio{frames};
  for(std::size_t at = 0; at < frames;)
  {
    const auto n = std::min(block, frames - at);
    engine.render(audio.left.data() + at, audio.right.data() + at, n); at += n;
  }
  REQUIRE(std::all_of(audio.left.begin(), audio.left.end(), [](double n) { return std::isfinite(n); }));
  REQUIRE(std::all_of(audio.right.begin(), audio.right.end(), [](double n) { return std::isfinite(n); }));
  return audio;
}
}
TEST_CASE("LA new patches route one instrument to MIDI 1 and layer enables use native receive assignment", "[emulation][la]")
{
  auto p = defaultPatch();
  REQUIRE_FALSE(validate(p));
  CHECK(p.allocation == Allocation::Poly);
  CHECK(p.values[partControl(0, Channel)] == 0);
  CHECK(p.values[RhythmChannel] == 16);
  for(int part = 1; part < 8; ++part)
  {
    CHECK(p.values[partControl(part, Channel)] == 16);
    REQUIRE(setControl(p, partControl(part, Channel),
        channelForEnabled(p.values[partControl(part, Channel)], true)));
    CHECK(p.values[partControl(part, Channel)] == 0);
    REQUIRE(setControl(p, partControl(part, Bank), 1));
    CHECK(p.values[partControl(part, Program)] == part);
  }
  for(int channel = 0; channel < 16; ++channel)
  {
    CHECK(channelForEnabled(channel, true) == channel);
    CHECK(channelForEnabled(channel, false) == 16);
  }
  CHECK(channelForEnabled(16, false) == 16);
  REQUIRE(setControl(p, partControl(1, Level), 63));
  REQUIRE(setControl(p, partControl(1, FineTune), 57));
  CHECK(p.values[partControl(0, Level)] == 100);
  CHECK(p.values[partControl(0, FineTune)] == 50);
  CHECK_FALSE(validate(p)); // Layers share the unchanged native 32-partial budget.
  REQUIRE(setControl(p, partControl(1, Reserve), 5));
  CHECK(validate(p));
}
TEST_CASE("LA schema preserves all edited memory timbres and ROM references", "[emulation][la]")
{
  auto p = defaultPatch();
  CHECK_FALSE(validate(p));
  auto refs = std::make_shared<RomReferences>();
  refs->controlPath = "user/control.rom"; refs->pcmPath = "user/pcm.rom";
  refs->controlHash = std::string(40, 'a'); refs->pcmHash = std::string(40, 'b'); p.roms = refs;
  REQUIRE(setControl(p, Volume, .25));
  REQUIRE(setControl(p, VoiceAllocation, int(Allocation::MidiChannel)));
  REQUIRE(setControl(p, EditTimbre, 63));
  REQUIRE(setControl(p, TimbreBase, 12));
  REQUIRE(setControl(p, partialControl(3, 23), 37));
  REQUIRE(setControl(p, partialControl(2, 49), 89));
  REQUIRE(decodePatch(encodePatch(p)) == p);
  CHECK(p.timbres[0] != p.timbres[63]);
  for(double gain : {0., .25, 1., 4.5, double(Emulation::MaxVolume)}) {
    REQUIRE(setControl(p, Volume, gain)); CHECK_FALSE(validate(p));
    CHECK(decodePatch(encodePatch(p)) == p);
  }
  for(double invalid : {-1., double(Emulation::MaxVolume) + .01,
      std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
    const auto before = p;
    CHECK_FALSE(setControl(p, Volume, invalid)); CHECK(p == before);
    auto rejected = p; rejected.volume = invalid;
    CHECK(validate(rejected)); CHECK_THROWS_AS(encodePatch(rejected), std::runtime_error);
  }
  CHECK_FALSE(setControl(p, partialControl(0, 24), 31));
  CHECK_FALSE(setControl(p, EditTimbre, .5));
  const auto bytes = encodePatch(p);
  auto truncated = bytes; truncated.pop_back(); CHECK_THROWS_AS(decodePatch(truncated), std::runtime_error);
  auto unsupported = bytes; unsupported[0] ^= 1; CHECK_THROWS_AS(decodePatch(unsupported), std::runtime_error);
  auto trailing = bytes; trailing.push_back(0); CHECK_THROWS_AS(decodePatch(trailing), std::runtime_error);
  p.values[partControl(0, Bank)] = 1; p.values[partControl(0, Program)] = 127;
  CHECK(validate(p)); CHECK_THROWS_AS(encodePatch(p), std::runtime_error);
  p = defaultPatch(); p.values[RhythmReserve] = 32; CHECK(validate(p));
}
TEST_CASE("LA bank destinations retain selections and standalone programs edit only their bank", "[emulation][la]")
{
  auto p = defaultPatch();
  REQUIRE(setControl(p, partControl(0, Program), 100));
  REQUIRE(setControl(p, partControl(0, Bank), 1));
  CHECK_FALSE(validate(p));
  CHECK(p.values[partControl(0, Program)] == 0);
  REQUIRE(setControl(p, partControl(0, Program), 23));
  REQUIRE(setControl(p, partialControl(0, 23), 42));
  const auto authored = p.timbres;
  REQUIRE(setControl(p, partControl(0, Bank), 0));
  CHECK(p.values[partControl(0, Program)] == 100);
  REQUIRE(setControl(p, partControl(0, Bank), 1));
  CHECK(p.values[partControl(0, Program)] == 23);
  const auto beforeInvalid = p;
  CHECK_FALSE(setControl(p, partControl(0, Program), 101));
  CHECK(p == beforeInvalid);
  REQUIRE(setControl(p, partControl(0, Bank), 0));
  CHECK(p.values[partControl(0, Program)] == 100);
  REQUIRE(setControl(p, partControl(0, Program), 10));
  REQUIRE(setControl(p, partControl(0, Bank), 1));
  REQUIRE(setControl(p, partControl(0, Bank), 0));
  CHECK(p.values[partControl(0, Program)] == 10);
  auto restored = decodePatch(encodePatch(p));
  REQUIRE(setControl(restored, partControl(0, Bank), 1));
  CHECK(restored.values[partControl(0, Program)] == 23);
  CHECK(restored.timbres == authored);
}
TEST_CASE("LA legacy state seeds destination memory without losing authored timbres", "[emulation][la]")
{
  auto p = defaultPatch();
  p.values[RhythmChannel] = 9;
  for(int part = 0; part < 8; ++part)
    p.values[partControl(part, Channel)] = part + 1;
  REQUIRE(setControl(p, partControl(0, Program), 100));
  REQUIRE(setControl(p, partialControl(2, 23), 31));
  auto legacy = encodePatch(p);
  legacy.resize(legacy.size() - 4); // SLA3 appended allocation.
  // SLA1 has no per-bank selections or sound-provenance hashes.
  const auto extension = 4 + 4 * 4 + 8 + 4 * TimbreBase;
  legacy.erase(legacy.begin() + extension, legacy.begin() + extension + 8 * 5 * 4 + 8);
  legacy[3] = '1';
  auto restored = decodePatch(legacy);
  CHECK(restored.values == p.values);
  CHECK(restored.values[RhythmChannel] == 9);
  for(int part = 0; part < 8; ++part)
    CHECK(restored.values[partControl(part, Channel)] == part + 1);
  CHECK(decodePatch(encodePatch(restored)) == restored);
  CHECK(restored.timbres == p.timbres);
  CHECK(restored.allocation == Allocation::Poly);
  REQUIRE(setControl(restored, partControl(0, Bank), 1));
  CHECK_FALSE(validate(restored));
  REQUIRE(setControl(restored, partControl(0, Bank), 0));
  CHECK(restored.values[partControl(0, Program)] == 100);
}
TEST_CASE("LA missing and unrecognized ROM errors are actionable without accepting fake ROMs", "[emulation][la]")
{
  auto p = defaultPatch();
  CHECK_THROWS_AS(Engine(48000., p), std::runtime_error);
  auto refs = std::make_shared<RomReferences>();
  refs->controlPath = "score-la-nonexistent-control-rom-file.rom"; p.roms = refs;
  CHECK_THROWS_AS(Engine(48000., p), std::runtime_error);
  const auto temp = std::filesystem::temp_directory_path() / ("score-la-invalid-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".rom");
  struct Remove { std::filesystem::path path; ~Remove() { std::error_code error; std::filesystem::remove(path, error); } } remove{temp};
  { std::ofstream file{temp, std::ios::binary}; file << "Not a manufacturer ROM"; }
  const auto utf8 = temp.u8string(); refs->controlPath.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
  CHECK_THROWS_AS(Engine(48000., p), std::runtime_error);
  CHECK_THROWS_AS(Engine(0., p), std::runtime_error);
}
TEST_CASE("LA lawful ROM identity detects wrong roles and preserves hashes", "[emulation][la][rom]")
{
  auto p = lawfulPatch(); Engine engine{48000., p};
  REQUIRE_FALSE(engine.machine().empty());
  REQUIRE(engine.resolvedROMs().controlHash.size() == 40);
  REQUIRE(engine.resolvedROMs().pcmHash.size() == 40);
  auto refs = std::make_shared<RomReferences>(engine.resolvedROMs());
  p.roms = refs; REQUIRE(decodePatch(encodePatch(p)) == p);
  refs->controlHash[0] = refs->controlHash[0] == '0' ? '1' : '0';
  CHECK_THROWS_AS(Engine(48000., p), std::runtime_error);
  *refs = engine.resolvedROMs();
  std::atomic<bool> cancelled{true};
  CHECK_THROWS_AS(Engine(48000., p, &cancelled), std::runtime_error);
  std::swap(refs->controlPath, refs->pcmPath); refs->controlHash.clear(); refs->pcmHash.clear();
  CHECK_THROWS_AS(Engine(48000., p), std::runtime_error);
  if(const auto* mismatch = std::getenv("SCORE_EMULATION_LA_MISMATCHED_PCM_ROM"))
  {
    p = lawfulPatch(); refs = std::make_shared<RomReferences>(*p.roms); refs->pcmPath = mismatch; p.roms = refs;
    CHECK_THROWS_AS(Engine(48000., p), std::runtime_error);
  }
  send(engine, {0xf0, 0x41});
  REQUIRE(engine.lastError());
  send(engine, {0x80, 60, 0});
  CHECK_FALSE(engine.lastError());
  auto invalid = engine.patch(); invalid.volume = Emulation::MaxVolume + 1.;
  REQUIRE_FALSE(engine.setPatch(invalid));
  send(engine, {0x80, 60, 0});
  CHECK(engine.lastError()); // MIDI recovery cannot hide rejected parameter state.
  REQUIRE(engine.setPatch(engine.patch()));
  CHECK_FALSE(engine.lastError());
}
TEST_CASE("LA actual munt audio is partition independent with linear mute and live timbre restoration", "[emulation][la][rom]")
{
  auto p = lawfulPatch();
  for(double rate : {32000.,44100.,48000.,96000.})
  {
    Engine a{rate,p}, b{rate,p};
    send(a,{0x90,60,100}); send(b,{0x90,60,100});
    const auto x = render(a,8192,512), y = render(b,8192,37);
    CHECK(x.left == y.left); CHECK(x.right == y.right);
    CHECK(std::any_of(x.left.begin(),x.left.end(),[](double n) { return std::abs(n) > .0001; }));
    REQUIRE(a.active()); REQUIRE(b.active());
    REQUIRE(b.control(Volume,0.));
    const auto muted = render(b,1024,17); render(a,1024);
    CHECK(std::all_of(muted.left.begin(),muted.left.end(),[](double n) { return n == 0.; }));
    CHECK(std::all_of(muted.right.begin(),muted.right.end(),[](double n) { return n == 0.; }));
    CHECK(b.active()); CHECK(a.frames() == b.frames());
    for(double gain : {.25, 4.5, double(Emulation::MaxVolume), 1.}) {
      REQUIRE(b.control(Volume,gain));
      const auto full = render(a,1024), adjusted = render(b,1024);
      for(std::size_t i = 0; i < full.left.size(); ++i)
      { CHECK(adjusted.left[i] == gain * full.left[i]); CHECK(adjusted.right[i] == gain * full.right[i]); }
    }
    // Part volume automation must not reselect a patch or kill its active note.
    REQUIRE(b.control(partControl(0, Level),70)); CHECK(b.active());
    REQUIRE(b.control(partialControl(0,23),42));
    std::array<uint8_t,246> memory{}, temporary{};
    b.readMemory(0x20000,memory); b.readMemory(0x10000,temporary);
    CHECK(memory[14 + 23] == 42); CHECK(temporary[14 + 23] == 42);
    auto restored = decodePatch(encodePatch(b.patch())); Engine fresh{rate,restored};
    fresh.readMemory(0x20000,temporary); CHECK(temporary == memory);
    b.panic(); const auto stopped = render(b,2048);
    CHECK(std::all_of(stopped.left.begin(),stopped.left.end(),[](double n) { return n == 0.; }));
    CHECK_FALSE(b.active());
  }
}
TEST_CASE("LA timestamped native SysEx and program changes reach real hardware memory", "[emulation][la][rom]")
{
  auto p = lawfulPatch(); Engine engine{48000.,p};
  // Roland DT1: system reverb mode = 2, checksum = -(0x10 + 0 + 1 + 2) & 127.
  send(engine,{0xf0,0x41,0x10,0x16,0x12,0x10,0x00,0x01,0x02,0x6d,0xf7});
  render(engine,512);
  std::array<uint8_t,1> mode{}; engine.readMemory(0x40001,mode); CHECK(mode[0] == 2);
  send(engine,{0xc0,65}); render(engine,512);
  std::array<uint8_t,2> selection{}; engine.readMemory(0xc000,selection);
  CHECK(selection[0] == 1); CHECK(selection[1] == 1);
  REQUIRE(engine.control(partControl(0,Bank),1)); REQUIRE(engine.control(partControl(0,Program),7));
  engine.readMemory(0xc000,selection); CHECK(selection[0] == 2); CHECK(selection[1] == 7);
}

TEST_CASE("LA ROM-derived catalog names describe the selected native patches and timbres", "[emulation][la][rom][catalog]")
{
  auto p = lawfulPatch();
  constexpr char userName[] = "User Voice";
  std::copy_n(userName,10,p.timbres[7].begin());
  Engine engine{48000.,p};
  const auto& catalog = engine.programCatalog(); REQUIRE(catalog);
  REQUIRE((catalog->rhythmPrograms == 30 || catalog->rhythmPrograms == 64));
  const auto nameAt = [&](uint32_t address) {
    std::array<uint8_t,10> bytes{}; engine.readMemory(address,bytes);
    std::string name{reinterpret_cast<const char*>(bytes.data()),bytes.size()};
    const auto end = name.find_last_not_of(' ');
    name.resize(end == std::string::npos ? 0 : end + 1);
    return name;
  };
  REQUIRE(engine.control(partControl(0,Bank),0));
  int named{};
  for(int program = 0; program < 128; ++program)
  {
    REQUIRE(engine.control(partControl(0,Program),program));
    std::array<uint8_t,2> source{}, selected{};
    engine.readMemory(0x14000 + program*8,source);
    engine.readMemory(0xc000,selected);
    CHECK(selected == source);
    if(!catalog->factory[program].empty())
    { ++named; CHECK(nameAt(0x10000) == catalog->factory[program]); }
  }
  CHECK(named > 0);
  for(int bank : {1,2,3,4})
  {
    REQUIRE(engine.control(partControl(0,Program),0));
    REQUIRE(engine.control(partControl(0,Bank),bank));
    const int count = bank == 4 ? catalog->rhythmPrograms : 64;
    constexpr int groups[] = {0,2,0,1,3};
    for(int program = 0; program < count; ++program)
    {
      REQUIRE(engine.control(partControl(0,Program),program));
      std::array<uint8_t,2> selected{}; engine.readMemory(0xc000,selected);
      CHECK(selected[0] == groups[bank]); CHECK(selected[1] == program);
      if(!catalog->timbres[groups[bank]][program].empty())
        CHECK(nameAt(0x10000) == catalog->timbres[groups[bank]][program]);
    }
  }
  if(catalog->rhythmPrograms == 30)
    CHECK_FALSE(engine.control(partControl(0,Program),30));
  else
    REQUIRE(engine.control(partControl(0,Program),63)); // new MT-32 as well as CM-compatible
  CHECK(catalog->timbres[2][7] == nameAt(0x20000 + 7*256));
}

TEST_CASE("LA native channel 1 reaches every enabled layer without changing independent patch parameters", "[emulation][la][rom]")
{
  auto p = lawfulPatch();
  REQUIRE(setControl(p, partControl(1, Channel), channelForEnabled(16, true)));
  REQUIRE(setControl(p, partControl(1, Bank), 1));
  REQUIRE(setControl(p, partControl(1, Level), 63));
  REQUIRE(setControl(p, partControl(1, FineTune), 57));
  Engine engine{48000., p};
  std::array<uint8_t, 9> assignments{};
  engine.readMemory(0x4000d, assignments);
  CHECK(assignments[0] == 0); CHECK(assignments[1] == 0);
  for(int part = 2; part < 9; ++part) CHECK(assignments[part] == 16);
  std::array<uint8_t, 2> disabledBefore{}, disabledAfter{};
  engine.readMemory(0xc020, disabledBefore);
  send(engine, {0xc0, 65}); render(engine, 512);
  for(int part = 0; part < 2; ++part)
  {
    std::array<uint8_t, 2> selection{};
    engine.readMemory(0xc000 + part * 16, selection);
    CHECK(selection[0] == 1); CHECK(selection[1] == 1);
  }
  engine.readMemory(0xc020, disabledAfter);
  CHECK(disabledBefore == disabledAfter);
  // Reapply each authored program after external MIDI, retaining separate tuning and levels.
  REQUIRE(engine.control(partControl(0, Program), 3));
  REQUIRE(engine.control(partControl(1, Program), 7));
  std::array<uint8_t, 10> first{}, second{};
  engine.readMemory(0xc000, first); engine.readMemory(0xc010, second);
  CHECK(first[1] == 3); CHECK(second[1] == 7);
  CHECK(first[3] == 50); CHECK(second[3] == 57);
  CHECK(first[8] == 100); CHECK(second[8] == 63);
}

TEST_CASE("LA allocation policy restores last-note channel and velocity without remapping parts", "[emulation][la][allocation]")
{
  VoicePolicy p;
  p.mode = Allocation::Mono;
  std::vector<std::array<int, 3>> events;
  const auto emit = [&](int ch, int key, int velocity) { events.push_back({ch, key, velocity}); };
  p.note(2, 60, 41, true, emit);
  p.note(7, 67, 99, true, emit);
  REQUIRE(events.size() == 3);
  CHECK(events.back() == std::array<int, 3>{7, 67, 99});
  p.note(7, 67, 0, true, emit);
  REQUIRE(events.size() == 5);
  CHECK(events.back() == std::array<int, 3>{2, 60, 41});
  p.note(12, 80, 100, false, emit); // No melodic receive route: never steals Mono.
  CHECK(events.size() == 5);
  p.note(12, 80, 0, false, emit);
  p.note(2, 60, 0, true, emit);
  CHECK(events.back() == std::array<int, 3>{2, 60, 0});
}

TEST_CASE("LA channel lines and sustain are independently scoped", "[emulation][la][allocation]")
{
  VoicePolicy p;
  p.mode = Allocation::MidiChannel;
  std::vector<std::array<int, 3>> events;
  const auto emit = [&](int ch, int key, int velocity) { events.push_back({ch, key, velocity}); };
  p.note(0, 60, 71, true, emit);
  p.note(1, 64, 83, true, emit);
  REQUIRE(events.size() == 2);
  p.pedal(0, true, emit);
  p.note(0, 60, 0, true, emit);
  CHECK(events.size() == 2);
  p.note(0, 67, 95, true, emit);
  CHECK(events.back() == std::array<int, 3>{0, 67, 95});
  p.note(0, 67, 0, true, emit); // Latest sustained note stays selected.
  CHECK(events.size() == 4);
  p.pedal(1, false, emit);
  CHECK(events.size() == 4);
  p.pedal(0, false, emit);
  CHECK(events.back() == std::array<int, 3>{0, 67, 0});
  CHECK(p.selected[1].note == 64);
  p.note(1, 64, 0, true, emit);
  CHECK(events.back() == std::array<int, 3>{1, 64, 0});
}

TEST_CASE("LA repeat lifetimes survive mode and controller stops", "[emulation][la][allocation]")
{
  for(auto mode : {Allocation::Mono, Allocation::Poly, Allocation::MidiChannel})
  {
    VoicePolicy p;
    p.mode = mode;
    std::vector<std::array<int, 3>> events;
    const auto emit = [&](int ch, int key, int velocity) { events.push_back({ch, key, velocity}); };
    p.note(3, 60, 40, true, emit);
    p.note(3, 60, 90, true, emit);
    REQUIRE(events.size() == 3);
    p.note(3, 60, 0, true, emit);
    CHECK(events.size() == 3); // Old key-up cannot release the retrigger.
    p.stop();
    p.mode = Allocation::MidiChannel;
    p.note(3, 60, 100, true, emit);
    const auto afterSwitch = events.size();
    p.note(3, 60, 0, true, emit);
    CHECK(events.size() == afterSwitch);
    p.note(3, 60, 0, true, emit);
    CHECK(events.back() == std::array<int, 3>{3, 60, 0});
    p.note(3, 60, 77, true, emit);
    p.stopChannel(3, false, emit);
    p.note(3, 60, 88, true, emit);
    const auto afterStop = events.size();
    p.note(3, 60, 0, true, emit);
    CHECK(events.size() == afterStop);
    p.note(3, 60, 0, true, emit);
    CHECK(events.back() == std::array<int, 3>{3, 60, 0});
  }
}

TEST_CASE("LA all-notes-off sustain and full MIDI key space stay bounded", "[emulation][la][allocation]")
{
  VoicePolicy p;
  p.mode = Allocation::Mono;
  std::array<int, 3> last{};
  const auto emit = [&](int ch, int key, int velocity) { last = {ch, key, velocity}; };
  for(int ch = 0; ch < 16; ++ch)
    for(int key = 0; key < 128; ++key) p.note(ch, key, 64, true, emit);
  CHECK(last == std::array<int, 3>{15, 127, 64});
  p.pedal(15, true, emit);
  p.stopChannel(15, true, emit);
  CHECK(p.selected[0].channel == 15);
  p.pedal(15, false, emit);
  CHECK(last == std::array<int, 3>{14, 127, 64});
  p.notes.keys[0][0].outstanding = std::numeric_limits<uint64_t>::max();
  p.note(0, 0, 120, true, emit); // Saturation rejects safely, without wrapping.
  CHECK(last == std::array<int, 3>{14, 127, 64});
}

TEST_CASE("LA SLA2 migration preserves authored routing and defaults to Poly", "[emulation][la][allocation]")
{
  auto p = defaultPatch();
  p.values[partControl(0, Channel)] = 8;
  p.values[partControl(1, Channel)] = 8;
  p.values[RhythmChannel] = 8;
  auto bytes = encodePatch(p);
  bytes[3] = '2';
  bytes.resize(bytes.size() - 4);
  CHECK(decodePatch(bytes) == p);
  for(int mode = 0; mode < 3; ++mode)
  {
    REQUIRE(setControl(p, VoiceAllocation, mode));
    CHECK(decodePatch(encodePatch(p)) == p);
    CHECK(p.values[partControl(0, Channel)] == 8);
    CHECK(p.values[partControl(1, Channel)] == 8);
    CHECK(p.values[RhythmChannel] == 8);
  }
  const auto before = p;
  CHECK_FALSE(setControl(p, VoiceAllocation, 3));
  CHECK_FALSE(setControl(p, VoiceAllocation, .5));
  CHECK(p == before);
}

TEST_CASE("LA native queued allocation retriggers and falls back across authored layers", "[emulation][la][allocation][rom]")
{
  auto p = lawfulPatch();
  p.allocation = Allocation::Mono;
  p.values[partControl(1, Channel)] = 0;
  p.values[partControl(1, Bank)] = 1;
  p.values[partControl(2, Channel)] = 1;
  p.values[partControl(2, Bank)] = 1;
  p.values[MidiDelay] = 2;
  Engine engine{48000., p};
  const auto check = [&](int part, int key, int velocity) {
    std::array<uint8_t, 32> keys{}, velocities{};
    const auto count = engine.playingNotes(part, keys, velocities);
    REQUIRE(count == (key < 0 ? 0 : 1));
    if(key >= 0) { CHECK(keys[0] == key); CHECK(velocities[0] == velocity); }
  };
  send(engine, {0x90, 60, 41}); render(engine, 48000);
  check(0, 60, 41); check(1, 60, 41);
  send(engine, {0x90, 60, 92}); render(engine, 48000);
  check(0, 60, 92); check(1, 60, 92); // Single-assign abort must retry, not drop.
  send(engine, {0x80, 60, 0}); render(engine, 48000);
  check(0, 60, 92); // Old key-up cannot stop retrigger.
  send(engine, {0x91, 67, 73}); render(engine, 48000);
  check(0, -1, 0); check(1, -1, 0); check(2, 67, 73);
  send(engine, {0x81, 67, 0}); render(engine, 48000);
  check(0, 60, 92); check(1, 60, 92); check(2, -1, 0);
  REQUIRE(engine.control(VoiceAllocation, int(Allocation::MidiChannel)));
  send(engine, {0x90, 60, 80});
  send(engine, {0x80, 60, 0}); // Debt from the old Mono note.
  send(engine, {0x91, 64, 88}); render(engine, 48000);
  check(0, 60, 80); check(1, 60, 80); check(2, 64, 88);
  std::array<uint8_t, 9> receive{};
  engine.readMemory(0x4000d, receive);
  CHECK(receive[0] == 0); CHECK(receive[1] == 0); CHECK(receive[2] == 1);
  REQUIRE(engine.control(VoiceAllocation, int(Allocation::Poly)));
  send(engine, {0x90, 72, 90}); // Leave a delayed event queued during mode switch.
  REQUIRE(engine.control(VoiceAllocation, int(Allocation::Mono)));
  render(engine, 48000);
  check(0, -1, 0); check(1, -1, 0); check(2, -1, 0);
}

TEST_CASE("LA native Poly retriggers under sustain without releasing other keys", "[emulation][la][allocation][rom]")
{
  auto p = lawfulPatch();
  p.values[partControl(0, AssignMode)] = 2; // Native multi-assign POLY3.
  Engine engine{48000., p};
  send(engine, {0xb0, 64, 127});
  send(engine, {0x90, 60, 51});
  send(engine, {0x90, 64, 62});
  send(engine, {0x80, 64, 0});
  render(engine, 48000);
  send(engine, {0x90, 60, 93});
  send(engine, {0x80, 60, 0}); // FIFO release of the older strike.
  render(engine, 48000);
  std::array<uint8_t, 32> keys{}, velocities{};
  REQUIRE(engine.playingNotes(0, keys, velocities) == 2);
  CHECK(std::find(keys.begin(), keys.begin() + 2, 60) != keys.begin() + 2);
  CHECK(std::find(keys.begin(), keys.begin() + 2, 64) != keys.begin() + 2);
  const auto retrigger = std::find(keys.begin(), keys.begin() + 2, 60) - keys.begin();
  CHECK(velocities[retrigger] == 93);
  send(engine, {0xb0, 120, 0}); render(engine, 48000);
  CHECK(engine.playingNotes(0, keys, velocities) == 0);
  send(engine, {0x90, 72, 84});
  send(engine, {0x80, 72, 0}); render(engine, 48000);
  REQUIRE(engine.playingNotes(0, keys, velocities) == 1);
  CHECK(keys[0] == 72); // All-sounds-off must retain channel pedal state.
  send(engine, {0xb0, 64, 0}); render(engine, 48000);
  CHECK(engine.playingNotes(0, keys, velocities) == 0);
}
