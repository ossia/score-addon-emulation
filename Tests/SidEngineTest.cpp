// SPDX-License-Identifier: GPL-3.0-or-later
#include <Emulation/Sid/Engine.hpp>
#include <Emulation/Volume.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>
namespace
{
using namespace Emulation::Sid;
void send(Engine& e, std::initializer_list<uint8_t> b) { e.midi({b.begin(), b.size()}); }
std::vector<double> render(Engine& e, std::size_t frames, std::size_t block = 256)
{
  std::vector<double> left(frames), right(frames);
  for(std::size_t i = 0; i < frames;)
  {
    const auto n = std::min(block, frames - i);
    e.render(left.data() + i, right.data() + i, n); i += n;
  }
  REQUIRE(left == right);
  REQUIRE(std::all_of(left.begin(), left.end(), [](double x) { return std::isfinite(x); }));
  return left;
}
}
TEST_CASE("SID semantic state rejects invalid register values without changing the live patch", "[emulation][sid]")
{
  Patch p;
  p.oscillators[0].waveform = 1;
  p.oscillators[0].ring = true;
  REQUIRE(decodePatch(encodePatch(p)) == p);
  Engine e{48000., p};
  send(e, {0x90, 60, 100});
  REQUIRE(e.activeVoices() == 3);
  const auto before = e.patch();
  p.oscillators[0].pulseWidth = 4096;
  CHECK_FALSE(e.setPatch(p));
  CHECK(e.lastError());
  CHECK(e.patch() == before);
  CHECK(e.activeVoices() == 3);
  CHECK_THROWS_AS(encodePatch(p), std::runtime_error);
  auto data = encodePatch(before);
  data[0] = 2;
  CHECK_THROWS_AS(decodePatch(data), std::runtime_error);
  data.pop_back();
  CHECK_THROWS_AS(decodePatch(data), std::runtime_error);
  p = before; p.volume = std::numeric_limits<double>::quiet_NaN();
  CHECK(validate(p));
}
TEST_CASE("SID allocation has explicit capacity, last-note priority and pedal ownership", "[emulation][sid]")
{
  auto p = defaultPatch();
  Engine mono{48000., p};
  send(mono, {0x90, 60, 100}); send(mono, {0x91, 64, 100});
  CHECK(mono.voiceNote(0) == 64);
  send(mono, {0xb1, 64, 127}); send(mono, {0x81, 64, 0});
  CHECK(mono.voiceNote(2) == 64);
  send(mono, {0xb0, 64, 0});
  CHECK(mono.voiceNote(2) == 64);
  send(mono, {0xb1, 64, 0});
  CHECK(mono.voiceNote(2) == 60);
  send(mono, {0x90, 60, 0}); CHECK(mono.activeVoices() == 0);
  p.allocation = Allocation::Poly;
  for(auto& o : p.oscillators) { o.sync = true; o.ring = true; }
  Engine poly{48000., p};
  for(uint8_t n : {60, 62, 64, 65}) send(poly, {0x90, n, 100});
  CHECK(poly.activeVoices() == 3);
  CHECK(poly.voiceNote(0) == 65);
  for(int v = 0; v < 3; ++v) CHECK((poly.voiceControl(v) & 6) == 6);
  send(poly, {0x80, 60, 0}); CHECK(poly.activeVoices() == 3);
  send(poly, {0x80, 65, 0}); CHECK(poly.activeVoices() == 2);
  send(poly, {0x90, 62, 127}); CHECK(poly.activeVoices() == 2); // repeated key coalesces
  send(poly, {0xb0, 64, 127}); send(poly, {0xb0, 123, 0});
  CHECK(poly.activeVoices() == 2);
  send(poly, {0xb0, 121, 0}); CHECK(poly.activeVoices() == 0);
}
TEST_CASE("SID fixed channels preserve ring and sync relationships and reject other channels", "[emulation][sid]")
{
  Patch p;
  p.allocation = Allocation::Fixed;
  p.oscillators[0].ring = true;
  p.oscillators[1].sync = true;
  Engine e{48000., p};
  send(e, {0x92, 55, 100}); send(e, {0x90, 60, 100}); send(e, {0x91, 67, 100});
  CHECK(e.voiceNote(0) == 60); CHECK(e.voiceNote(1) == 67); CHECK(e.voiceNote(2) == 55);
  CHECK((e.voiceControl(0) & 4) == 4); CHECK((e.voiceControl(1) & 2) == 2);
  send(e, {0x93, 72, 100}); CHECK(e.lastError()); CHECK(e.activeVoices() == 3);
  send(e, {0xb1, 120, 0}); CHECK(e.activeVoices() == 2); CHECK(e.voiceControl(1) == 8);
  send(e, {0xb0, 7, 80}); CHECK(e.voiceControl(1) == 8); // CC7 cannot revive All Sound Off
  CHECK(e.voiceNote(0) == 60); CHECK(e.voiceNote(2) == 55);
}
TEST_CASE("SID chip clocks and resampler are continuous across partitioning and volume automation", "[emulation][sid]")
{
  for(auto model : {Model::Mos6581, Model::Csg8580})
    for(auto clock : {Clock::Pal, Clock::Ntsc})
    {
      auto p = defaultPatch(); p.model = model; p.clock = clock;
      Engine reference{48000., p}, split{48000., p};
      send(reference, {0x90, 69, 100}); send(split, {0x90, 69, 100});
      auto a = render(reference, 4096, 4096), b = render(split, 4096, 37);
      REQUIRE(a == b);
      CHECK(std::any_of(a.begin(), a.end(), [](double v) { return std::abs(v) > .001; }));
      CHECK(reference.clocks() == split.clocks());
      const double nominal = clock == Clock::Pal ? 985248. : 1022730.;
      CHECK(std::abs(double(reference.clocks()) - 4096. * nominal / 48000.) < 100.);
      p.volume = 0.; REQUIRE(split.setPatch(p));
      auto muted = render(split, 1024, 17);
      render(reference, 1024);
      CHECK(std::all_of(muted.begin(), muted.end(), [](double x) { return x == 0.; }));
      CHECK(split.activeVoices() == 3);
      CHECK(split.clocks() == reference.clocks());
      p.volume = .25; REQUIRE(split.setPatch(p));
      a = render(reference, 1024); b = render(split, 1024);
      for(std::size_t i = 0; i < a.size(); ++i) CHECK(b[i] == .25 * a[i]);
      for(double gain : {2., double(Emulation::MaxVolume), 1.}) {
        p.volume = gain; REQUIRE(split.setPatch(p));
        REQUIRE(decodePatch(encodePatch(p)) == p);
        CHECK(split.activeVoices() == 3);
        a = render(reference, 1024); b = render(split, 1024);
        for(std::size_t i = 0; i < a.size(); ++i) CHECK(b[i] == gain * a[i]);
      }
      for(double gain : {-1., double(Emulation::MaxVolume) + 1.,
          std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto invalid = p;
        CHECK_FALSE(setControl(invalid, Volume, gain));
        CHECK(invalid == p);
        invalid.volume = gain;
        CHECK_FALSE(split.setPatch(invalid));
        CHECK(split.patch() == p);
        CHECK_THROWS_AS(encodePatch(invalid), std::runtime_error);
        CHECK_THROWS_AS((Engine{48000., invalid}), std::runtime_error);
        CHECK(render(reference, 1024) == render(split, 1024));
      }
    }
}
TEST_CASE("SID filter character is instance-local and pitch never wraps the hardware register", "[emulation][sid]")
{
  Patch p;
  p.allocation = Allocation::Fixed;
  for(auto& oscillator : p.oscillators) oscillator.filterRoute = true;
  p.cutoff = 900; p.resonance = 5;
  Engine a{44100., p}, reference{44100., p};
  auto otherPatch = p; otherPatch.filterCurve = 95;
  Engine other{44100., otherPatch};
  send(a, {0x90, 69, 100}); send(reference, {0x90, 69, 100}); send(other, {0x90, 60, 100});
  render(other, 512);
  CHECK(render(a, 2048, 63) == render(reference, 2048, 128));
  const auto frequency = a.voiceFrequency(0);
  CHECK(std::abs(double(frequency) - 440. * 16777216. / 985248.) < 1.);
  send(a, {0xe0, 0, 96}); // +1 semitone with fixed +/-2 bend range
  CHECK(a.voiceFrequency(0) > frequency);
  send(a, {0xb0, 121, 0}); CHECK(a.voiceFrequency(0) == frequency);
  send(a, {0x90, 127, 127}); CHECK(a.lastError()); CHECK(a.activeVoices() == 0);
  CHECK(a.voiceFrequency(0) == frequency); // rejection, not modulo wrap or a false pitch
  send(a, {0xff}); CHECK(a.activeVoices() == 0);
  const auto before = a.clocks();
  const auto silence = render(a, 1024);
  CHECK(a.clocks() > before);
  CHECK(std::all_of(silence.begin(), silence.end(), [](double x) { return x == 0.; }));
  CHECK_THROWS_AS(Engine(7999., p), std::runtime_error);
}

TEST_CASE("SID modulation renders continuously in every allocation with the physical previous oscillator", "[emulation][sid][sync]")
{
  for(auto allocation : {Allocation::Mono, Allocation::Poly, Allocation::Fixed})
    for(auto model : {Model::Mos6581, Model::Csg8580})
      for(auto field : {Sync, Ring})
      for(int target = 0; target < 3; ++target)
      {
        CAPTURE(allocation, model, target, field);
        Patch p;
        p.model = model; p.allocation = allocation;
        for(auto& o : p.oscillators)
        {
          o.waveform = 0; o.attack = 0; o.sustain = 15;
        }
        p.oscillators[target].waveform = field == Sync ? 2 : 1;
        p.oscillators[(target + 2) % 3].semitones = 7;
        auto synced = p;
        REQUIRE(setControl(synced, oscillatorControl(target, field), 1.));
        CHECK(decodePatch(encodePatch(synced)) == synced);
        Engine off{48000., p}, live{48000., p}, reference{48000., synced}, split{48000., synced};
        for(auto* e : {&off, &live, &reference, &split})
        {
          if(allocation == Allocation::Mono) send(*e, {0x90, 60, 100});
          else for(uint8_t channel = 0; channel < 3; ++channel)
            send(*e, {uint8_t(0x90 | channel), 60, 100});
        }
        REQUIRE(live.setPatch(synced));
        const int mask = field == Sync ? 2 : 4;
        CHECK((live.voiceControl(target) & mask) == mask);
        const auto expected = render(reference, 8192);
        CHECK(render(live, 8192) == expected);
        CHECK(render(split, 8192, 17) == expected);
        CHECK(render(off, 8192) != expected);
        CHECK(live.clocks() == reference.clocks());
        CHECK(split.clocks() == reference.clocks());
        REQUIRE(live.setPatch(p));
        CHECK((live.voiceControl(target) & mask) == 0);
        CHECK(live.activeVoices() == 3);
        render(live, 1024);
      }
}

TEST_CASE("SID coalesced retriggers retain every outstanding note lifetime", "[emulation][sid][qualification]")
{
  for(auto allocation : {Allocation::Mono, Allocation::Poly, Allocation::Fixed})
  {
    auto patch = defaultPatch(); patch.allocation = allocation;
    Engine engine{48000., patch};
    const int voices = allocation == Allocation::Mono ? 3 : 1;
    send(engine, {0x90, 60, 100}); send(engine, {0x90, 60, 110});
    CHECK(engine.activeVoices() == voices);
    send(engine, {0x80, 60, 0}); CHECK(engine.activeVoices() == voices);
    send(engine, {0xb0, 64, 127});
    send(engine, {0x90, 60, 0}); CHECK(engine.activeVoices() == voices);
    send(engine, {0x90, 60, 127});
    send(engine, {0xb0, 64, 0}); CHECK(engine.activeVoices() == voices);
    send(engine, {0x80, 60, 0}); CHECK(engine.activeVoices() == 0);
    send(engine, {0x80, 60, 0}); CHECK(engine.activeVoices() == 0);
    send(engine, {0x90, 60, 100}); send(engine, {0x90, 60, 100});
    send(engine, {0xb0, 123, 0}); CHECK(engine.activeVoices() == 0);
    send(engine, {0x90, 60, 100}); send(engine, {0x80, 60, 0});
    CHECK(engine.activeVoices() == voices); // Late release from before CC123.
    send(engine, {0x80, 60, 0}); CHECK(engine.activeVoices() == voices);
    send(engine, {0x80, 60, 0}); CHECK(engine.activeVoices() == 0);
    send(engine, {0x90, 60, 100}); engine.panic();
    send(engine, {0x90, 60, 100}); send(engine, {0x80, 60, 0});
    CHECK(engine.activeVoices() == 0);
  }
}

TEST_CASE("SID stolen repeated keys cannot release a later retrigger", "[emulation][sid][qualification]")
{
  auto patch = defaultPatch(); patch.allocation = Allocation::Poly;
  Engine engine{48000., patch};
  for(uint8_t note : {60, 62, 64, 65, 60}) send(engine, {0x90, note, 100});
  REQUIRE(engine.voiceNote(1) == 60);
  send(engine, {0x80, 60, 0});
  CHECK(engine.activeVoices() == 3);
  CHECK(engine.voiceNote(1) == 60);
  send(engine, {0x80, 60, 0});
  CHECK(engine.activeVoices() == 2);
}

TEST_CASE("SID live patch edits and sample-rate extremes preserve clock partitioning", "[emulation][sid][qualification]")
{
  for(double rate : {8000., 44100., 96000., 192000.})
  {
    CAPTURE(rate);
    auto patch = defaultPatch();
    Engine whole{rate, patch}, split{rate, patch};
    send(whole, {0x90, 60, 100}); send(split, {0x90, 60, 100});
    CHECK(render(whole, 513, 513) == render(split, 513, 1));
    patch.oscillators[0].pulseWidth = 1700;
    patch.oscillators[1].sync = true; patch.oscillators[2].ring = true;
    patch.cutoff = 321; patch.resonance = 8;
    REQUIRE(whole.setPatch(patch)); REQUIRE(split.setPatch(patch));
    CHECK(whole.activeVoices() == 3); CHECK(split.activeVoices() == 3);
    CHECK(render(whole, 1025, 1025) == render(split, 1025, 37));
    CHECK(whole.clocks() == split.clocks());
  }
}

TEST_CASE("SID held and sustained notes recover rendered sound after invalid pitch", "[emulation][sid][qualification][pitch]")
{
  const auto span = [](const std::vector<double>& audio) {
    const auto [lo, hi] = std::minmax_element(audio.begin(), audio.end());
    return *hi - *lo;
  };
  for(auto model : {Model::Mos6581, Model::Csg8580})
    for(auto allocation : {Allocation::Mono, Allocation::Poly, Allocation::Fixed})
      for(bool pedal : {false, true})
        for(bool tuning : {false, true})
        {
          CAPTURE(model, allocation, pedal, tuning);
          Patch patch; patch.model = model; patch.allocation = allocation;
          for(auto& oscillator : patch.oscillators)
          {
            oscillator.attack = oscillator.decay = oscillator.release = 0;
            oscillator.sustain = 15;
          }
          Engine engine{48000., patch}, reference{48000., patch};
          const int voices = allocation == Allocation::Mono ? 3 : 1;
          const auto start = [&](Engine& target) {
            if(pedal) send(target, {0xb0, 64, 127});
            // MIDI 106 is in range, but +2 semitones exceeds the PAL register.
            send(target, {0x90, 106, 110});
            if(pedal) send(target, {0x80, 106, 0});
          };
          const auto release = [](Engine& target) {
            send(target, {0xb0, 64, 0});
            send(target, {0x80, 106, 0});
          };
          const auto setPitch = [&](Engine& target, bool invalid) {
            if(tuning)
            {
              auto next = patch;
              for(auto& oscillator : next.oscillators) oscillator.semitones = invalid ? 2 : 0;
              REQUIRE(target.setPatch(next));
            }
            else if(invalid) send(target, {0xe0, 127, 127});
            else send(target, {0xe0, 0, 64});
          };
          start(engine); start(reference);
          REQUIRE(engine.activeVoices() == voices);
          const auto initial = render(engine, 4096);
          REQUIRE(span(initial) > .001);
          REQUIRE(initial == render(reference, 4096));

          // Out-of-range pitch must act like a physical gate-off. Compare the
          // entire normal-release waveform, including the pinned core's 8-bit
          // output noise, ADSR and analog/filter history; it is not digital zero.
          release(reference);
          setPitch(engine, true); setPitch(reference, true);
          CHECK(engine.lastError());
          CHECK(engine.activeVoices() == 0);
          for(int voice = 0; voice < 3; ++voice) CHECK((engine.voiceControl(voice) & 1) == 0);
          CHECK(render(engine, 10240) == render(reference, 10240));

          setPitch(engine, false); setPitch(reference, false);
          CHECK_FALSE(engine.lastError());
          start(reference);
          CHECK(engine.activeVoices() == voices);
          const auto recovered = render(engine, 4096);
          CHECK(span(recovered) > .001);
          CHECK(recovered == render(reference, 4096));

          // Release while invalid must match ordinary release, not restore a
          // gate whose key and pedal lifetime already ended on valid pitch.
          release(reference);
          setPitch(engine, true); setPitch(reference, true);
          release(engine);
          CHECK_FALSE(engine.lastError());
          setPitch(engine, false); setPitch(reference, false);
          CHECK(engine.activeVoices() == 0);
          for(int voice = 0; voice < 3; ++voice) CHECK((engine.voiceControl(voice) & 1) == 0);
          CHECK(render(engine, 10240) == render(reference, 10240));
        }
}

TEST_CASE("SID initially invalid and stolen notes retain logical gate ownership", "[emulation][sid][qualification][pitch]")
{
  for(auto model : {Model::Mos6581, Model::Csg8580})
  {
    Patch patch; patch.model = model; patch.allocation = Allocation::Poly;
    for(auto& oscillator : patch.oscillators)
    {
      oscillator.attack = oscillator.decay = oscillator.release = 0;
      oscillator.sustain = 15;
    }
    Engine engine{48000., patch}, reference{48000., patch};
    send(engine, {0xe0, 127, 127});
    send(engine, {0x90, 106, 110});
    CHECK(engine.activeVoices() == 0);
    send(engine, {0xe0, 0, 64});
    send(reference, {0x90, 106, 110});
    REQUIRE(engine.activeVoices() == 1);
    const auto audio = render(engine, 4096);
    const auto [lo, hi] = std::minmax_element(audio.begin(), audio.end());
    CHECK(*hi - *lo > .001);
    REQUIRE(audio == render(reference, 4096));
    for(auto* target : {&engine, &reference})
    {
      send(*target, {0xe0, 127, 127});
      for(uint8_t note : {60, 64, 67}) send(*target, {0x90, note, 110});
    }
    REQUIRE(engine.voiceNote(0) == 67); // oldest logical gate was stolen
    for(uint8_t note : {60, 64, 67}) send(engine, {0x80, note, 0});
    send(reference, {0xb0, 123, 0}); // independent channel-wide normal release
    send(engine, {0xe0, 0, 64}); send(reference, {0xe0, 0, 64});
    CHECK(engine.activeVoices() == 0);
    CHECK(engine.voiceNote(0) == 67);
    for(int voice = 0; voice < 3; ++voice) CHECK((engine.voiceControl(voice) & 1) == 0);
    CHECK(render(engine, 10240) == render(reference, 10240));
  }
}

TEST_CASE("SID error recovery preserves unrelated rejected state and out-of-range voices", "[emulation][sid][errors]")
{
  Patch patch; patch.allocation = Allocation::Fixed;
  Engine engine{48000., patch};
  send(engine, {0x93, 60, 100});
  REQUIRE(engine.lastError());
  REQUIRE(engine.setPatch(patch));
  CHECK(engine.lastError()); // An edit is not recovery from rejected MIDI.
  send(engine, {0x90, 106, 100});
  CHECK_FALSE(engine.lastError());
  send(engine, {0x91, 106, 100});
  send(engine, {0xe0, 127, 127});
  send(engine, {0xe1, 127, 127});
  REQUIRE(engine.lastError());
  patch.volume = .5;
  REQUIRE(engine.setPatch(patch));
  CHECK(engine.lastError());
  send(engine, {0xe0, 0, 64});
  CHECK(engine.lastError()); // Channel 2 is still outside the hardware range.
  CHECK(engine.activeVoices() == 1);
  send(engine, {0xe1, 0, 64});
  CHECK_FALSE(engine.lastError());
  CHECK(engine.activeVoices() == 2);

  auto invalid = patch; invalid.cutoff = 2048;
  CHECK_FALSE(engine.setPatch(invalid));
  send(engine, {0x80, 106, 0});
  CHECK(engine.lastError()); // Valid MIDI cannot clear a rejected patch.
  REQUIRE(engine.setPatch(patch));
  CHECK_FALSE(engine.lastError());
  send(engine, {0x90, 60});
  REQUIRE(engine.lastError());
  send(engine, {0x90, 60, 100});
  CHECK_FALSE(engine.lastError());
}

TEST_CASE("SID allocation switches retain controllers and reject stale note-offs", "[emulation][sid][allocation]")
{
  auto p = defaultPatch(); p.allocation = Allocation::Poly;
  Engine e{48000., p};
  send(e, {0xe0, 127, 127}); send(e, {0x90, 60, 100});
  const auto frequency = e.voiceFrequency(0);
  p.allocation = Allocation::Fixed; REQUIRE(e.setPatch(p));
  CHECK(e.activeVoices() == 0);
  send(e, {0x90, 60, 100}); send(e, {0x80, 60, 0});
  CHECK(e.activeVoices() == 1); CHECK(e.voiceFrequency(0) == frequency);
  send(e, {0x80, 60, 0}); CHECK(e.activeVoices() == 0);
  send(e, {0x90, 64, 100}); send(e, {0xb0, 120, 0});
  send(e, {0x90, 64, 100}); send(e, {0x80, 64, 0});
  CHECK(e.activeVoices() == 1);
  send(e, {0x80, 64, 0}); CHECK(e.activeVoices() == 0);
  // The channel becomes unmapped, but its old note-off must still be drained.
  p.allocation = Allocation::Poly; REQUIRE(e.setPatch(p));
  send(e, {0x94, 67, 100});
  p.allocation = Allocation::Fixed; REQUIRE(e.setPatch(p));
  send(e, {0x84, 67, 0});
  p.allocation = Allocation::Mono; REQUIRE(e.setPatch(p));
  send(e, {0x94, 67, 100}); send(e, {0x84, 67, 0});
  CHECK(e.activeVoices() == 0);
}
