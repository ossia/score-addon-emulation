// SPDX-License-Identifier: GPL-3.0-or-later
#include <Emulation/Wavetable/ProcessModel.hpp>
#include <Dataflow/WaveformInlet.hpp>
#include <Process/Preset.hpp>
#include <Process/ProcessList.hpp>
#include <score_test/App.hpp>
#include <score_test/Project.hpp>
#include <catch2/catch_test_macros.hpp>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <array>
#include <cmath>
#include <vector>

namespace
{
using namespace Emulation::Wavetable;
void note(Engine& engine, int key, bool on)
{
  const std::array<uint8_t, 3> message{uint8_t(on ? 0x90 : 0x80), uint8_t(key), uint8_t(on ? 100 : 0)};
  engine.midi(message);
  REQUIRE_FALSE(engine.error());
}
// First substantial minimum of YIN's cumulative mean normalized difference:
// unlike zero crossings, this identifies pitch in a multi-peaked wavetable.
double pitch(const std::vector<double>& samples)
{
  constexpr int window = 4096, maximumLag = 1024;
  std::array<double, maximumLag + 1> difference{};
  double accumulated{};
  difference[0] = 1.;
  for(int lag = 1; lag <= maximumLag; ++lag)
  {
    double sum{};
    for(int i = 0; i < window; ++i)
    {
      const double delta = samples[i] - samples[i + lag];
      sum += delta * delta;
    }
    accumulated += sum;
    difference[lag] = accumulated > 0. ? sum * lag / accumulated : 1.;
  }
  for(int lag = 20; lag < maximumLag - 1; ++lag)
    if(difference[lag] < .05)
    {
      while(lag < maximumLag - 1 && difference[lag + 1] < difference[lag]) ++lag;
      const double a = difference[lag - 1], b = difference[lag], c = difference[lag + 1];
      const double denominator = a - 2. * b + c;
      const double correction = denominator != 0. ? .5 * (a - c) / denominator : 0.;
      return 48000. / (lag + correction);
    }
  FAIL("Rendered wavetable has no measurable musical fundamental");
  return 0.;
}
}

TEST_CASE("Polyphonic factory wavetable pitch does not depend on occupied voices", "[emulation][wavetable][preset][pitch]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* document = score::test::new_document(ctx); REQUIRE(document);
    auto* base = score::test::add_process(*document, QStringLiteral("989bc228-c2e7-40a8-a56e-4700b946d864"), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    const auto directory = QFileInfo{QString::fromUtf8(__FILE__)}.absoluteDir();
    for(const char* name : {"SCC Plus Oboe thirds", "SCC Octave staircase"})
    {
      CAPTURE(name);
      QFile file{directory.filePath(QStringLiteral("../Presets/Wavetable/%1.scp").arg(QString::fromLatin1(name)))};
      REQUIRE(file.open(QIODevice::ReadOnly));
      auto preset = Process::Preset::fromJson(ctx.interfaces<Process::ProcessFactoryList>(), file.readAll()); REQUIRE(preset);
      model.loadPreset(*preset); REQUIRE(model.error().isEmpty());
      const auto settings = model.settings();
      REQUIRE(settings.assignment == Assignment::Polyphonic);
      double solo{};
      for(int occupied = 0; occupied < 5; ++occupied)
      {
        CAPTURE(occupied);
        Engine engine{48000., settings};
        // C followed by D versus C+D, extended to every physical voice.
        if(occupied == 0) { note(engine, 60, true); note(engine, 60, false); }
        for(int i = 0; i < occupied; ++i) note(engine, i == 0 ? 60 : 47 + i, true);
        note(engine, 62, true);
        REQUIRE(engine.voiceNote(occupied) == 62);
        for(int i = 0; i < occupied; ++i) note(engine, i == 0 ? 60 : 47 + i, false);
        std::vector<double> left(8192), right(8192);
        engine.render(left.data(), right.data(), 512);
        engine.render(left.data(), right.data(), left.size());
        const auto hz = pitch(left);
        INFO("D fundamental: " << hz << " Hz");
        if(occupied == 0) solo = hz;
        const double cents = 1200. * std::log2(hz / solo);
        CHECK(std::abs(cents) < 10.);
        const double expected = 440. * std::exp2((62. - 69. + settings.tuning / 100.) / 12.);
        CHECK(std::abs(1200. * std::log2(hz / expected)) < 15.);
      }
    }
  });
}

TEST_CASE("Every shipped wavetable preset contains canonical single-frame samples", "[emulation][wavetable][preset][samples]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* document = score::test::new_document(ctx); REQUIRE(document);
    auto* base = score::test::add_process(*document, QStringLiteral("989bc228-c2e7-40a8-a56e-4700b946d864"), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    QDir directory{QFileInfo{QString::fromUtf8(__FILE__)}.absoluteDir().filePath("../Presets/Wavetable")};
    const auto files = directory.entryList({"*.scp"}, QDir::Files);
    REQUIRE(files.size() == 13);
    for(const auto& name : files) {
      CAPTURE(name.toStdString());
      QFile file{directory.filePath(name)}; REQUIRE(file.open(QIODevice::ReadOnly));
      auto preset = Process::Preset::fromJson(ctx.interfaces<Process::ProcessFactoryList>(), file.readAll()); REQUIRE(preset);
      model.loadPreset(*preset); REQUIRE(model.error().isEmpty());
      const auto settings = model.settings();
      Engine engine{48000., settings};
      for(int ch = 0; ch < 6; ++ch) {
        auto* inlet = dynamic_cast<Dataflow::WaveformInlet*>(model.inlets()[channelControl(ch, Wave) + 1]);
        REQUIRE(inlet); REQUIRE(inlet->wavetable().valid());
        CHECK(inlet->wavetable().frameSize == 32);
        CHECK(inlet->wavetable().frameCount() == 1);
        CHECK(inlet->value() == Process::wavetableToValue(inlet->wavetable()));
        CHECK(inlet->amplitudeLevels() == (settings.model == Model::HuC6280 ? 32 : 256));
        if(ch >= (settings.model == Model::HuC6280 ? 6 : 5)) continue;
        const int owner = settings.model == Model::Scc && ch == 4 ? 3 : ch;
        for(int i = 0; i < 32; ++i)
          CHECK(engine.nativeWave(ch, i) == std::lround(
              (settings.model == Model::HuC6280 ? 0. : -128.)
              + (settings.model == Model::HuC6280 ? 31. : 255.) * settings.channels[owner].wave[i]));
      }
      ProcessModel restored{TimeVal::fromMsecs(1000), {}, Id<Process::ProcessModel>{999}, nullptr};
      restored.loadPreset(model.savePreset()); REQUIRE(restored.error().isEmpty());
      CHECK(restored.settings() == settings);
      for(int ch = 0; ch < 6; ++ch)
        CHECK(static_cast<Process::ControlInlet*>(restored.inlets()[channelControl(ch, Wave) + 1])->value()
            == static_cast<Process::ControlInlet*>(model.inlets()[channelControl(ch, Wave) + 1])->value());
    }
  });
}
