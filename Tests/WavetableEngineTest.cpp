// SPDX-License-Identifier: GPL-3.0-or-later
#include <Emulation/Wavetable/Engine.hpp>
#include <Emulation/Volume.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>
namespace {
using namespace Emulation::Wavetable;
void send(Engine& e, std::initializer_list<uint8_t> bytes) { e.midi({bytes.begin(), bytes.size()}); REQUIRE(e.error() == nullptr); }
std::vector<double> render(Engine& e, std::size_t frames, std::size_t block = 113) {
  std::vector<double> l(frames), r(frames);
  for(std::size_t at = 0; at < frames; at += block) e.render(l.data() + at, r.data() + at, std::min(block, frames - at));
  REQUIRE(std::all_of(l.begin(), l.end(), [](double x) { return std::isfinite(x); })); return l;
}
Settings settings(Model m) { Settings s; s.model = m; return s; }
std::vector<double> risingCrossings(const std::vector<double>& audio, std::size_t start) {
  std::vector<double> crossings;
  for(std::size_t i = std::max<std::size_t>(1, start); i < audio.size(); ++i)
    if(audio[i - 1] <= 0. && audio[i] > 0.)
      crossings.push_back(double(i - 1) - audio[i - 1] / (audio[i] - audio[i - 1]));
  return crossings;
}
}
TEST_CASE("Wavetable coalesces retriggers within its physical polyphony limit", "[emulation][wavetable][allocation]") {
  for(auto model : {Model::Scc, Model::SccPlus, Model::HuC6280}) {
    Engine e{48000, settings(model)}; const int n = model == Model::HuC6280 ? 6 : 5;
    send(e, {0x90, 60, 127}); send(e, {0x90, 60, 100});
    REQUIRE(e.activeVoices() == 1);
    send(e, {0x80, 60, 0}); CHECK(e.activeVoices() == 1);
    for(int i = 1; i < n; ++i) send(e, {0x90, uint8_t(60 + i), 127});
    REQUIRE(e.activeVoices() == n); send(e, {0x90, 72, 127});
    send(e, {0x80, 60, 0}); CHECK(e.activeVoices() == n);
    for(int i = 1; i < n; ++i) send(e, {0x80, uint8_t(60 + i), 0});
    CHECK(e.activeVoices() == 1); CHECK(e.voiceNote(0) == 72);
    send(e, {0xb0, 64, 127}); send(e, {0x80, 72, 0}); CHECK(e.activeVoices() == 1);
    send(e, {0xb0, 121, 0}); CHECK(e.activeVoices() == 0);
    send(e, {0x90, 70, 127}); send(e, {0xb0, 123, 0}); CHECK(e.activeVoices() == 0);
    send(e, {0x90, 70, 127}); send(e, {0xb0, 120, 0}); CHECK(e.activeVoices() == 0);
  }
}
TEST_CASE("SCC channel four owns shared wave RAM while SCC+ retains independent fifth wave", "[emulation][wavetable]") {
  Settings s; s.channels[3].wave.fill((47. + 128.) / 255.); s.channels[4].wave.fill((-37. + 128.) / 255.);
  Engine scc{48000, s}; CHECK(scc.nativeWave(4, 17) == 47);
  s.channels[3].wave[17] = (-82. + 128.) / 255.; REQUIRE(scc.configure(s)); CHECK(scc.nativeWave(4, 17) == -82);
  s.model = Model::SccPlus; Engine plus{48000, s}; CHECK(plus.nativeWave(4, 17) == -37);
  s.model = Model::HuC6280; s.channels[5].wave[17] = 29. / 31.; Engine huc{48000, s}; CHECK(huc.nativeWave(5, 17) == 29);
  s.channels[5].wave[17] = 1.1; CHECK_FALSE(huc.configure(s)); CHECK(huc.nativeWave(5, 17) == 29);
}
TEST_CASE("Wavetable phase and pitch are independent of host rate and block partition", "[emulation][wavetable]") {
  for(auto model : {Model::Scc, Model::SccPlus, Model::HuC6280}) for(double rate : {8000., 44100., 48000., 96000., 384000.}) {
    auto s = settings(model); Engine whole{rate, s}, split{rate, s}, isolated{rate, s};
    send(whole, {0x90, 69, 127}); send(split, {0x90, 69, 127});
    auto a = render(whole, std::size_t(rate), std::size_t(rate)), b = render(split, std::size_t(rate)); CHECK(a == b);
    int crossings{}; for(std::size_t i = 1; i < a.size(); ++i) if(a[i - 1] <= 0. && a[i] > 0.) ++crossings;
    CHECK(crossings >= 435); CHECK(crossings <= 445);
    const auto silent = render(isolated, 500); CHECK(std::all_of(silent.begin(), silent.end(), [](double x) { return x == 0.; }));
  }
}
TEST_CASE("Wavetable volume edits retain clocks without requiring another note", "[emulation][wavetable]") {
  for(auto model : {Model::Scc, Model::SccPlus, Model::HuC6280}) {
    auto s = settings(model); Engine a{48000, s}, b{48000, s}; send(a, {0x90, 69, 127}); send(b, {0x90, 69, 127});
    for(double volume : {.25, 0., 1., 4.5, double(Emulation::MaxVolume)}) {
      s.volume = volume; REQUIRE(b.configure(s)); const auto ref = render(a, 777), actual = render(b, 777);
      for(std::size_t i = 0; i < ref.size(); ++i) CHECK(actual[i] == ref[i] * volume);
    }
  }
}
TEST_CASE("HuC stereo attenuation and LFO consume native channel resources", "[emulation][wavetable]") {
  auto s = settings(Model::HuC6280); s.assignment = Assignment::Fixed; s.lfoDepth = 2; s.channels[0].right = 0;
  Engine e{48000, s}; send(e, {0x91, 69, 127}); CHECK(e.activeVoices() == 0);
  send(e, {0x90, 69, 127}); std::array<double, 2048> l{}, r{}; e.render(l.data(), r.data(), l.size());
  CHECK(std::all_of(r.begin(), r.end(), [](double x) { return x == 0.; }));
  CHECK(std::any_of(l.begin(), l.end(), [](double x) { return x != 0.; }));
  s.assignment = Assignment::Polyphonic; REQUIRE(e.configure(s));
  for(uint8_t note = 60; note < 72; ++note) send(e, {0x90, note, 127});
  CHECK(e.activeVoices() == 5); CHECK(e.voiceNote(1) == -1);
  s.lfoDepth = 0; REQUIRE(e.configure(s)); send(e, {0x90, 75, 127}); CHECK(e.activeVoices() == 6);
}
TEST_CASE("HuC noise remains instance local and partition invariant", "[emulation][wavetable]") {
  auto s = settings(Model::HuC6280); s.assignment = Assignment::Fixed; s.channels[4].noise = true; s.channels[4].noisePeriod = 23;
  Engine a{44100, s}, b{44100, s}; send(a, {0x94, 60, 127}); send(b, {0x94, 60, 127});
  CHECK(render(a, 10000, 10000) == render(b, 10000, 71));
  a.panic(); CHECK(a.activeVoices() == 0);
}

TEST_CASE("Wavetable scalar deltas preserve wave RAM and phase through dense volume edits", "[emulation][wavetable]") {
  for(auto model : {Model::Scc, Model::SccPlus, Model::HuC6280}) {
    auto s = settings(model); Engine reference{48000, s}, edited{48000, s};
    send(reference, {0x90, 69, 127}); send(edited, {0x90, 69, 127});
    for(int frame = 0; frame < 1024; ++frame) {
      const double volume = (frame % 65) * .5;
      REQUIRE(edited.setScalarControl(Volume, volume));
      REQUIRE(edited.setScalarControl(Volume, volume));
      double a{}, ar{}, b{}, br{};
      reference.render(&a, &ar, 1); edited.render(&b, &br, 1);
      CHECK(b == a * volume); CHECK(br == ar * volume);
    }
    for(double invalid : {-1., double(Emulation::MaxVolume) + .01,
        std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
      const auto before = s;
      CHECK_FALSE(setScalarControl(s, Volume, invalid)); CHECK(s == before);
      CHECK_FALSE(edited.setScalarControl(Volume, invalid));
      auto rejected = s; rejected.volume = invalid;
      CHECK(validate(rejected)); CHECK_FALSE(edited.configure(rejected));
    }
    CHECK_FALSE(edited.setScalarControl(channelControl(0, Level), 1.01));
    REQUIRE(edited.setScalarControl(Volume, 1.));
    CHECK(render(reference, 257) == render(edited, 257));
    for(int i = 0; i < 32; ++i) CHECK(edited.nativeWave(0, i) == reference.nativeWave(0, i));
    REQUIRE(edited.setScalarControl(channelControl(0, ChannelTuning), 100.));
    s.channels[0].tuning = 100; REQUIRE(reference.configure(s));
    CHECK(render(reference, 257) == render(edited, 257));
    const std::array<uint8_t, 2> malformed{0x90, 60};
    edited.midi(malformed); REQUIRE(edited.error() != nullptr);
    send(edited, {0x90, 72, 127}); CHECK(edited.error() == nullptr);
  }
}

TEST_CASE("Wavetable engines quantize one normalized channel wave and musical level", "[emulation][wavetable]") {
  Settings authored;
  authored.channels[0].level = .43;
  for(int i = 0; i < 32; ++i) authored.channels[0].wave[i] = (i + .37) / 32.;
  for(auto model : {Model::Scc, Model::HuC6280, Model::SccPlus, Model::Scc}) {
    auto s = authored; s.model = model;
    Engine engine{48000, s};
    for(int i = 0; i < 32; ++i)
      CHECK(engine.nativeWave(0, i) == std::lround((model == Model::HuC6280 ? 0. : -128.)
          + (model == Model::HuC6280 ? 31. : 255.) * authored.channels[0].wave[i]));
    send(engine, {0x90, 69, 127});
    const auto sound = render(engine, 512);
    CHECK(std::any_of(sound.begin(), sound.end(), [](double x) { return x != 0.; }));
    REQUIRE(engine.setScalarControl(channelControl(0, Level), 0.));
    const auto silence = render(engine, 1024);
    CHECK(std::all_of(silence.begin() + 512, silence.end(), [](double x) { return x == 0.; }));
  }
}

TEST_CASE("Prepared waveform snapshots update native RAM without replacing held voices", "[emulation][wavetable][samples]") {
  for(auto model : {Model::Scc, Model::SccPlus, Model::HuC6280}) {
    auto authored = settings(model);
    authored.assignment = Assignment::Fixed;
    Engine engine{48000., authored};
    const int channels = model == Model::HuC6280 ? 6 : 5;
    for(int ch = 0; ch < channels; ++ch)
      send(engine, {uint8_t(0x90 + ch), uint8_t(60 + ch), 127});
    render(engine, 127);
    for(int ch = 0; ch < 6; ++ch)
      for(int i = 0; i < 32; ++i)
        authored.channels[ch].wave[i] = ((i * 7 + ch * 11) % 32) / 31.;
    REQUIRE(engine.configure(authored));
    CHECK(engine.activeVoices() == channels);
    for(int ch = 0; ch < channels; ++ch) {
      CHECK(engine.voiceNote(ch) == 60 + ch);
      const int owner = model == Model::Scc && ch == 4 ? 3 : ch;
      for(int i = 0; i < 32; ++i)
        CHECK(engine.nativeWave(ch, i) == std::lround(
            (model == Model::HuC6280 ? 0. : -128.)
            + (model == Model::HuC6280 ? 31. : 255.) * authored.channels[owner].wave[i]));
    }
    const auto output = render(engine, 512);
    CHECK(std::any_of(output.begin(), output.end(), [](double x) { return x != 0.; }));
    for(int ch = 0; ch < channels; ++ch)
      send(engine, {uint8_t(0x80 + ch), uint8_t(60 + ch), 0});
    CHECK(engine.activeVoices() == 0);
  }
}

TEST_CASE("HuC live cents tuning follows native dividers without extra pitch plateaus", "[emulation][wavetable][huc-pitch]") {
  for(int rate : {44100, 48000, 96000}) for(int note : {24, 41, 69}) {
    auto s = settings(Model::HuC6280);
    s.assignment = Assignment::Fixed;
    Engine engine{double(rate), s};
    send(engine, {0x92, uint8_t(note), 100});
    for(int cents : {0, 1, 10, 28, 29, 40}) {
      CAPTURE(rate, note, cents);
      s.channels[2].tuning = cents;
      REQUIRE(engine.configure(s));
      const auto audio = render(engine, rate);
      const auto crossings = risingCrossings(audio, rate / 4);
      REQUIRE(crossings.size() >= 2);
      const double actual = rate * double(crossings.size() - 1)
          / (crossings.back() - crossings.front());
      const double requested = 440. * std::exp2((note - 69) / 12. + cents / 1200.);
      const auto divider = std::lround(s.clock / (32. * requested));
      const double expected = s.clock / (32. * divider);
      // Allow sampling/zero-crossing measurement error, not another tuning grid.
      CHECK(std::abs(1200. * std::log2(actual / expected)) < .15);
      CHECK(engine.voiceNote(2) == note);
    }
  }
}

TEST_CASE("HuC LFO retains waveform pitch and advances its source across render blocks", "[emulation][wavetable][huc-pitch]") {
  auto normal = settings(Model::HuC6280);
  normal.assignment = Assignment::Fixed;
  auto modulated = normal;
  modulated.lfoDepth = 3;
  modulated.lfoPeriod = 4095;
  modulated.lfoFrequency = 255;
  modulated.channels[1].wave.fill(16. / 31.);
  Engine plain{48000., normal}, neutral{48000., modulated};
  send(plain, {0x90, 69, 100});
  send(neutral, {0x90, 69, 100});
  CHECK(render(plain, 48000) == render(neutral, 48000));

  for(int i = 0; i < 32; ++i)
    modulated.channels[1].wave[i] = (i < 16 ? 15. : 17.) / 31.;
  Engine whole{48000., modulated}, split{48000., modulated};
  send(whole, {0x90, 69, 100});
  send(split, {0x90, 69, 100});
  const auto audio = render(whole, 48000, 48000);
  CHECK(audio == render(split, 48000, 71));
  const auto crossings = risingCrossings(audio, 12000);
  REQUIRE(crossings.size() >= 3);
  double shortest = std::numeric_limits<double>::max(), longest{};
  for(std::size_t i = 1; i < crossings.size(); ++i) {
    const double period = crossings[i] - crossings[i - 1];
    shortest = std::min(shortest, period);
    longest = std::max(longest, period);
  }
  CHECK(longest - shortest > 8.);
}

TEST_CASE("Wavetable mono lines retain original channel expression and held-note priority", "[emulation][wavetable][allocation]") {
  for(auto model : {Model::Scc, Model::SccPlus, Model::HuC6280})
    for(auto mode : {Assignment::Mono, Assignment::Fixed}) {
      auto s = settings(model); s.assignment = mode;
      Engine e{48000, s};
      send(e, {0x90, 60, 55});
      const uint8_t other = mode == Assignment::Mono ? 2 : 0;
      send(e, {uint8_t(0x90 | other), 64, 127});
      send(e, {uint8_t(0x80 | other), 64, 0});
      CHECK(e.voiceNote(0) == 60);
      CHECK(e.activeVoices() == (mode == Assignment::Mono ? (model == Model::HuC6280 ? 6 : 5) : 1));
      const auto audio = render(e, 2048);
      const auto [lo, hi] = std::minmax_element(audio.begin(), audio.end());
      CHECK(*hi - *lo > 1e-4);
      send(e, {0x81, 60, 0}); CHECK(e.voiceNote(0) == 60);
      send(e, {0xb0, 7, 0});
      const auto muted = render(e, 2048);
      CHECK(std::all_of(muted.begin() + 1024, muted.end(), [](double x) { return x == 0.; }));
      send(e, {0xb0, 64, 127}); send(e, {0x80, 60, 0});
      send(e, {0xb1, 64, 0}); CHECK(e.voiceNote(0) == 60);
      send(e, {0xb0, 64, 0}); CHECK(e.activeVoices() == 0);
    }
}

TEST_CASE("Wavetable mode changes drain stale note-offs and respect reserved mono resources", "[emulation][wavetable][allocation]") {
  for(auto model : {Model::Scc, Model::SccPlus, Model::HuC6280}) {
    auto s = settings(model); Engine e{48000, s};
    send(e, {0x90, 60, 127});
    REQUIRE(e.setScalarControl(VoiceAssignment, int(Assignment::Fixed)));
    CHECK(e.activeVoices() == 0);
    send(e, {0x90, 60, 127}); send(e, {0x80, 60, 0});
    CHECK(e.activeVoices() == 1);
    send(e, {0x80, 60, 0}); CHECK(e.activeVoices() == 0);
    s.assignment = Assignment::Mono; s.channels[4].enabled = false;
    if(model == Model::HuC6280) s.lfoDepth = 1;
    REQUIRE(e.configure(s));
    send(e, {0x90, 64, 127});
    CHECK(e.activeVoices() == 4);
    CHECK(e.voiceNote(4) == -1);
    if(model == Model::HuC6280) CHECK(e.voiceNote(1) == -1);
    send(e, {0xb0, 120, 0}); CHECK(e.activeVoices() == 0);
    send(e, {0x90, 64, 127}); send(e, {0x80, 64, 0});
    CHECK(e.activeVoices() == 4);
    send(e, {0x80, 64, 0}); CHECK(e.activeVoices() == 0);
  }
}

TEST_CASE("SCC channels fall to exact silence within a few buffers of a note-off", "[emulation][wavetable]") {
  for(auto model : {Model::Scc, Model::SccPlus})
    for(auto [rate, block] : {std::pair{44100., std::size_t(512)}, std::pair{48000., std::size_t(8192)}}) {
      CAPTURE(int(model), rate, block);
      Engine e{rate, settings(model)};
      send(e, {0x90, 69, 127});
      const auto sound = render(e, 4 * block, block);
      REQUIRE(std::any_of(sound.begin(), sound.end(), [](double x) { return x != 0.; }));
      send(e, {0x80, 69, 0});
      render(e, 2 * block, block);
      const auto tail = render(e, 2 * block, block);
      CHECK(std::all_of(tail.begin(), tail.end(), [](double x) { return x == 0.; }));
    }
}
