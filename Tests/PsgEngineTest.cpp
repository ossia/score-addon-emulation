// SPDX-License-Identifier: GPL-3.0-or-later
#include <Emulation/Psg/Engine.hpp>
#include <Emulation/Psg/Parameters.hpp>
#include <Emulation/Volume.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <vector>
namespace
{
using namespace Emulation::Psg;
void send(Engine& e, std::initializer_list<uint8_t> bytes)
{ e.midi({bytes.begin(), bytes.size()}); REQUIRE(e.error() == nullptr); }
std::vector<double> render(Engine& e, std::size_t count, std::size_t block = 257)
{
  std::vector<double> l(count), r(count);
  for(std::size_t at = 0; at < count; at += block)
    e.render(l.data() + at, r.data() + at, std::min(block, count - at));
  REQUIRE(l == r);
  REQUIRE(std::all_of(l.begin(), l.end(), [](double n) { return std::isfinite(n); }));
  return l;
}
bool audible(const std::vector<double>& a)
{ const auto [lo, hi] = std::minmax_element(a.begin(), a.end()); return *hi - *lo > 1e-4; }
Settings modelSettings(Model model)
{ Settings s; s.model = model; if(model == Model::SegaYm7101) s.clock = 3579545; return s; }
}
TEST_CASE("PSG coalesces retriggers and never revives stolen poly notes", "[emulation][psg][allocation]")
{
  for(auto model : {Model::Ay8910, Model::Ym2149, Model::SegaYm7101}) {
    Engine e{48000, modelSettings(model)};
    send(e, {0x90, 60, 127}); send(e, {0x90, 60, 100}); send(e, {0x90, 60, 90});
    REQUIRE(e.activeVoices() == 1);
    send(e, {0x80, 60, 0}); send(e, {0x80, 60, 0});
    CHECK(e.activeVoices() == 1);
    send(e, {0x90, 62, 127}); send(e, {0x90, 64, 127});
    REQUIRE(e.activeVoices() == 3);
    send(e, {0x90, 65, 127}); REQUIRE(e.voiceNote(0) == 65);
    send(e, {0x80, 60, 0}); CHECK(e.activeVoices() == 3);
    send(e, {0x80, 65, 0}); CHECK(e.activeVoices() == 2);
    send(e, {0x80, 62, 0}); send(e, {0x80, 64, 0}); CHECK(e.activeVoices() == 0);
  }
}
TEST_CASE("PSG fixed channels, sustain and resets do not revive stolen voices", "[emulation][psg]")
{
  Settings s; s.assignment = Assignment::Fixed; Engine e{48000, s};
  send(e, {0x93, 60, 127}); CHECK(e.activeVoices() == 0);
  send(e, {0x90, 60, 127}); send(e, {0x90, 64, 127});
  send(e, {0x80, 60, 0}); CHECK(e.voiceNote(0) == 64);
  send(e, {0xb0, 64, 127}); send(e, {0x80, 64, 0}); CHECK(e.activeVoices() == 1);
  send(e, {0xb0, 64, 0}); CHECK(e.activeVoices() == 0);
  send(e, {0x91, 67, 127}); send(e, {0xb1, 123, 0}); CHECK(e.activeVoices() == 0);
  send(e, {0x92, 69, 127}); send(e, {0xb2, 120, 0}); CHECK(e.activeVoices() == 0);
  send(e, {0x90, 72, 127}); send(e, {0xff}); CHECK(e.activeVoices() == 0);
}
TEST_CASE("PSG clocks and resampling retain phase across arbitrary block partitions", "[emulation][psg]")
{
  for(auto model : {Model::Ay8910, Model::Ym2149, Model::SegaYm7101})
    for(double rate : {44100., 48000., 96000.}) {
      const auto settings = modelSettings(model);
      Engine whole{rate, settings}, split{rate, settings}, isolated{rate, settings};
      send(whole, {0x90, 69, 127}); send(split, {0x90, 69, 127});
      const auto a = render(whole, 6000, 6000), b = render(split, 6000, 113);
      CHECK(a == b); CHECK(audible(a));
      CHECK_FALSE(audible(render(isolated, 1000)));
      send(whole, {0xe0, 127, 127});
      const int period = model == Model::SegaYm7101 ? whole.registerValue(0)
          : whole.registerValue(0) | (whole.registerValue(1) << 8);
      const int unbent = model == Model::SegaYm7101 ? split.registerValue(0)
          : split.registerValue(0) | (split.registerValue(1) << 8);
      CHECK(period < unbent);
    }
}
TEST_CASE("PSG Volume is post-render, exact at zero, and independent of CC7", "[emulation][psg]")
{
  for(auto model : {Model::Ay8910, Model::Ym2149, Model::SegaYm7101}) {
    auto s = modelSettings(model); Engine reference{48000, s}, adjusted{48000, s};
    send(reference, {0x90, 69, 127}); send(adjusted, {0x90, 69, 127});
    s.volume = .25; REQUIRE(adjusted.configure(s));
    auto a = render(reference, 1000), b = render(adjusted, 1000);
    for(std::size_t i = 0; i < a.size(); ++i) CHECK(b[i] == a[i] * .25);
    REQUIRE(audible(a));
    for(double gain : {2., double(Emulation::MaxVolume)}) {
      s.volume = gain; REQUIRE(adjusted.configure(s));
      CHECK(adjusted.activeVoices() == reference.activeVoices());
      a = render(reference, 1000); b = render(adjusted, 1000);
      for(std::size_t i = 0; i < a.size(); ++i) CHECK(b[i] == a[i] * gain);
    }
    for(double gain : {-1., double(Emulation::MaxVolume) + 1.,
        std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
      s.volume = gain;
      CHECK_FALSE(validControlValue(Volume, gain));
      CHECK_FALSE(adjusted.configure(s));
      CHECK_THROWS_AS((Engine{48000., s}), std::runtime_error);
      a = render(reference, 1000); b = render(adjusted, 1000);
      for(std::size_t i = 0; i < a.size(); ++i) CHECK(b[i] == a[i] * Emulation::MaxVolume);
    }
    s.volume = 0.; REQUIRE(adjusted.configure(s));
    render(reference, 1000); b = render(adjusted, 1000);
    CHECK(std::all_of(b.begin(), b.end(), [](double x) { return x == 0.; }));
    s.volume = 1.; REQUIRE(adjusted.configure(s));
    CHECK(render(reference, 1000) == render(adjusted, 1000));
    send(adjusted, {0xb0, 7, 0});
    CHECK(adjusted.registerValue(model == Model::SegaYm7101 ? 1 : 8)
        == (model == Model::SegaYm7101 ? 15 : 0));
  }
}
TEST_CASE("AY shared envelope and noise keep their native register semantics", "[emulation][psg]")
{
  Settings s; s.assignment = Assignment::Fixed; s.noisePeriod = 23; s.envelopePeriod = 0x1234;
  s.channels[0].hardwareEnvelope = true; s.channels[1].hardwareEnvelope = true;
  s.channels[1].noise = true; s.envelopeShape = 10;
  Engine e{48000, s}; send(e, {0x90, 60, 127}); send(e, {0x91, 67, 127});
  CHECK(e.registerValue(6) == 23); CHECK(e.registerValue(11) == 0x34); CHECK(e.registerValue(12) == 0x12);
  CHECK(e.registerValue(13) == 10); CHECK(e.registerValue(8) == 16); CHECK(e.registerValue(9) == 16);
  CHECK((e.registerValue(7) & 16) == 0); CHECK((e.registerValue(7) & 8) != 0);
  s.softwareEnvelope = true; REQUIRE(e.configure(s)); CHECK(e.error() == nullptr);
  send(e, {0x90, 60, 127}); send(e, {0x91, 67, 127});
  CHECK(e.registerValue(8) < 16); CHECK(e.registerValue(9) < 16);
  s.softwareEnvelope = false; REQUIRE(e.configure(s));
  send(e, {0x90, 60, 127}); send(e, {0x91, 67, 127});
  CHECK(e.registerValue(8) == 16); CHECK(e.registerValue(9) == 16);
}
TEST_CASE("Sega has a separate noise voice, native attenuation and tone C noise coupling", "[emulation][psg]")
{
  auto s = modelSettings(Model::SegaYm7101); s.segaNoise = true; s.segaNoiseRate = 3;
  s.segaNoiseLevel = 9; s.assignment = Assignment::Fixed;
  Engine e{48000, s};
  send(e, {0x90, 60, 127}); send(e, {0x91, 64, 127}); send(e, {0x92, 67, 127});
  send(e, {0x99, 38, 127}); CHECK(e.activeVoices() == 4); CHECK(e.voiceNote(3) == 38);
  CHECK(e.registerValue(6) == 7); CHECK(e.registerValue(7) == 6);
  const auto before = e.registerValue(4); send(e, {0xe2, 127, 127}); CHECK(e.registerValue(4) < before);
  send(e, {0x89, 38, 0}); CHECK(e.activeVoices() == 3); CHECK(e.registerValue(7) == 15);
  s.channels[0].level = 4; REQUIRE(e.configure(s)); CHECK(e.registerValue(1) == 11);
  s.channels[0].tone = false; REQUIRE(e.configure(s)); CHECK(e.registerValue(1) == 15);
}
TEST_CASE("PSG software release and invalid configurations remain bounded", "[emulation][psg]")
{
  Settings s; s.softwareEnvelope = true; s.attackMs = 0; s.decayMs = 180;
  s.sustain = 3; s.releaseMs = 10; Engine e{48000, s};
  send(e, {0x90, 69, 127}); render(e, 200); send(e, {0x80, 69, 0});
  CHECK(e.activeVoices() == 1); render(e, 1000); CHECK(e.activeVoices() == 0);
  s.model = Model(99); CHECK_THROWS_AS(Engine(48000, s), std::runtime_error);
  s = {}; s.clock = 0; CHECK_THROWS_AS(Engine(48000, s), std::runtime_error);
  s = {}; s.volume = std::numeric_limits<double>::quiet_NaN(); CHECK_FALSE(e.configure(s));
  CHECK_THROWS_AS(Engine(1, Settings{}), std::runtime_error);
}

TEST_CASE("PSG pitch saturates at native counter limits without wraparound", "[emulation][psg]")
{
  for(auto model : {Model::Ay8910, Model::Ym2149, Model::SegaYm7101}) {
    auto s = modelSettings(model); Engine e{48000, s};
    send(e, {0x90, 0, 127});
    const int period = model == Model::SegaYm7101 ? e.registerValue(0)
        : e.registerValue(0) | (e.registerValue(1) << 8);
    CHECK(period == (model == Model::SegaYm7101 ? 1023 : 4095));
    e.panic(); send(e, {0x90, 127, 127});
    const int highest = model == Model::SegaYm7101 ? e.registerValue(0)
        : e.registerValue(0) | (e.registerValue(1) << 8);
    CHECK(highest >= 1); CHECK(highest < 20);
  }
}

TEST_CASE("Sega channel level reaches its native nonlinear attenuation DAC", "[emulation][psg]")
{
  auto full = modelSettings(Model::SegaYm7101), quiet = full;
  quiet.channels[0].level = 4; // attenuation 11: native table amplitude 0.051
  Engine a{48000, full}, b{48000, quiet};
  send(a, {0x90, 69, 127}); send(b, {0x90, 69, 127});
  const auto loud = render(a, 4000), soft = render(b, 4000);
  const double high = *std::max_element(loud.begin() + 1000, loud.end());
  const double low = *std::max_element(soft.begin() + 1000, soft.end());
  REQUIRE(high > 0.1);
  CHECK(std::abs(low / high - .051) < .001);
}

TEST_CASE("PSG MIDI pitch uses the native AY and Sega clock dividers", "[emulation][psg]")
{
  for(auto model : {Model::Ay8910, Model::Ym2149, Model::SegaYm7101}) {
    CAPTURE(int(model));
    Engine e{48000, modelSettings(model)};
    send(e, {0x90, 69, 127});
    const auto audio = render(e, 16000);
    const auto [lo, hi] = std::minmax_element(audio.begin() + 1000, audio.end());
    const double center = (*lo + *hi) * .5;
    std::vector<double> edges;
    for(std::size_t i = 1001; i < audio.size(); ++i)
      if(audio[i - 1] <= center && audio[i] > center)
        edges.push_back(double(i - 1) + (center - audio[i - 1]) / (audio[i] - audio[i - 1]));
    REQUIRE(edges.size() > 100);
    const double hz = (edges.size() - 1) * 48000. / (edges.back() - edges.front());
    CHECK(std::abs(hz - 440.) < 4.);
  }
}

TEST_CASE("PSG sustain retriggers and channel resets keep FIFO lifetimes isolated", "[emulation][psg][qualification]")
{
  for(auto model : {Model::Ay8910, Model::Ym2149, Model::SegaYm7101})
  {
    auto settings = modelSettings(model);
    Engine engine{48000., settings};
    send(engine, {0xb0, 64, 127});
    send(engine, {0x90, 60, 100}); send(engine, {0x80, 60, 0});
    send(engine, {0x90, 60, 110}); send(engine, {0x91, 60, 100});
    send(engine, {0xb0, 64, 0}); CHECK(engine.activeVoices() == 2);
    send(engine, {0xb0, 123, 0}); CHECK(engine.activeVoices() == 1);
    send(engine, {0x80, 60, 0}); CHECK(engine.activeVoices() == 1);
    send(engine, {0x81, 60, 0}); CHECK(engine.activeVoices() == 0);
    send(engine, {0x90, 60, 100}); engine.panic();
    send(engine, {0x90, 60, 100}); send(engine, {0x80, 60, 0});
    CHECK(engine.activeVoices() == 0);
  }
}

TEST_CASE("PSG live register edits preserve voices and phase at supported rate limits", "[emulation][psg][qualification]")
{
  for(auto model : {Model::Ay8910, Model::Ym2149, Model::SegaYm7101})
    for(double rate : {8000., 192000., 384000.})
    {
      CAPTURE(int(model), rate);
      auto settings = modelSettings(model);
      Engine whole{rate, settings}, split{rate, settings};
      send(whole, {0x90, 60, 100}); send(split, {0x90, 60, 100});
      CHECK(render(whole, 513, 513) == render(split, 513, 1));
      settings.channels[0].level = 8; settings.noisePeriod = 7;
      settings.channels[0].noise = model != Model::SegaYm7101;
      REQUIRE(whole.configure(settings)); REQUIRE(split.configure(settings));
      CHECK(whole.activeVoices() == 1); CHECK(split.activeVoices() == 1);
      CHECK(render(whole, 1025, 1025) == render(split, 1025, 37));
    }
}

TEST_CASE("PSG valid MIDI clears transient errors without hiding configuration failures", "[emulation][psg]")
{
  for(auto model : {Model::Ay8910, Model::Ym2149, Model::SegaYm7101}) {
    auto settings = modelSettings(model);
    Engine engine{48000, settings};
    const std::array<uint8_t, 2> malformed{0x90, 60};
    engine.midi(malformed); REQUIRE(engine.error());
    send(engine, {0x90, 60, 127});
    CHECK(engine.activeVoices() == 1);
    auto invalid = settings; invalid.clock = 0;
    CHECK_FALSE(engine.configure(invalid));
    const auto* preparationError = engine.error();
    const std::array<uint8_t, 3> off{0x80, 60, 0};
    engine.midi(off);
    CHECK(engine.error() == preparationError);
    REQUIRE(engine.configure(settings));
    CHECK(engine.error() == nullptr);
    engine.midi(malformed); REQUIRE(engine.error());
    REQUIRE(engine.configure(settings));
    CHECK(engine.error() != nullptr);
    send(engine, {0x90, 64, 127});
  }
}

TEST_CASE("PSG mono lines restore held notes with their channel and velocity", "[emulation][psg][allocation]")
{
  for(auto model : {Model::Ay8910, Model::Ym2149, Model::SegaYm7101})
    for(auto mode : {Assignment::Mono, Assignment::Fixed}) {
      auto s = modelSettings(model); s.assignment = mode;
      Engine e{48000, s};
      send(e, {0xe0, 127, 127}); send(e, {0x90, 60, 55});
      const int period = e.registerValue(0);
      const int level = e.registerValue(model == Model::SegaYm7101 ? 1 : 8);
      const uint8_t other = mode == Assignment::Mono ? 2 : 0;
      send(e, {uint8_t(0x90 | other), 64, 127});
      send(e, {uint8_t(0x80 | other), 64, 0});
      CHECK(e.voiceNote(0) == 60);
      CHECK(e.registerValue(0) == period);
      CHECK(e.registerValue(model == Model::SegaYm7101 ? 1 : 8) == level);
      CHECK(e.activeVoices() == (mode == Assignment::Mono ? 3 : 1));
      CHECK(audible(render(e, 2048)));
      send(e, {0x81, 60, 0}); CHECK(e.voiceNote(0) == 60);
      send(e, {0xb0, 64, 127}); send(e, {0x80, 60, 0});
      send(e, {0xb1, 64, 0}); CHECK(e.voiceNote(0) == 60);
      send(e, {0xb0, 64, 0}); CHECK(e.activeVoices() == 0);
    }
}

TEST_CASE("PSG allocation changes retain controllers and old note-off debt", "[emulation][psg][allocation]")
{
  Settings s; Engine e{48000, s};
  send(e, {0xe0, 127, 127}); send(e, {0xb0, 7, 65}); send(e, {0x90, 60, 127});
  const int period = e.registerValue(0), level = e.registerValue(8);
  s.assignment = Assignment::Fixed; REQUIRE(e.configure(s));
  CHECK(e.activeVoices() == 0);
  send(e, {0x90, 60, 127}); send(e, {0x80, 60, 0});
  CHECK(e.activeVoices() == 1); CHECK(e.voiceNote(0) == 60);
  CHECK(e.registerValue(0) == period); CHECK(e.registerValue(8) == level);
  send(e, {0x80, 60, 0}); CHECK(e.activeVoices() == 0);
  send(e, {0x90, 64, 127}); send(e, {0xb0, 120, 0});
  send(e, {0x90, 64, 127}); send(e, {0x80, 64, 0});
  CHECK(e.activeVoices() == 1);
  send(e, {0x80, 64, 0}); CHECK(e.activeVoices() == 0);
}
