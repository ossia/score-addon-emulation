// SPDX-License-Identifier: GPL-3.0-or-later
#include <Emulation/Wurli/Engine.hpp>
#include <Emulation/Wurli/Parameters.hpp>
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
using namespace Emulation::Wurli;
void send(Engine& e, std::initializer_list<uint8_t> bytes) { e.midi({bytes.begin(), bytes.size()}); }
std::vector<double> render(Engine& e, std::size_t count, std::size_t block = 256)
{
  std::vector<double> left(count), right(count);
  for(std::size_t i = 0; i < count;) {
    const auto n = std::min(block, count - i);
    e.render(left.data() + i, right.data() + i, n); i += n;
  }
  REQUIRE(left == right);
  REQUIRE(std::all_of(left.begin(), left.end(), [](double v) { return std::isfinite(v); }));
  REQUIRE_FALSE(e.error());
  return left;
}
bool audible(const std::vector<double>& v)
{ return std::any_of(v.begin(), v.end(), [](double x) { return std::abs(x) > 1e-6; }); }
}
TEST_CASE("Wurlitzer physical circuit is continuous across render partitions and host rates", "[emulation][wurli]")
{
  for(double rate : {44100., 48000., 88200., 96000., 192000.}) {
    CAPTURE(rate);
    Engine whole{rate}, split{rate};
    for(auto* e : {&whole, &split}) {
      send(*e, {0x90, 48, 104}); send(*e, {0x90, 64, 91}); send(*e, {0x90, 71, 83});
    }
    auto a = render(whole, 4096, 4096), b = render(split, 4096, 37);
    CHECK(a == b); CHECK(audible(a)); CHECK(whole.sampleRate() == rate);
    for(auto* e : {&whole, &split}) { send(*e, {0xb0, 64, 127}); send(*e, {0x80, 48, 0}); }
    CHECK(render(whole, 617, 617) == render(split, 617, 1));
    for(auto* e : {&whole, &split}) send(*e, {0xb0, 64, 0});
    CHECK(render(whole, 2048, 2048) == render(split, 2048, 113));
  }
}
TEST_CASE("Wurlitzer output Volume is linear exact mute without stopping the model", "[emulation][wurli]")
{
  Settings s; s.volume = 1.f;
  Engine reference{48000., s}, adjusted{48000., s};
  for(auto* e : {&reference, &adjusted}) send(*e, {0x90, 60, 100});
  CHECK(render(reference, 512) == render(adjusted, 512));
  for(float gain : {.25f, 4.5f, Emulation::MaxVolume, 0.f, 1.f}) {
    REQUIRE(setControl(s, Volume, gain)); REQUIRE(adjusted.configure(s));
    const auto a = render(reference, 1024), b = render(adjusted, 1024, 31);
    REQUIRE(audible(a));
    for(std::size_t i = 0; i < a.size(); ++i) CHECK(b[i] == a[i] * gain);
    CHECK(adjusted.heldVoices() == 1);
  }
}
TEST_CASE("Wurlitzer sustain and note lifetimes belong to their MIDI channel and original pitch", "[emulation][wurli]")
{
  Engine e{48000.};
  send(e, {0x90, 60, 100}); send(e, {0x91, 60, 100});
  send(e, {0xb0, 64, 127}); send(e, {0x80, 60, 0});
  CHECK(e.heldVoices() == 1); CHECK(e.sustainedVoices() == 1);
  send(e, {0xb1, 64, 0}); CHECK(e.sustainedVoices() == 1);
  send(e, {0x91, 60, 0}); CHECK(e.heldVoices() == 0);
  send(e, {0xb0, 121, 0}); CHECK(e.sustainedVoices() == 0);
  e.reset();
  for(uint8_t note : {0, 32, 97, 127}) {
    send(e, {0x90, note, 100}); send(e, {0x80, note, 0});
  }
  CHECK(e.heldVoices() == 0);
  e.reset();
  send(e, {0x90, 60, 80}); send(e, {0x90, 60, 95});
  CHECK(e.heldVoices() == 1); CHECK(e.activeVoices() == 1);
  send(e, {0x80, 60, 0}); CHECK(e.heldVoices() == 1);
  send(e, {0x80, 60, 0}); CHECK(e.heldVoices() == 0);
  e.reset();
  send(e, {0x90, 96, 100}); send(e, {0x80, 96, 0});
  CHECK(e.heldVoices() == 0); // The real top five reeds have no damper.
  CHECK(audible(render(e, 2048)));
}
TEST_CASE("Wurlitzer voice stealing is bounded and stop reset cannot leave ringing reeds", "[emulation][wurli]")
{
  Engine e{48000.};
  for(int n = 0; n < 96; ++n) send(e, {uint8_t(0x90 | (n % 16)), uint8_t(33 + n % 64), 100});
  CHECK(e.activeVoices() == 64); CHECK(audible(render(e, 1024)));
  e.panic(); CHECK(e.activeVoices() == 0);
  auto zero = render(e, 4096);
  CHECK(std::all_of(zero.begin(), zero.end(), [](double v) { return v == 0.; }));
  send(e, {0x90, 60, 100}); render(e, 256); send(e, {0xb0, 120, 0});
  CHECK(e.activeVoices() == 0); CHECK_FALSE(audible(render(e, 1024)));
  e.reset(); send(e, {0x90, 60, 100}); const auto first = render(e, 2048);
  send(e, {0xff}); send(e, {0x90, 60, 100}); CHECK(render(e, 2048) == first);
}
TEST_CASE("Wurlitzer model controls affect real physical rendering and reject invalid state", "[emulation][wurli]")
{
  Settings s; Engine e{48000., s};
  send(e, {0x90, 57, 50}); const auto normal = render(e, 2048);
  e.reset(); s.velocityCurve = 3; REQUIRE(e.configure(s));
  send(e, {0x90, 57, 50}); CHECK(render(e, 2048) != normal);
  const auto before = s;
  for(float invalid : {-1.f, Emulation::MaxVolume + .01f,
      std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
    CHECK_FALSE(setControl(s, Volume, invalid)); CHECK(s == before);
    auto rejected = s; rejected.volume = invalid;
    CHECK(validate(rejected)); CHECK_FALSE(e.configure(rejected)); CHECK(e.error());
  }
  s = before; REQUIRE(e.configure(s));
  CHECK_FALSE(setControl(s, VelocityCurve, 1.5)); CHECK_FALSE(setControl(s, Correction, .2));
  CHECK_FALSE(setControl(s, VelocityCurve, 4.));
  CHECK_FALSE(setControl(s, VoiceAllocation, 1.5)); CHECK_FALSE(setControl(s, VoiceAllocation, 3.));
  auto invalidAllocation = s; invalidAllocation.voiceAllocation = Allocation(-1);
  CHECK_FALSE(e.configure(invalidAllocation)); REQUIRE(e.configure(s));
  CHECK_FALSE(setControl(s, Amplifier, 2.)); CHECK(s == before);
  CHECK_THROWS_AS(Engine(22050.), std::invalid_argument);
  CHECK_THROWS_AS(Engine(std::numeric_limits<double>::infinity()), std::invalid_argument);
}
TEST_CASE("Wurlitzer unsupported bend is not a fake transposition and controllers survive unrelated gain edits", "[emulation][wurli]")
{
  Settings s; s.volume = 1.f; Engine a{48000., s}, b{48000., s};
  for(auto* e : {&a, &b}) { send(*e, {0x90, 60, 100}); send(*e, {0xb0, 1, 110}); }
  send(b, {0xe0, 127, 127}); send(b, {0xd0, 120}); send(b, {0xc0, 17});
  CHECK(render(a, 2048) == render(b, 2048));
  s.volume = .5f; REQUIRE(b.configure(s));
  auto x = render(a, 2048), y = render(b, 2048);
  for(std::size_t i = 0; i < x.size(); ++i) CHECK(y[i] == x[i] * .5);
  // All notes off honors the pedal, controller reset releases it.
  send(b, {0xb0, 64, 127}); send(b, {0xb0, 123, 0});
  CHECK(b.heldVoices() == 0); CHECK(b.sustainedVoices() == 1);
  send(b, {0xb0, 121, 0}); CHECK(b.sustainedVoices() == 0);
}
TEST_CASE("Wurlitzer amplifier speaker tremolo and correction controls alter physical audio", "[emulation][wurli]")
{
  Settings base; base.tremolo = 0.f;
  Engine reference{48000., base};
  send(reference, {0x90, 60, 95}); const auto dry = render(reference, 4096);
  for(int c : {Amplifier, Tremolo, Speaker, Correction}) {
    CAPTURE(c);
    auto changed = base;
    REQUIRE(setControl(changed, c, c == Amplifier ? .25 : 1.));
    Engine candidate{48000., changed};
    send(candidate, {0x90, 60, 95});
    const auto audio = render(candidate, 4096);
    CHECK(audible(audio)); CHECK(audio != dry);
  }
}
TEST_CASE("Wurlitzer stolen held repetitions retain FIFO Note Off ownership", "[emulation][wurli]")
{
  Engine e{48000.};
  send(e, {0x90, 60, 100}); send(e, {0x90, 60, 90});
  for(uint8_t note = 61; note <= 123; ++note) send(e, {0x90, note, 100});
  REQUIRE(e.heldVoices() == 64);
  send(e, {0x91, 40, 100}); // Steals the coalesced 60, retaining both pending offs.
  REQUIRE(e.heldVoices() == 64);
  send(e, {0x80, 60, 0}); CHECK(e.heldVoices() == 64);
  send(e, {0x90, 60, 95}); // New lifetime after the old reed was stolen.
  send(e, {0x80, 60, 0}); CHECK(e.heldVoices() == 64);
  send(e, {0x80, 60, 0}); CHECK(e.heldVoices() == 63);
  // A stolen lifetime remains a tombstone even after the fading voice disappears.
  e.reset();
  for(int i = 0; i < 65; ++i) send(e, {0x90, 60, 100});
  render(e, 1024);
  CHECK(e.heldVoices() == 1);
  send(e, {0x80, 60, 0}); CHECK(e.heldVoices() == 1);
  for(int i = 0; i < 64; ++i) send(e, {0x80, 60, 0});
  CHECK(e.heldVoices() == 0);
}
TEST_CASE("Wurlitzer channel All Sound Off preserves another idle channel's pedal", "[emulation][wurli]")
{
  Engine e{48000.};
  send(e, {0xb1, 64, 127});
  send(e, {0xb0, 120, 0}); // No active voices: the circuit may reset, controllers may not.
  send(e, {0x91, 60, 100}); send(e, {0x81, 60, 0});
  CHECK(e.heldVoices() == 0); CHECK(e.sustainedVoices() == 1);
  send(e, {0xb1, 64, 0}); CHECK(e.sustainedVoices() == 0);
}
TEST_CASE("Wurlitzer exposes four distinct velocity curves with consistent MIDI selection", "[emulation][wurli]")
{
  Settings s; Engine e{48000., s};
  std::array<std::vector<double>, 4> audio;
  for(int curve = 0; curve < 4; ++curve) {
    s.velocityCurve = curve; REQUIRE(e.configure(s)); e.reset();
    send(e, {0x90, 60, 50}); audio[curve] = render(e, 2048);
    for(int previous = 0; previous < curve; ++previous) CHECK(audio[curve] != audio[previous]);
  }
  s.velocityCurve = 0; REQUIRE(e.configure(s));
  for(int curve = 0; curve < 4; ++curve) {
    e.reset(); send(e, {0xb0, 75, uint8_t(curve)});
    send(e, {0x90, 60, 50}); CHECK(render(e, 2048) == audio[curve]);
    e.reset(); send(e, {0xb0, 75, uint8_t(curve * 32 + 16)});
    send(e, {0x90, 60, 50}); CHECK(render(e, 2048) == audio[curve]);
  }
  s.velocityCurve = 4; CHECK_FALSE(e.configure(s));
}

TEST_CASE("Wurlitzer controller reset restores authored shared timbre without cutting channel tails", "[emulation][wurli][reset]")
{
  for(double rate : {48000., 96000.}) {
    CAPTURE(rate);
    Engine reset{rate}, explicitRestore{rate};
    Settings authored;
    authored.tremolo = 20 / 127.f;
    authored.speaker = 40 / 127.f;
    authored.correction = true;
    authored.velocityCurve = 2;
    authored.amplifier = .65f;
    authored.volume = .4f;
    for(auto* engine : {&reset, &explicitRestore}) {
      REQUIRE(engine->configure(authored));
      send(*engine, {0x90, 60, 90}); send(*engine, {0x91, 67, 75});
      send(*engine, {0xb0, 64, 127}); send(*engine, {0xb1, 64, 127});
      send(*engine, {0x80, 60, 0}); send(*engine, {0x81, 67, 0});
      send(*engine, {0xb0, 7, 48}); send(*engine, {0xb0, 11, 80});
      send(*engine, {0xb1, 7, 72}); send(*engine, {0xb1, 11, 96});
      // Controllers on channel 2 still target the shared physical instrument.
      send(*engine, {0xb1, 1, 120}); send(*engine, {0xb1, 70, 0});
      send(*engine, {0xb1, 71, 115}); send(*engine, {0xb1, 75, 1});
    }
    CHECK(render(reset, 513) == render(explicitRestore, 513));
    send(reset, {0xb0, 121, 0});
    send(explicitRestore, {0xb0, 7, 127}); send(explicitRestore, {0xb0, 11, 127});
    send(explicitRestore, {0xb0, 64, 0});
    send(explicitRestore, {0xb0, 1, 20}); send(explicitRestore, {0xb0, 70, 127});
    send(explicitRestore, {0xb0, 71, 40}); send(explicitRestore, {0xb0, 75, 2});
    CHECK(reset.sustainedVoices() == 1); // Only channel 1's pedal resets.
    CHECK(reset.activeVoices() == 2); // Its release tail is not a panic.
    const auto tail = render(reset, 1024, 17);
    CHECK(audible(tail));
    CHECK(tail == render(explicitRestore, 1024));
    for(auto* engine : {&reset, &explicitRestore}) {
      send(*engine, {0x90, 55, 42}); send(*engine, {0x91, 72, 108});
    }
    CHECK(render(reset, 4096) == render(explicitRestore, 4096, 31));
  }
}

TEST_CASE("Wurlitzer rejected MIDI recovers independently from rejected authored settings", "[emulation][wurli][errors]")
{
  Settings authored;
  Engine engine{48000., authored};
  send(engine, {0x90, 60});
  REQUIRE(engine.error());
  authored.volume = .5f;
  REQUIRE(engine.configure(authored));
  CHECK(engine.error());
  send(engine, {0x90, 60, 100});
  CHECK_FALSE(engine.error());
  CHECK(audible(render(engine, 512)));
  auto invalid = authored; invalid.speaker = 2.f;
  CHECK_FALSE(engine.configure(invalid));
  send(engine, {0xb0, 121, 0});
  CHECK(engine.error());
  REQUIRE(engine.configure(authored));
  CHECK_FALSE(engine.error());
  send(engine, {0x80, 60});
  REQUIRE(engine.error());
  send(engine, {0x80, 60, 0});
  CHECK_FALSE(engine.error());
}

TEST_CASE("Wurlitzer Mono uses global last-note priority and channel-owned fallback", "[emulation][wurli][allocation]")
{
  Settings s; s.voiceAllocation = Allocation::Mono;
  Engine e{48000., s};
  send(e, {0x90, 48, 40}); send(e, {0x91, 67, 110}); send(e, {0x92, 72, 80});
  CHECK(e.heldVoices() == 1); CHECK(e.activeVoices() == 1);
  send(e, {0x81, 72, 0}); CHECK(e.heldVoices() == 1); // Wrong owner.
  send(e, {0x82, 72, 0}); CHECK(e.heldVoices() == 1); // Back to channel 2.
  send(e, {0x80, 67, 0}); CHECK(e.heldVoices() == 1);
  send(e, {0x81, 67, 0}); CHECK(e.heldVoices() == 1); // Back to channel 1.
  send(e, {0xb1, 64, 127}); send(e, {0x80, 48, 0});
  CHECK(e.heldVoices() == 0); CHECK(e.sustainedVoices() == 0);
  CHECK(audible(render(e, 1024)));
  e.reset();
  send(e, {0x90, 48, 50}); send(e, {0x91, 67, 100});
  send(e, {0xb0, 64, 127}); send(e, {0x81, 67, 0}); send(e, {0x80, 48, 0});
  CHECK(e.heldVoices() == 0); CHECK(e.sustainedVoices() == 1);
  send(e, {0xb1, 64, 0}); CHECK(e.sustainedVoices() == 1);
  send(e, {0xb0, 64, 0}); CHECK(e.sustainedVoices() == 0);
}

TEST_CASE("Wurlitzer MIDI channel allocation supplies sixteen independent mono lines", "[emulation][wurli][allocation]")
{
  Settings s; s.voiceAllocation = Allocation::MidiChannel;
  Engine e{48000., s};
  for(uint8_t ch = 0; ch < 16; ++ch) {
    send(e, {uint8_t(0x90 | ch), 48, uint8_t(40 + ch)});
    send(e, {uint8_t(0x90 | ch), 67, 100});
  }
  CHECK(e.heldVoices() == 16); CHECK(e.activeVoices() == 16);
  for(uint8_t ch = 0; ch < 16; ++ch) send(e, {uint8_t(0x80 | ch), 67, 0});
  CHECK(e.heldVoices() == 16);
  send(e, {0xb0, 64, 127});
  for(uint8_t ch = 0; ch < 16; ++ch) send(e, {uint8_t(0x80 | ch), 48, 0});
  CHECK(e.heldVoices() == 0); CHECK(e.sustainedVoices() == 1);
  CHECK(audible(render(e, 2048)));
  send(e, {0xbf, 64, 0}); CHECK(e.sustainedVoices() == 1);
  send(e, {0xb0, 64, 0}); CHECK(e.sustainedVoices() == 0);
}

TEST_CASE("Wurlitzer allocation changes and channel cleanup retain old FIFO debt and controllers", "[emulation][wurli][allocation]")
{
  for(auto mode : {Allocation::Mono, Allocation::Poly, Allocation::MidiChannel}) {
    Settings s; Engine e{48000., s};
    send(e, {0xb0, 64, 127}); send(e, {0x90, 60, 80});
    // Always make an actual change, including when the requested mode is Poly.
    s.voiceAllocation = mode == Allocation::Poly ? Allocation::Mono : mode;
    REQUIRE(e.configure(s)); CHECK(e.activeVoices() == 0);
    s.voiceAllocation = mode; REQUIRE(e.configure(s));
    send(e, {0x90, 60, 90}); send(e, {0x90, 60, 100});
    CHECK(e.heldVoices() == 1); CHECK(e.activeVoices() == 1);
    send(e, {0x80, 60, 0}); CHECK(e.heldVoices() == 1);
    send(e, {0x80, 60, 0}); CHECK(e.heldVoices() == 1);
    send(e, {0x80, 60, 0}); CHECK(e.heldVoices() == 0); CHECK(e.sustainedVoices() == 1);
    for(uint8_t cc : {120, 123}) {
      e.reset();
      send(e, {0x90, 60, 80}); send(e, {0xb0, cc, 0});
      send(e, {0x90, 60, 100});
      send(e, {0x80, 60, 0}); CHECK(e.heldVoices() == 1);
      send(e, {0x80, 60, 0}); CHECK(e.heldVoices() == 0);
      render(e, 512);
    }
  }
}

TEST_CASE("Wurlitzer fallback preserves original velocity volume and expression owners", "[emulation][wurli][allocation]")
{
  for(auto mode : {Allocation::Mono, Allocation::MidiChannel}) {
    Settings s; s.voiceAllocation = mode;
    Engine a{48000., s}, b{48000., s};
    // Swapping channel identities and their controllers must not change the audio,
    // including fallback from a louder note to the original quiet strike.
    for(auto* e : {&a, &b}) send(*e, {0xb2, 64, 127});
    send(a, {0xb0, 7, 80}); send(a, {0xb0, 11, 64});
    send(b, {0xb1, 7, 80}); send(b, {0xb1, 11, 64});
    send(a, {0x90, 48, 42}); send(b, {0x91, 48, 42});
    const uint8_t otherA = mode == Allocation::Mono ? 0x91 : 0x90;
    const uint8_t otherB = mode == Allocation::Mono ? 0x90 : 0x91;
    send(a, {otherA, 72, 110}); send(b, {otherB, 72, 110});
    CHECK(render(a, 512) == render(b, 512));
    send(a, {uint8_t(otherA + 0x20), 64, 127}); send(b, {uint8_t(otherB + 0x20), 64, 127});
    send(a, {uint8_t(otherA - 0x10), 72, 0}); send(b, {uint8_t(otherB - 0x10), 72, 0});
    CHECK(a.heldVoices() == 0); CHECK(b.heldVoices() == 0);
    CHECK(a.sustainedVoices() == 1); CHECK(b.sustainedVoices() == 1);
    CHECK(render(a, 512) == render(b, 512));
    send(a, {0xb2, 64, 0}); send(b, {0xb2, 64, 0});
    CHECK(a.sustainedVoices() == 1); CHECK(b.sustainedVoices() == 1);
    send(a, {uint8_t(otherA + 0x20), 64, 0}); send(b, {uint8_t(otherB + 0x20), 64, 0});
    CHECK(a.heldVoices() == 1); CHECK(b.heldVoices() == 1);
    CHECK(render(a, 2048) == render(b, 2048));
    send(a, {0x80, 48, 0}); send(b, {0x81, 48, 0});
    CHECK(a.heldVoices() == 0); CHECK(b.heldVoices() == 0);
  }
}

TEST_CASE("Wurlitzer mode changes retain channel gain and expression independently of allocation", "[emulation][wurli][allocation]")
{
  Settings s;
  Engine a{48000., s}, b{48000., s};
  send(a, {0xb3, 7, 40}); send(a, {0xb3, 11, 70});
  for(auto mode : {Allocation::Mono, Allocation::MidiChannel, Allocation::Poly}) {
    s.voiceAllocation = mode;
    REQUIRE(a.configure(s)); REQUIRE(b.configure(s));
    send(b, {0xb3, 7, 40}); send(b, {0xb3, 11, 70});
    send(a, {0x93, 55, 90}); send(b, {0x93, 55, 90});
    CHECK(render(a, 1024) == render(b, 1024));
  }
}

TEST_CASE("Wurlitzer monophonic pedal priority retains sustained fallback candidates", "[emulation][wurli][allocation][sustain]")
{
  Settings s; s.voiceAllocation = Allocation::Mono;
  Engine e{48000., s};
  send(e, {0xb0, 64, 127});
  send(e, {0x90, 60, 42}); send(e, {0x80, 60, 0}); // Sustained C on channel 1.
  send(e, {0x91, 64, 110}); // Displaced C stays eligible.
  send(e, {0x81, 64, 0});
  CHECK(e.heldVoices() == 0); CHECK(e.sustainedVoices() == 1);
  send(e, {0xb1, 64, 0}); CHECK(e.sustainedVoices() == 1);
  send(e, {0xb0, 64, 0}); CHECK(e.sustainedVoices() == 0);
  CHECK(audible(render(e, 1024)));

  e.reset();
  send(e, {0x90, 60, 42}); send(e, {0x91, 64, 110});
  send(e, {0xb0, 64, 127}); send(e, {0x80, 60, 0}); // Sustain a displaced key.
  CHECK(e.heldVoices() == 1); CHECK(e.sustainedVoices() == 0);
  send(e, {0x81, 64, 0}); CHECK(e.sustainedVoices() == 1);
  send(e, {0xb0, 64, 0}); CHECK(e.sustainedVoices() == 0);
}

TEST_CASE("Wurlitzer All Notes Off honors latest sustained priority without losing FIFO debt", "[emulation][wurli][allocation][sustain]")
{
  for(auto mode : {Allocation::Mono, Allocation::MidiChannel}) {
    Settings s; s.voiceAllocation = mode;
    Engine e{48000., s};
    const uint8_t top = mode == Allocation::Mono ? 1 : 0;
    send(e, {0x90, 60, 42}); send(e, {uint8_t(0x90 | top), 64, 110});
    send(e, {uint8_t(0xb0 | top), 64, 127});
    send(e, {uint8_t(0xb0 | top), 123, 0});
    CHECK(e.heldVoices() == 0); CHECK(e.sustainedVoices() == 1);
    send(e, {0xb2, 64, 0}); CHECK(e.sustainedVoices() == 1);
    send(e, {uint8_t(0x80 | top), 64, 0}); CHECK(e.sustainedVoices() == 1);
    send(e, {uint8_t(0xb0 | top), 64, 0});
    CHECK(e.sustainedVoices() == 0);
    CHECK(e.heldVoices() == (mode == Allocation::Mono ? 1 : 0));
    send(e, {0x80, 60, 0}); CHECK(e.heldVoices() == 0);
    render(e, 1024);
  }
}
