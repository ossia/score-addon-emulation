#include <Emulation/Opl/Engine.hpp>
#include <Emulation/Opl/Allocation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace
{
using namespace Emulation::Opl;

void send(Engine& engine, std::initializer_list<std::uint8_t> bytes)
{
  engine.midi({bytes.begin(), bytes.size()});
  REQUIRE(engine.lastError() == nullptr);
}

struct Audio
{
  std::vector<double> left, right;
  explicit Audio(std::size_t frames) : left(frames), right(frames) { }
};

void render(Engine& engine, Audio& audio, std::size_t begin, std::size_t end,
            std::size_t block = 256)
{
  while(begin < end)
  {
    const auto size = std::min(block, end - begin);
    engine.render(audio.left.data() + begin, audio.right.data() + begin, size);
    REQUIRE(engine.lastError() == nullptr);
    begin += size;
  }
}

double rms(std::span<const double> samples)
{
  double energy{};
  for(const auto sample : samples)
  {
    REQUIRE(std::isfinite(sample));
    energy += sample * sample;
  }
  return std::sqrt(energy / double(samples.size()));
}

double rms(const Audio& audio, std::size_t begin, std::size_t end)
{
  return rms(std::span<const double>{audio.left}.subspan(begin, end - begin));
}

double frequency(const Audio& audio, std::size_t begin, std::size_t end, int rate)
{
  std::vector<double> crossings;
  for(auto i = begin + 1; i < end; ++i)
  {
    const auto a = audio.left[i - 1];
    const auto b = audio.left[i];
    if(a <= 0 && b > 0)
      crossings.push_back(double(i - 1) - a / (b - a));
  }
  REQUIRE(crossings.size() > 20);
  return double(crossings.size() - 1) * rate / (crossings.back() - crossings.front());
}

Audio note(int rate, std::size_t block, Model model = Model::Opl3)
{
  Settings settings;
  settings.model = model;
  Engine engine{double(rate), settings, factoryBank()};
  Audio audio(std::size_t(rate) * 2);
  const auto on = std::size_t(rate / 20);
  const auto off = std::size_t(rate / 2);
  send(engine, {0xC0, 6}); // Original sine patch, no software modulation.
  render(engine, audio, 0, on, block);
  send(engine, {0x90, 69, 100});
  render(engine, audio, on, off, block);
  send(engine, {0x80, 69, 0});
  render(engine, audio, off, audio.left.size(), block);
  return audio;
}
}

TEST_CASE("OPL note timestamps, release and planar outputs", "[emulation][opl]")
{
  for(const auto model : {Model::Opl2, Model::Opl3})
  {
    const auto audio = note(48000, 257, model);
    REQUIRE(rms(audio, 0, 2400) < 1e-8);
    const auto active = rms(audio, 6000, 18000);
    REQUIRE(active > 1e-4);
    REQUIRE(rms(audio, 84000, 96000) < active * .02);
    REQUIRE(rms(audio.right) > 1e-4);
    REQUIRE(std::abs(frequency(audio, 6000, 18000, 48000) - 440.) < 3.);
  }
}

TEST_CASE("OPL fixed event frames are independent of host block size", "[emulation][opl]")
{
  const auto reference = note(48000, 64);
  for(const auto block : {std::size_t(1), std::size_t(31), std::size_t(257), std::size_t(4096)})
  {
    const auto actual = note(48000, block);
    REQUIRE(actual.left == reference.left);
    REQUIRE(actual.right == reference.right);
  }
}

TEST_CASE("OPL pitch and release duration survive output sample-rate changes", "[emulation][opl]")
{
  for(const int rate : {22050, 44100, 48000, 96000})
  {
    const auto audio = note(rate, 127);
    const auto active = rms(audio, rate / 8, rate * 3 / 8);
    REQUIRE(active > 1e-4);
    REQUIRE(std::abs(frequency(audio, rate / 8, rate * 3 / 8, rate) - 440.) < 3.);
    REQUIRE(rms(audio, rate * 7 / 4, rate * 2) < active * .02);
  }
}

TEST_CASE("OPL velocity-zero note-off and sustain controllers are MIDI semantics", "[emulation][opl]")
{
  Engine engine{48000., {}, factoryBank()};
  Audio audio(96000);
  send(engine, {0xC0, 6});
  send(engine, {0xB0, 64, 127});
  send(engine, {0x90, 69, 100});
  render(engine, audio, 0, 12000);
  send(engine, {0x90, 69, 0});
  render(engine, audio, 12000, 24000);
  const auto held = rms(audio, 6000, 10000);
  REQUIRE(rms(audio, 18000, 23000) > held * .8);
  send(engine, {0xB0, 64, 0});
  render(engine, audio, 24000, 96000);
  REQUIRE(rms(audio, 84000, 96000) < held * .02);
}

TEST_CASE("OPL channel program, bank and pitch-bend messages reach the synthesizer", "[emulation][opl]")
{
  auto bank = factoryBank();
  auto extra = *findProgram(bank, {false, 0, 0, 6});
  extra.address = {false, 3, 7, 11};
  extra.name = "One octave higher";
  extra.instrument.note_offset1 = 12;
  bank.programs.push_back(extra);
  Engine engine{48000., {}, bank};
  Audio audio(48000);
  send(engine, {0xB2, 0, 3});
  send(engine, {0xB2, 32, 7});
  send(engine, {0xC2, 11});
  send(engine, {0x92, 69, 100});
  render(engine, audio, 0, 18000);
  REQUIRE(std::abs(frequency(audio, 6000, 17000, 48000) - 880.) < 5.);
  send(engine, {0xE2, 0x7F, 0x7F});
  render(engine, audio, 18000, 36000);
  REQUIRE(frequency(audio, 24000, 35000, 48000) > 970.);
  send(engine, {0xA2, 69, 20});
  send(engine, {0xD2, 20});
  send(engine, {0xB2, 120, 0});
  render(engine, audio, 36000, 48000);
  REQUIRE(rms(audio, 45000, 48000) < 1e-5);
}

TEST_CASE("OPL complete SysEx attenuates and restores master volume", "[emulation][opl]")
{
  Engine engine{48000., {}, factoryBank()};
  Audio audio(36000);
  send(engine, {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}); // GM system on.
  send(engine, {0xC0, 6});
  send(engine, {0x90, 69, 100});
  render(engine, audio, 0, 12000);
  const auto original = rms(audio, 6000, 12000);
  REQUIRE(original > 1e-4);
  send(engine, {0xF0, 0x7F, 0x7F, 0x04, 0x01, 0, 0, 0xF7});
  render(engine, audio, 12000, 24000);
  // Native total-level attenuation is finite (63 * 0.75 dB), not a
  // post-render digital mute. Keep the hardware floor instead of masking it.
  REQUIRE(rms(audio, 18000, 24000) < original * .032);
  send(engine, {0xF0, 0x7F, 0x7F, 0x04, 0x01, 0x7F, 0x7F, 0xF7});
  render(engine, audio, 24000, 36000);
  REQUIRE(rms(audio, 30000, 36000) > original * .8);
  const std::array<std::uint8_t, 5> unknown{0xF0, 0x7D, 0, 0, 0xF7};
  engine.midi(unknown);
  REQUIRE(engine.lastError() != nullptr);
  const std::array<std::uint8_t, 2> truncated{0x90, 60};
  engine.midi(truncated);
  REQUIRE(engine.lastError() != nullptr);
  send(engine, {0xFF});
}

TEST_CASE("OPL native WOPL codec preserves every patch field and bank metadata", "[emulation][opl]")
{
  auto bank = factoryBank();
  bank.flags = 0x87; // Codec preserves unknown data; engine separately rejects it.
  bank.volumeModel = 9;
  bank.metadata[0].name = "Native named melodic bank";
  auto& p = *findProgram(bank, {false, 0, 0, 0});
  p.name = "0123456789abcdef0123456789abcdef";
  auto& ins = p.instrument;
  ins.note_offset1 = -32768;
  ins.note_offset2 = 32767;
  ins.midi_velocity_offset = -120;
  ins.second_voice_detune = 37;
  ins.percussion_key_number = 200;
  ins.inst_flags = 0xFB;
  ins.fb_conn1_C0 = 0xFE;
  ins.fb_conn2_C0 = 0xAB;
  ins.delay_on_ms = 65535;
  ins.delay_off_ms = 56789;
  for(unsigned op = 0; op < 4; ++op)
    ins.operators[op] = {std::uint8_t(0x80 + op), std::uint8_t(0x40 + op),
                         std::uint8_t(0xE0 + op), std::uint8_t(0xF0 + op), std::uint8_t(7 - op)};
  const auto bytes = encodeBank(bank);
  const auto decoded = decodeBank(bytes);
  REQUIRE(decoded.version == 3);
  REQUIRE(decoded.flags == bank.flags);
  REQUIRE(decoded.volumeModel == bank.volumeModel);
  REQUIRE(decoded.programs.size() == 256);
  REQUIRE(decoded.metadata[0].name == bank.metadata[0].name);
  REQUIRE(encodeBank(decoded) == bytes);
  const auto* result = findProgram(decoded, p.address);
  REQUIRE(result != nullptr);
  REQUIRE(result->name == p.name);
  REQUIRE(result->instrument.note_offset1 == -32768);
  REQUIRE(result->instrument.note_offset2 == 32767);
  REQUIRE(result->instrument.second_voice_detune == 37);
  REQUIRE(result->instrument.inst_flags == 0xFB);
  REQUIRE(result->instrument.delay_off_ms == 56789);
}

TEST_CASE("OPL old WOPL versions upgrade only when edited state needs new fields", "[emulation][opl]")
{
  Bank bank;
  bank.version = 1;
  Program p;
  p.address = {false, 0, 0, 0};
  p.name = "Old native instrument";
  p.instrument = findProgram(factoryBank(), p.address)->instrument;
  p.instrument.delay_on_ms = 0;
  p.instrument.delay_off_ms = 0;
  bank.programs.push_back(p);
  auto decoded = decodeBank(encodeBank(bank));
  REQUIRE(decoded.version == 1);
  REQUIRE(decoded.metadata.size() == 1); // No phantom percussion bank.
  REQUIRE(decoded.programs.size() == 128);
  decoded.metadata[0].name = "Named bank";
  decoded = decodeBank(encodeBank(decoded));
  REQUIRE(decoded.version == 2);
  findProgram(decoded, p.address)->instrument.delay_off_ms = 4321;
  decoded = decodeBank(encodeBank(decoded));
  REQUIRE(decoded.version == 3);
  REQUIRE(findProgram(decoded, p.address)->instrument.delay_off_ms == 4321);
}

TEST_CASE("OPL codec rejects damaged and ambiguous data instead of dropping fields", "[emulation][opl]")
{
  const auto valid = encodeBank(factoryBank());
  REQUIRE_THROWS_AS(decodeBank({}), std::runtime_error);
  for(const auto size : {std::size_t(10), std::size_t(18), valid.size() - 1})
    REQUIRE_THROWS_AS(decodeBank(std::span<const std::uint8_t>{valid}.first(size)), std::runtime_error);
  auto invalid = valid;
  invalid[0] = 0;
  REQUIRE_THROWS_AS(decodeBank(invalid), std::runtime_error);
  invalid = valid;
  invalid[11] = 4;
  REQUIRE_THROWS_AS(decodeBank(invalid), std::runtime_error);
  invalid = valid;
  invalid.push_back(0);
  REQUIRE_THROWS_AS(decodeBank(invalid), std::runtime_error);
  auto duplicate = factoryBank();
  duplicate.programs.push_back(duplicate.programs.front());
  REQUIRE_THROWS_AS(encodeBank(duplicate), std::runtime_error);
  auto longName = factoryBank();
  longName.programs[0].name.assign(33, 'x');
  REQUIRE_THROWS_AS(encodeBank(longName), std::runtime_error);
}

TEST_CASE("OPL model restrictions reject unsupported settings and preserve rejected edits", "[emulation][opl]")
{
  const auto original = factoryBank();
  Settings opl2;
  opl2.model = Model::Opl2;
  auto four = original;
  four.programs[0].instrument.inst_flags = ADLMIDI_Ins_4op;
  REQUIRE_THROWS_AS(Engine(48000., opl2, four), std::runtime_error);
  REQUIRE_THROWS_AS(Engine(48000., Settings{}, four), std::runtime_error);
  auto waveform = original;
  waveform.programs[0].instrument.operators[0].waveform_E0 = 4;
  REQUIRE_THROWS_AS(Engine(48000., opl2, waveform), std::runtime_error);
  auto softPan = Settings{};
  softPan.softPan = true;
  REQUIRE_THROWS_AS(Engine(48000., softPan, original), std::runtime_error);
  REQUIRE_THROWS_AS(Engine(std::numeric_limits<double>::quiet_NaN(), {}, original), std::runtime_error);
  auto invalidOffset = original;
  invalidOffset.programs[0].instrument.note_offset1 = 128;
  REQUIRE_THROWS_AS(Engine(48000., {}, invalidOffset), std::runtime_error);

  Engine engine{48000., opl2, original};
  const auto address = original.programs[0].address;
  engine.setInstrument(address, waveform.programs[0].instrument);
  REQUIRE(engine.lastError() != nullptr);
  ADL_Instrument after{};
  REQUIRE(engine.getInstrument(address, after));
  REQUIRE(after.operators[0].waveform_E0 == original.programs[0].instrument.operators[0].waveform_E0);
  engine.setInstrument({false, 1, 0, 0}, original.programs[0].instrument);
  REQUIRE(engine.lastError() != nullptr);
  REQUIRE_FALSE(engine.getInstrument({false, 1, 0, 0}, after));
}

TEST_CASE("OPL3 true four-op and OPL2 paired two-op voices produce audio", "[emulation][opl]")
{
  for(const auto model : {Model::Opl2, Model::Opl3})
  {
    auto bank = factoryBank();
    auto& ins = bank.programs[6].instrument;
    ins.inst_flags = model == Model::Opl3 ? 1 : 3;
    ins.operators[2] = ins.operators[0];
    ins.operators[3] = ins.operators[1];
    ins.fb_conn1_C0 = 1;
    ins.fb_conn2_C0 = 1;
    ins.note_offset2 = 7;
    Settings settings;
    settings.model = model;
    settings.fourOpChannels = model == Model::Opl3 ? 1 : 0;
    Engine engine{48000., settings, bank};
    Audio audio(48000);
    send(engine, {0xC0, 6});
    send(engine, {0x90, 60, 100});
    render(engine, audio, 0, 24000);
    REQUIRE(rms(audio, 6000, 22000) > 1e-4);
    send(engine, {0x80, 60, 0});
    render(engine, audio, 24000, 48000);
    REQUIRE(rms(audio, 45000, 48000) < rms(audio, 6000, 22000) * .02);
  }
}

TEST_CASE("OPL edits invalidate native patch caches and retain exact authored values", "[emulation][opl]")
{
  const auto bank = factoryBank();
  const Address address{false, 0, 0, 6};
  Engine engine{48000., {}, bank};
  Audio before(12000), after(12000);
  send(engine, {0xC0, 6});
  send(engine, {0x90, 69, 100});
  render(engine, before, 0, 12000);
  auto ins = findProgram(bank, address)->instrument;
  ins.operators[0].avekf_20 = 0x22; // Carrier multiplier doubles, not just pitch data.
  ins.second_voice_detune = 37; // Preserve odd values despite runtime quantization.
  engine.setInstrument(address, ins);
  REQUIRE(engine.lastError() == nullptr);
  ADL_Instrument queried{};
  REQUIRE(engine.getInstrument(address, queried));
  REQUIRE(queried.second_voice_detune == 37);
  REQUIRE(queried.operators[0].avekf_20 == 0x22);
  send(engine, {0x90, 69, 100});
  render(engine, after, 0, 12000);
  REQUIRE(std::abs(frequency(before, 4000, 11000, 48000) - 440.) < 3.);
  REQUIRE(std::abs(frequency(after, 4000, 11000, 48000) - 880.) < 5.);
}

TEST_CASE("OPL simultaneous instances have isolated programs, notes and render state", "[emulation][opl]")
{
  Engine first{48000., {}, factoryBank()};
  Engine second{48000., {}, factoryBank()};
  Engine reference{48000., {}, factoryBank()};
  Audio a(24000), b(24000), expected(24000);
  send(first, {0xC0, 0});
  send(first, {0x90, 48, 120});
  render(first, a, 0, 12000);
  render(second, b, 0, 12000);
  render(reference, expected, 0, 12000);
  REQUIRE(rms(a, 6000, 12000) > 1e-4);
  REQUIRE(rms(b, 0, 12000) < 1e-8);
  send(second, {0xC0, 6});
  send(reference, {0xC0, 6});
  send(second, {0x90, 69, 100});
  send(reference, {0x90, 69, 100});
  first.panic();
  for(std::size_t frame = 12000; frame < 24000; frame += 64)
  {
    const auto end = std::min<std::size_t>(frame + 64, 24000);
    render(first, a, frame, end);
    render(second, b, frame, end);
    render(reference, expected, frame, end);
  }
  REQUIRE(b.left == expected.left);
  REQUIRE(b.right == expected.right);
}

TEST_CASE("OPL authored percussion plays and short note-off tails terminate", "[emulation][opl]")
{
  Engine engine{48000., {}, factoryBank()};
  Audio audio(96000);
  send(engine, {0x99, 36, 110});
  render(engine, audio, 0, 48, 17);
  send(engine, {0x89, 36, 0});
  render(engine, audio, 48, 96000, 257);
  const auto attack = rms(audio, 48, 4800);
  REQUIRE(attack > 1e-4);
  REQUIRE(rms(audio, 84000, 96000) < attack * .02);
}

TEST_CASE("OPL panic and blank edits silence zero-release voices and allow retriggering", "[emulation][opl]")
{
  for(const bool blankEdit : {false, true})
  {
    auto bank = factoryBank();
    const Address address{false, 0, 0, 6};
    auto& original = findProgram(bank, address)->instrument;
    original.operators[0].susrel_80 = 0;
    original.operators[1].susrel_80 = 0;
    Engine engine{48000., {}, bank};
    Audio audio(36000);
    send(engine, {0xC0, 6});
    send(engine, {0xB0, 64, 127});
    send(engine, {0x90, 69, 100});
    render(engine, audio, 0, 12000);
    const auto sounding = rms(audio, 6000, 12000);
    REQUIRE(sounding > 1e-4);
    if(blankEdit)
    {
      auto blank = original;
      blank.inst_flags |= ADLMIDI_Ins_IsBlank;
      engine.setInstrument(address, blank);
    }
    else
      engine.panic();
    REQUIRE(engine.lastError() == nullptr);
    render(engine, audio, 12000, 24000);
    REQUIRE(rms(audio, 18000, 24000) < sounding * .01);
    if(blankEdit)
      engine.setInstrument(address, original);
    send(engine, {0x90, 69, 100});
    render(engine, audio, 24000, 36000);
    REQUIRE(rms(audio, 30000, 36000) > sounding * .8);
  }
}

TEST_CASE("OPL register burst overflow fails closed without corrupting the emulator", "[emulation][opl]")
{
  Engine engine{48000., {}, factoryBank()};
  Audio audio(1024);
  send(engine, {0xC0, 6});
  send(engine, {0x90, 69, 100});
  render(engine, audio, 0, 1024);
  const std::array<std::uint8_t, 3> volume{0xB0, 7, 100};
  for(int i = 0; i < 8192; ++i) engine.midi(volume);
  REQUIRE(engine.lastError() != nullptr);
  engine.render(audio.left.data(), audio.right.data(), 1024);
  REQUIRE(std::all_of(audio.left.begin(), audio.left.end(), [](double sample) { return sample == 0.; }));
  REQUIRE(std::all_of(audio.right.begin(), audio.right.end(), [](double sample) { return sample == 0.; }));
}

TEST_CASE("OPL hardware rhythm requires prepared percussion topology", "[emulation][opl]")
{
  auto bank = factoryBank();
  const Address drum{true, 0, 0, 36};
  auto& patch = findProgram(bank, drum)->instrument;
  patch.inst_flags = ADLMIDI_RM_BassDrum;
  Engine unprepared{48000., {}, factoryBank()};
  unprepared.setInstrument(drum, patch);
  REQUIRE(unprepared.lastError() != nullptr);
  const auto recalled = decodeBank(encodeBank(bank));
  REQUIRE((findProgram(recalled, drum)->instrument.inst_flags & ADLMIDI_Ins_RhythmModeMask)
      == ADLMIDI_RM_BassDrum);
  Engine prepared{48000., {}, recalled};
  Audio audio(96000);
  send(prepared, {0x99, 36, 110});
  render(prepared, audio, 0, 2400);
  const auto attack = rms(audio, 0, 2400);
  REQUIRE(attack > 1e-4);
  send(prepared, {0x89, 36, 0});
  render(prepared, audio, 2400, 96000);
  REQUIRE(rms(audio, 84000, 96000) < attack * .02);
}

TEST_CASE("OPL model transitions validate the currently edited instrument slots", "[emulation][opl]")
{
  const auto bank = factoryBank();
  Engine engine{48000., {}, bank};
  Settings opl2;
  opl2.model = Model::Opl2;
  REQUIRE(engine.configurationError(opl2) == nullptr);
  const Address address{false, 0, 0, 0};
  auto patch = findProgram(bank, address)->instrument;
  patch.operators[0].waveform_E0 = 7;
  engine.setInstrument(address, patch);
  REQUIRE(engine.lastError() == nullptr);
  REQUIRE(engine.configurationError(opl2) != nullptr);
  REQUIRE(engine.configurationError(Settings{}) == nullptr);
  ADL_Instrument retained{};
  REQUIRE(engine.getInstrument(address, retained));
  CHECK(retained.operators[0].waveform_E0 == 7);
  patch.operators[0].waveform_E0 = 3;
  engine.setInstrument(address, patch);
  REQUIRE(engine.lastError() == nullptr);
  REQUIRE(engine.configurationError(opl2) == nullptr);
}

TEST_CASE("OPL cached compatibility follows hidden operators and blank voice transitions", "[emulation][opl]")
{
  const auto bank = factoryBank();
  Settings opl3;
  opl3.fourOpChannels = 1;
  Settings opl2;
  opl2.model = Model::Opl2;
  Engine engine{48000., opl3, bank};
  const Address first{false, 0, 0, 0}, second{false, 0, 0, 1};
  auto patch = findProgram(bank, first)->instrument;
  patch.operators[2].waveform_E0 = 7;
  engine.setInstrument(first, patch);
  REQUIRE(engine.lastError() == nullptr);
  CHECK(engine.configurationError(opl2) == nullptr);
  patch.inst_flags = 3;
  engine.setInstrument(first, patch);
  REQUIRE(engine.lastError() == nullptr);
  CHECK(engine.configurationError(opl2) != nullptr);
  engine.setInstrument(second, patch);
  REQUIRE(engine.lastError() == nullptr);
  patch.inst_flags = ADLMIDI_Ins_IsBlank;
  engine.setInstrument(first, patch);
  REQUIRE(engine.lastError() == nullptr);
  CHECK(engine.configurationError(opl2) != nullptr);
  engine.setInstrument(second, patch);
  REQUIRE(engine.lastError() == nullptr);
  CHECK(engine.configurationError(opl2) == nullptr);
  patch.inst_flags = 1;
  patch.operators[2].waveform_E0 = 0;
  engine.setInstrument(first, patch);
  REQUIRE(engine.lastError() == nullptr);
  CHECK(engine.configurationError(opl2) != nullptr);
  CHECK(engine.configurationError(Settings{}) != nullptr);
  CHECK(engine.configurationError(opl3) == nullptr);
  patch.inst_flags = 0;
  engine.setInstrument(first, patch);
  REQUIRE(engine.lastError() == nullptr);
  CHECK(engine.configurationError(opl2) == nullptr);
  CHECK(engine.configurationError(Settings{}) == nullptr);
}

namespace
{
struct AllocationProbe
{
  Allocation allocation;
  std::vector<std::vector<uint8_t>> events;
  auto sink()
  {
    return [this](std::span<const uint8_t> bytes) {
      events.emplace_back(bytes.begin(), bytes.end());
    };
  }
  void mode(AllocationMode mode)
  {
    allocation.setMode(mode, sink());
    events.clear();
  }
  void send(std::initializer_list<uint8_t> bytes)
  { allocation.midi({bytes.begin(), bytes.size()}, sink()); }
};
}

TEST_CASE("OPL musical allocation preserves native channel and velocity on fallback",
          "[emulation][opl][allocation]")
{
  AllocationProbe p;
  p.mode(AllocationMode::Mono);
  p.send({0x92, 60, 41});
  p.send({0x99, 38, 110}); // Native percussion part, not a synthetic hardware slot.
  CHECK(p.events == std::vector<std::vector<uint8_t>>{
      {0x92, 60, 41}, {0x82, 60, 0}, {0x99, 38, 110}});
  p.events.clear();
  p.send({0x89, 38, 0});
  CHECK(p.events == std::vector<std::vector<uint8_t>>{
      {0x89, 38, 0}, {0x92, 60, 41}});
  p.mode(AllocationMode::MidiChannel);
  p.send({0x91, 60, 42});
  p.send({0x92, 60, 73});
  p.send({0x91, 64, 99});
  CHECK(p.events == std::vector<std::vector<uint8_t>>{
      {0x91, 60, 42}, {0x92, 60, 73}, {0x81, 60, 0}, {0x91, 64, 99}});
  p.events.clear();
  p.send({0x81, 64, 0});
  CHECK(p.events == std::vector<std::vector<uint8_t>>{
      {0x81, 64, 0}, {0x91, 60, 42}});
}

TEST_CASE("OPL retriggers retain FIFO releases across allocation changes and channel stops",
          "[emulation][opl][allocation]")
{
  for(auto mode : {AllocationMode::Mono, AllocationMode::Poly, AllocationMode::MidiChannel})
  {
    AllocationProbe p;
    p.mode(mode);
    p.send({0x93, 60, 20});
    p.send({0x93, 60, 90});
    CHECK(p.events == std::vector<std::vector<uint8_t>>{
        {0x93, 60, 20}, {0x83, 60, 0}, {0x93, 60, 90}});
    p.events.clear();
    p.send({0x83, 60, 0});
    CHECK(p.events.empty());
    p.send({0x93, 60, 0});
    CHECK(p.events == std::vector<std::vector<uint8_t>>{{0x83, 60, 0}});
    for(int stop : {0, 120, 123, 124})
    {
      CAPTURE(stop);
      p.send({0x93, 60, 70});
      if(stop) p.send({0xb3, uint8_t(stop), 0});
      else
      {
        p.mode(mode == AllocationMode::Mono ? AllocationMode::Poly : AllocationMode::Mono);
        p.mode(mode);
      }
      p.send({0x93, 60, 100});
      p.events.clear();
      p.send({0x83, 60, 0});
      CHECK(p.events.empty());
      p.send({0x83, 60, 0});
      CHECK(p.events == std::vector<std::vector<uint8_t>>{{0x83, 60, 0}});
    }
  }
}

TEST_CASE("OPL pedals and controllers are channel scoped without latching displaced mono notes",
          "[emulation][opl][allocation]")
{
  AllocationProbe p;
  p.mode(AllocationMode::Mono);
  p.send({0xb2, 64, 127});
  p.send({0x92, 60, 55});
  p.send({0x82, 60, 0});
  p.events.clear();
  p.send({0x94, 67, 90});
  p.send({0xe2, 1, 70});
  p.send({0xc4, 6});
  p.send({0xb4, 7, 80});
  p.send({0xb4, 64, 0}); // Must not release channel 3's sustained fallback.
  p.send({0x84, 67, 0});
  CHECK(p.events == std::vector<std::vector<uint8_t>>{
      {0x82, 60, 0}, {0x94, 67, 90}, {0xe2, 1, 70}, {0xc4, 6},
      {0xb4, 7, 80}, {0x84, 67, 0}, {0x92, 60, 55}});
  p.events.clear();
  p.send({0xb2, 64, 0});
  CHECK(p.events == std::vector<std::vector<uint8_t>>{{0x82, 60, 0}});
  p.send({0x92, 62, 55});
  p.send({0xb2, 66, 127});
  p.send({0x82, 62, 0});
  p.events.clear();
  p.send({0xb2, 121, 0});
  CHECK(p.events == std::vector<std::vector<uint8_t>>{{0xb2, 121, 0}, {0x82, 62, 0}});
}

TEST_CASE("OPL engine replacement retires notes but consumes predecessor releases",
          "[emulation][opl][allocation]")
{
  AllocationProbe p;
  p.send({0x90, 60, 80});
  p.allocation.stop(p.sink());
  p.allocation.resetPedals();
  // Note-offs received during preparation must also be consumed.
  p.send({0x80, 60, 0});
  p.send({0x90, 60, 90});
  p.events.clear();
  p.send({0x80, 60, 0});
  CHECK(p.events == std::vector<std::vector<uint8_t>>{{0x80, 60, 0}});
  p.send({0x90, 60, 80});
  p.allocation.stop(p.sink());
  p.send({0x90, 60, 90});
  p.events.clear();
  p.send({0x80, 60, 0});
  CHECK(p.events.empty());
  p.send({0x80, 60, 0});
  CHECK(p.events == std::vector<std::vector<uint8_t>>{{0x80, 60, 0}});
}

TEST_CASE("OPL allocation renders through the native dynamic pool without ROMs",
          "[emulation][opl][allocation]")
{
  for(auto mode : {AllocationMode::Mono, AllocationMode::Poly, AllocationMode::MidiChannel})
  {
    Engine engine{48000., {}, factoryBank()};
    Allocation allocation;
    auto sink = [&](std::span<const uint8_t> bytes) { engine.midi(bytes); };
    allocation.setMode(mode, sink);
    allocation.midi(std::array<uint8_t, 2>{0xc0, 6}, sink);
    allocation.midi(std::array<uint8_t, 3>{0x90, 69, 100}, sink);
    Audio audio(96000);
    render(engine, audio, 0, 12000);
    REQUIRE(rms(audio, 6000, 10000) > 1e-4);
    allocation.midi(std::array<uint8_t, 3>{0x80, 69, 0}, sink);
    render(engine, audio, 12000, 96000);
    CHECK(rms(audio, 84000, 96000) < rms(audio, 6000, 10000) * .02);
  }
}

TEST_CASE("OPL all-notes-off honors its channel pedal and preserves release debt",
          "[emulation][opl][allocation]")
{
  AllocationProbe p;
  p.send({0xb1, 64, 127});
  p.send({0x91, 60, 80});
  p.events.clear();
  p.send({0xb1, 123, 0});
  CHECK(p.events.empty());
  p.send({0xb0, 64, 0});
  CHECK(p.events.empty());
  p.send({0xb1, 64, 0});
  CHECK(p.events == std::vector<std::vector<uint8_t>>{{0x81, 60, 0}});
  p.send({0x91, 60, 100});
  p.events.clear();
  p.send({0x81, 60, 0});
  CHECK(p.events.empty());
  p.send({0x81, 60, 0});
  CHECK(p.events == std::vector<std::vector<uint8_t>>{{0x81, 60, 0}});
}

TEST_CASE("OPL system reset retires fallback ownership and pedals without forgetting old releases",
          "[emulation][opl][allocation]")
{
  for(auto mode : {AllocationMode::Mono, AllocationMode::Poly, AllocationMode::MidiChannel})
  {
    AllocationProbe p;
    p.mode(mode);
    p.send({0xb0, 64, 127});
    p.send({0x90, 60, 80});
    p.send({0xb0, 66, 127});
    p.send({0x90, 64, 90});
    p.events.clear();
    p.send({0xff});
    CHECK(p.events == std::vector<std::vector<uint8_t>>{{0xff}});
    p.events.clear();
    p.send({0x90, 67, 100});
    p.send({0x80, 67, 0});
    // Neither a held pre-reset fallback nor either pedal may resurrect a voice.
    CHECK(p.events == std::vector<std::vector<uint8_t>>{{0x90, 67, 100}, {0x80, 67, 0}});
    p.send({0x90, 60, 110});
    p.events.clear();
    p.send({0x80, 60, 0});
    CHECK(p.events.empty());
    p.send({0x80, 60, 0});
    CHECK(p.events == std::vector<std::vector<uint8_t>>{{0x80, 60, 0}});
    p.events.clear();
    p.send({0x80, 64, 0});
    CHECK(p.events.empty());
  }
}
