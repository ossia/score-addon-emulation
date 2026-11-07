// SPDX-License-Identifier: GPL-3.0-or-later
#include <Process/ExecutionTransaction.hpp>
#include <Process/Commands/SetControlValue.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <Emulation/Psg/Controls.hpp>
#include <Emulation/Psg/Executor/Node.hpp>
#include <Emulation/Psg/ProcessModel.hpp>
#include <Emulation/Volume.hpp>
#include <Execution/DocumentPlugin.hpp>
#include <Scenario/Commands/LoadPresetCommand.hpp>
#include <score/command/Dispatchers/CommandDispatcher.hpp>
#include <core/command/CommandStack.hpp>
#include <score/model/EntitySerialization.hpp>
#include <score/serialization/VisitorCommon.hpp>
#include <ossia/dataflow/execution_state.hpp>
#include <ossia/dataflow/data_copy.hpp>
#include <libremidi/ump_events.hpp>
#include <catch2/catch_test_macros.hpp>
#include <score_test/App.hpp>
#include <score_test/Project.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>
namespace
{
using namespace Emulation::Psg;
Process::ControlInlet& control(ProcessModel& p, int index)
{ return *static_cast<Process::ControlInlet*>(p.inlets()[index + 1]); }
constexpr auto uuid = "5aabbeda-10c7-40ca-985a-90d7863df990";
void drain(Execution::DocumentPlugin& plug)
{
  const auto& ctx = plug.context(); Execution::ExecutionCommand command;
  ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
  while(ctx.executionQueue.try_dequeue(command)) { command(); ctx.gcQueue.enqueue(Execution::gc(std::move(command))); }
  ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
  Execution::GCCommand retired; while(ctx.gcQueue.try_dequeue(retired)) retired();
}
ossia::token_request tokenFor(const ossia::execution_state& state)
{
  ossia::token_request token;
  token.prev_date = ossia::time_value{0};
  token.date = ossia::time_value{int64_t(state.bufferSize * ossia::flicks_per_second<double> / state.sampleRate)};
  token.start_sample = 0; token.length_sample = state.bufferSize; return token;
}
}
TEST_CASE("PSG gain domains migrate without rescaling saved values and presets", "[emulation][psg][document][preset]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    for(float gain : {0.f, .375f, 1.f, 4.5f, Emulation::MaxVolume}) {
      CAPTURE(gain);
      control(model, Volume).setValue(gain); control(model, Volume).setInit(.625f);
      control(model, Volume).setDomain(ossia::make_domain(0.f, gain <= 1.f ? 1.f : Emulation::MaxVolume));
      REQUIRE(model.settings().volume == gain);
      const auto json = toValue(score::marshall<JSONObject>(*base));
      JSONObject::Deserializer reader{json}; ProcessModel restored{reader, nullptr};
      QByteArray bytes; DataStream::Serializer writer{&bytes};
      TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
      writer.read(static_cast<const Process::ProcessModel&>(model)); writer.read(model);
      DataStream::Deserializer binaryReader{bytes}; ProcessModel binary{binaryReader, nullptr};
      for(auto* copy : {&restored, &binary}) {
        CHECK(copy->settings().volume == gain);
        CHECK(control(*copy, Volume).value() == ossia::value{gain});
        CHECK(control(*copy, Volume).init() == ossia::value{.625f});
        CHECK(control(*copy, Volume).id() == Id<Process::Port>{Volume + 1});
        CHECK(control(*copy, Volume).domain().get().convert_min<float>() == 0.f);
        CHECK(control(*copy, Volume).domain().get().convert_max<float>() == Emulation::MaxVolume);
        CHECK(control(*copy, channelControl(0, Level)).domain().get().convert_max<int>() == 15);
        // Native automation outputs raw values on a domainless source port.
        ossia::value_port source; source.write_value(gain, 7);
        ossia::value_inlet sink; control(*copy, Volume).setupExecution(sink, nullptr);
        sink.data.add_port_values(source);
        REQUIRE(sink.data.get_data().size() == 1);
        CHECK(sink.data.get_data().front().value == ossia::value{gain});
        CHECK(sink.data.get_data().front().timestamp == 7);
      }
      const auto preset = model.savePreset();
      control(restored, Volume).setValue(gain == 0.f ? 1.f : 0.f);
      restored.loadPreset(preset);
      CHECK(restored.error().isEmpty()); CHECK(restored.settings().volume == gain);
      for(float invalid : {-1.f, Emulation::MaxVolume + 1.f,
          std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        control(restored, Volume).setValue(invalid);
        CHECK(restored.settings().volume == gain);
        CHECK(control(restored, Volume).value() == ossia::value{gain});
        CHECK_FALSE(restored.error().isEmpty());
      }
      for(float invalid : {-1.f, Emulation::MaxVolume + 1.f}) {
        auto rejected = preset;
        rejected.data = "[[" + QByteArray::number(Volume + 1) + ", {\"Float\":"
            + QByteArray::number(invalid) + "}]]";
        restored.loadPreset(rejected);
        CHECK(restored.settings().volume == gain); CHECK_FALSE(restored.error().isEmpty());
      }
    }
  });
}

TEST_CASE("PSG semantic state recalls model, dormant AY controls and post-render volume", "[emulation][psg][document]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model, ChipModel).setValue(2); control(model, Clock).setValue(3579545);
    control(model, AyEnvelopePeriod).setValue(31415);
    control(model, channelControl(1, HardwareEnvelope)).setValue(true);
    control(model, SegaNoiseEnabled).setValue(true); control(model, SegaNoiseRate).setValue(3);
    control(model, channelControl(2, Level)).setValue(7); control(model, Volume).setValue(.25f);
    auto json = toValue(score::marshall<JSONObject>(*base));
    JSONObject::Deserializer reader{json}; ProcessModel restored{reader, nullptr};
    for(int c = 0; c < ControlCount; ++c) CHECK(controlValue(restored.settings(), c) == controlValue(model.settings(), c));
    QByteArray binary; DataStream::Serializer writer{&binary};
    TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
    writer.read(static_cast<const Process::ProcessModel&>(model));
    writer.read(model);
    DataStream::Deserializer binaryReader{binary};
    ProcessModel binaryRestored{binaryReader, nullptr};
    for(int c = 0; c < ControlCount; ++c) CHECK(controlValue(binaryRestored.settings(), c) == controlValue(model.settings(), c));
    json["Model"].SetString("AY-3-8910");
    JSONObject::Deserializer mismatch{json};
    CHECK_THROWS_AS((ProcessModel{mismatch, nullptr}), std::runtime_error);
  });
}
TEST_CASE("PSG native preset commands recall chip architectures and undo all controls", "[emulation][psg][document][preset]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model, AyEnvelopePeriod).setValue(1600);
    control(model, channelControl(0, HardwareEnvelope)).setValue(true);
    control(model, Volume).setValue(.25f);
    const auto original = model.settings();
    const auto checkState = [&](const Settings& expected) {
      CHECK(model.error().isEmpty());
      const auto actual = model.settings();
      for(int c = 0; c < ControlCount; ++c) {
        CAPTURE(c);
        CHECK(controlValue(actual, c) == controlValue(expected, c));
      }
    };
    CommandDispatcher<> dispatcher{doc->context().commandStack};
    auto& stack = doc->commandStack();
    for(auto chip : {Model::Ym2149, Model::SegaYm7101}) {
      ProcessModel source{TimeVal::fromMsecs(1000), {}, Id<Process::ProcessModel>{999}, nullptr};
      control(source, ChipModel).setValue(int(chip));
      control(source, Clock).setValue(chip == Model::SegaYm7101 ? 3579545 : 2000000);
      control(source, VoiceAssignment).setValue(int(Assignment::Fixed));
      control(source, Volume).setValue(Emulation::MaxVolume);
      control(source, AyNoisePeriod).setValue(8);
      control(source, AyEnvelopePeriod).setValue(31415);
      control(source, SegaNoiseEnabled).setValue(true);
      control(source, SegaNoiseRate).setValue(3);
      control(source, SegaWhiteNoise).setValue(false);
      control(source, channelControl(1, Level)).setValue(7);
      control(source, SoftwareEnvelope).setValue(true);
      control(source, Attack).setValue(0);
      control(source, Release).setValue(60);
      const auto recalled = source.settings();
      dispatcher.submit<Scenario::Command::LoadPreset>(model, source.savePreset());
      checkState(recalled);
      REQUIRE(stack.canUndo()); stack.undo(); checkState(original);
      REQUIRE(stack.canRedo()); stack.redo(); checkState(recalled);
      stack.undo(); checkState(original);
    }
  });
}
TEST_CASE("PSG timestamped volume preserves split-token clocks and duplicate-window output", "[emulation][psg][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    REQUIRE(state.bufferSize >= 64);
    for(auto model : {Model::Ay8910, Model::Ym2149, Model::SegaYm7101}) {
      Settings settings; settings.model = model; if(model == Model::SegaYm7101) settings.clock = 3579545;
      Executor::Node reference{state, settings}, adjusted{state, settings};
      for(auto* node : {&reference, &adjusted}) {
        auto e = std::make_shared<Engine>(state.sampleRate, settings);
        node->install(e, settings.model, settings.clock);
        node->midi->data.messages.push_back(libremidi::from_midi1::note_on(0, 69, 127));
      }
      const int frames = state.bufferSize, q = frames / 4;
      adjusted.controls[Volume]->data.write_value(Emulation::MaxVolume, q);
      adjusted.controls[Volume]->data.write_value(0.f, 2 * q);
      adjusted.controls[Volume]->data.write_value(1.f, 3 * q);
      adjusted.controls[Volume]->data.write_value(Emulation::MaxVolume + 1.f, q + 1);
      adjusted.controls[Volume]->data.write_value(-1.f, q + 2);
      adjusted.controls[Volume]->data.write_value(std::numeric_limits<float>::infinity(), q + 3);
      adjusted.controls[Volume]->data.write_value(std::numeric_limits<float>::quiet_NaN(), q + 4);
      auto token = tokenFor(state), first = token, second = token;
      first.length_sample = 2 * q;
      first.date = ossia::time_value{int64_t(2 * q * ossia::flicks_per_second<double> / state.sampleRate)};
      second.prev_date = first.date; second.start_sample = 2 * q; second.length_sample = frames - 2 * q;
      state.samples_since_start += frames;
      ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
      reference.run(token, ossia::exec_state_facade{&state});
      adjusted.run(first, ossia::exec_state_facade{&state}); adjusted.run(second, ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
      const auto& a = reference.audio->data.channel(0); const auto& b = adjusted.audio->data.channel(0);
      for(int i = 0; i < frames; ++i) CHECK(b[i] == a[i] * (i < q || i >= 3 * q ? 1. : i < 2 * q ? Emulation::MaxVolume : 0.));
      const std::vector<double> saved{b.begin(), b.end()};
      ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
      adjusted.run(token, ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
      CHECK(std::equal(saved.begin(), saved.end(), b.begin()));
    }
  });
}
TEST_CASE("PSG dormant controls are retained and event overflow silences output", "[emulation][psg][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    Settings settings; settings.model = Model::SegaYm7101; settings.clock = 3579545;
    Executor::Node node{state, settings};
    auto e = std::make_shared<Engine>(state.sampleRate, settings); node.install(e, settings.model, settings.clock);
    node.controls[AyEnvelopeShape]->data.write_value(12, 0);
    auto token = tokenFor(state); state.samples_since_start += state.bufferSize;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    node.run(token, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    REQUIRE(node.error() == nullptr);
    node.controls[AyEnvelopeShape]->data.get_data().clear();
    for(int i = 0; i < 8193; ++i) node.midi->data.messages.push_back(libremidi::from_midi1::note_on(0, 69, 127));
    state.samples_since_start += state.bufferSize;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    node.run(token, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    CHECK(std::all_of(node.audio->data.channel(0).begin(), node.audio->data.channel(0).end(), [](double x) { return x == 0.; }));
    CHECK(node.error() != nullptr);
  });
}

TEST_CASE("PSG paused windows clean up sustained voices without replaying note-ons", "[emulation][psg][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    for(auto model : {Model::Ay8910, Model::SegaYm7101}) {
      Settings settings; settings.model = model;
      Executor::Node node{state, settings};
      auto engine = std::make_shared<Engine>(state.sampleRate, settings);
      const std::array<uint8_t, 3> sustain{0xb0, 64, 127}, note{0x90, 69, 127}, off{0x80, 69, 0};
      engine->midi(sustain); engine->midi(note); engine->midi(off);
      node.install(engine, settings.model, settings.clock);
      auto token = tokenFor(state);
      state.samples_since_start += state.bufferSize;
      ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
      node.run(token, ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
      auto paused = token; paused.date = paused.prev_date; paused.length_sample = 0;
      node.midi->data.messages.push_back(libremidi::from_midi1::note_on(0, 72, 127));
      state.samples_since_start += state.bufferSize;
      ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
      node.run(paused, ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
      node.midi->data.messages.clear();
      for(int i = 0; i < 4; ++i) {
        state.samples_since_start += state.bufferSize;
        ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
        node.run(token, ossia::exec_state_facade{&state});
        ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
      }
      CHECK(std::all_of(node.audio->data.channel(0).begin(), node.audio->data.channel(0).end(),
          [](double value) { return value == 0.; }));
    }
  });
}

TEST_CASE("PSG restores stereo output after graph buffer recycling", "[emulation][psg][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    for(auto model : {Model::Ay8910, Model::Ym2149, Model::SegaYm7101}) {
      Settings settings; settings.model = model;
      Executor::Node node{state, settings};
      auto engine = std::make_shared<Engine>(state.sampleRate, settings);
      node.install(engine, settings.model, settings.clock);
      Engine reference{double(state.sampleRate), settings};
      const std::array<uint8_t, 3> note{0x90, 69, 127}; reference.midi(note);
      std::vector<double> left(state.bufferSize), right(state.bufferSize);
      auto token = tokenFor(state);
      double peak{};
      for(int tick = 0; tick < 96; ++tick) {
        ossia::clear_data{}(node.audio->data);
        if(tick % 2) {
          node.audio->data.set_channels(2);
          for(auto& channel : node.audio->data.get()) {
            channel.clear(); channel.shrink_to_fit();
          }
        }
        node.midi->data.messages.clear();
        if(tick == 0) node.midi->data.messages.push_back(libremidi::from_midi1::note_on(0, 69, 127));
        state.samples_since_start += state.bufferSize;
        ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
        node.run(token, ossia::exec_state_facade{&state});
        reference.render(left.data(), right.data(), left.size());
        ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
        REQUIRE(node.audio->data.channels() == 2);
        CHECK_FALSE(node.error());
        CHECK(std::equal(left.begin(), left.end(), node.audio->data.channel(0).begin(), node.audio->data.channel(0).end()));
        CHECK(std::equal(right.begin(), right.end(), node.audio->data.channel(1).begin(), node.audio->data.channel(1).end()));
        for(double sample : left) peak = std::max(peak, std::abs(sample));
      }
      CHECK(peak > .001);
    }
  });
}

TEST_CASE("PSG preset recall publishes one valid sound and rejects partial invalid recall", "[emulation][psg][qualification][preset]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model, channelControl(0, HardwareEnvelope)).setValue(true);
    ProcessModel source{TimeVal::fromMsecs(1000), {}, Id<Process::ProcessModel>{999}, nullptr};
    control(source, SoftwareEnvelope).setValue(true);
    control(source, Attack).setValue(0);
    control(source, Release).setValue(35);
    control(source, Volume).setValue(.25f);
    const auto expected = source.settings();
    int publications{};
    QObject::connect(&model, &ProcessModel::settingsChanged, &model, [&] {
      ++publications;
      CHECK(validate(model.settings()) == nullptr);
      for(int c = 0; c < ControlCount; ++c)
        CHECK(controlValue(model.settings(), c) == controlValue(expected, c));
    });
    model.loadPreset(source.savePreset());
    CHECK(publications == 1);
    CHECK(model.error().isEmpty());
    auto invalid = source.savePreset();
    invalid.data = R"([[2,{"Int":0}]])";
    model.loadPreset(invalid);
    CHECK(publications == 1);
    CHECK_FALSE(model.error().isEmpty());
    for(int c = 0; c < ControlCount; ++c)
      CHECK(controlValue(model.settings(), c) == controlValue(expected, c));
    auto json = toValue(score::marshall<JSONObject>(static_cast<const Process::ProcessModel&>(model)));
    JSONObject::Deserializer reader{json}; ProcessModel reopened{reader, nullptr};
    Engine before{48000., expected}, after{48000., reopened.settings()};
    const std::array<uint8_t, 3> note{0x90, 60, 110}; before.midi(note); after.midi(note);
    std::array<double, 2048> a{}, ar{}, b{}, br{};
    before.render(a.data(), ar.data(), a.size()); after.render(b.data(), br.data(), b.size());
    CHECK(a == b); CHECK(ar == br);
    CHECK(std::any_of(a.begin(), a.end(), [](double value) { return std::abs(value) > .001; }));
  });
}

TEST_CASE("PSG invalid live settings do not stop the accepted sound", "[emulation][psg][qualification][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    Settings settings;
    Executor::Node node{state, settings};
    auto engine = std::make_shared<Engine>(state.sampleRate, settings), retained = engine;
    node.install(engine, settings.model, settings.clock);
    const std::array<uint8_t, 3> note{0x90, 60, 110}; retained->midi(note);
    auto invalid = settings; invalid.clock = 0;
    node.update(invalid);
    CHECK(node.ready()); CHECK(node.error()); CHECK(retained->activeVoices() == 1);
    node.update(settings);
    CHECK(node.ready()); CHECK_FALSE(node.error()); CHECK(retained->activeVoices() == 1);
  });
}

TEST_CASE("PSG envelope ownership switches atomically and retains AY routing through undo and models", "[emulation][psg][document]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model, channelControl(0, HardwareEnvelope)).setValue(true);
    control(model, AyEnvelopePeriod).setValue(12345);
    const auto native = model.settings();
    int publications{};
    QObject::connect(&model, &ProcessModel::settingsChanged, &model, [&] {
      ++publications;
      CHECK(validate(model.settings()) == nullptr);
    });
    CommandDispatcher<> dispatcher{doc->context().commandStack};
    dispatcher.submit<Process::SetControlValue>(control(model, SoftwareEnvelope), ossia::value{1});
    CHECK(publications == 1);
    CHECK(model.settings().softwareEnvelope);
    CHECK(model.settings().channels[0].hardwareEnvelope);
    doc->commandStack().undo();
    CHECK_FALSE(model.settings().softwareEnvelope);
    CHECK(model.settings().channels[0].hardwareEnvelope);
    doc->commandStack().redo();
    for(int chip : {2, 1, 0}) {
      control(model, ChipModel).setValue(chip);
      CHECK(model.settings().model == Model(chip));
      CHECK(model.settings().softwareEnvelope);
      CHECK(model.settings().channels[0].hardwareEnvelope);
      CHECK(model.settings().envelopePeriod == native.envelopePeriod);
    }
    auto legacy = model.savePreset();
    legacy.data = R"([[12,{"Bool":true}],[20,{"Bool":true}]])";
    model.loadPreset(legacy);
    CHECK(validate(model.settings()) == nullptr);
    auto json = toValue(score::marshall<JSONObject>(*base));
    JSONObject::Deserializer reader{json}; ProcessModel restored{reader, nullptr};
    CHECK(restored.settings().softwareEnvelope);
    CHECK(restored.settings().channels[0].hardwareEnvelope);
    CHECK(restored.settings().envelopePeriod == native.envelopePeriod);
    control(model, SoftwareEnvelope).setValue(0);
    Engine engine{48000., model.settings()};
    const std::array<uint8_t, 3> note{0x90, 60, 127}; engine.midi(note);
    CHECK(engine.registerValue(8) == 16);
  });
}

TEST_CASE("PSG rejected live values never publish or save and edits preserve engine diagnostics", "[emulation][psg][document]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    const auto before = model.settings();
    int publications{};
    QObject::connect(&model, &ProcessModel::settingsChanged, &model, [&] { ++publications; });
    control(model, Clock).setValue(0);
    CHECK(publications == 0);
    CHECK(model.settings().clock == before.clock);
    CHECK(ossia::convert<int>(control(model, Clock).value()) == before.clock);
    control(model, SoftwareEnvelope).setValue(2);
    CHECK(publications == 0);
    auto json = toValue(score::marshall<JSONObject>(*base));
    JSONObject::Deserializer reader{json}; ProcessModel restored{reader, nullptr};
    CHECK(validate(restored.settings()) == nullptr);
    model.setEngineError(QStringLiteral("Preparation failed"));
    control(model, Volume).setValue(.5f);
    CHECK(model.error() == QStringLiteral("Preparation failed"));
  });
}

TEST_CASE("PSG schema one toggle migrates its identity and conflicting authored routing", "[emulation][psg][document][migration]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model, channelControl(1, HardwareEnvelope)).setValue(true);
    Process::Toggle legacy{true, QStringLiteral("Software ADSR"), Id<Process::Port>{SoftwareEnvelope + 1}, nullptr};
    legacy.setExposed(QStringLiteral("envelope-owner"));
    legacy.setDescription(QStringLiteral("Authored source")); legacy.setScriptable(true);
    legacy.setAddress(State::AddressAccessor{State::Address{QStringLiteral("synth"), QStringList{QStringLiteral("adsr")}}});
    const auto checkMigration = [&](ProcessModel& restored) {
      auto& inlet = control(restored, SoftwareEnvelope);
      CHECK(inlet.id() == legacy.id());
      CHECK(inlet.address() == legacy.address());
      CHECK(inlet.exposed() == legacy.exposed());
      CHECK(inlet.description() == legacy.description());
      CHECK(inlet.scriptable() == legacy.scriptable());
      CHECK(restored.settings().softwareEnvelope);
      CHECK(restored.settings().channels[1].hardwareEnvelope);
      CHECK(validate(restored.settings()) == nullptr);
      CHECK(qobject_cast<Process::ComboBox*>(&inlet) != nullptr);
      control(restored, SoftwareEnvelope).setValue(0);
      CHECK_FALSE(restored.settings().softwareEnvelope);
      CHECK(restored.settings().channels[1].hardwareEnvelope);
    };
    auto json = toValue(score::marshall<JSONObject>(*base));
    json["Schema"].SetInt(1);
    auto port = toValue(score::marshall<JSONObject>(static_cast<const Process::Inlet&>(legacy)));
    json["Inlets"][SoftwareEnvelope + 1].CopyFrom(port, json.GetAllocator());
    JSONObject::Deserializer reader{json}; ProcessModel restored{reader, nullptr};
    checkMigration(restored);
    auto ports = model.inlets(); ports[SoftwareEnvelope + 1] = &legacy;
    QByteArray binary; DataStream::Serializer writer{&binary};
    TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
    writer.read(static_cast<const Process::ProcessModel&>(model));
    writer.m_stream << quint32{1} << QStringLiteral("emu2149+nuked-psg") << QStringLiteral("AY-3-8910");
    readPorts(writer, ports, model.outlets()); writer.insertDelimiter();
    DataStream::Deserializer binaryReader{binary}; ProcessModel binaryRestored{binaryReader, nullptr};
    checkMigration(binaryRestored);
  });
}

TEST_CASE("PSG executor clears rejected MIDI diagnostics when valid channel events recover", "[emulation][psg][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    Settings settings; Executor::Node node{state, settings};
    auto engine = std::make_shared<Engine>(state.sampleRate, settings), retained = engine;
    node.install(engine, settings.model, settings.clock);
    auto invalid = libremidi::from_midi1::note_on(0, 60, 127);
    invalid.data[0] |= 1u << 24;
    node.midi->data.messages.push_back(invalid);
    auto token = tokenFor(state);
    const auto run = [&] {
      state.samples_since_start += state.bufferSize;
      ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
      node.run(token, ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    };
    run(); CHECK(node.error() != nullptr); CHECK(retained->activeVoices() == 0);
    node.midi->data.messages.clear();
    node.midi->data.messages.push_back(libremidi::from_midi1::note_on(0, 60, 127));
    run(); CHECK(node.error() == nullptr); CHECK(retained->activeVoices() == 1);
  });
}

TEST_CASE("PSG legacy allocation selectors gain mono without changing saved routing", "[emulation][psg][document][allocation]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    auto& selector = static_cast<Process::ComboBox&>(control(model, VoiceAssignment));
    selector.setAlternatives({{"Poly", 0}, {"Fixed", 1}});
    selector.setValue(1); selector.setInit(1);
    const auto json = toValue(score::marshall<JSONObject>(*base));
    JSONObject::Deserializer reader{json}; ProcessModel restored{reader, nullptr};
    QByteArray bytes; DataStream::Serializer writer{&bytes};
    TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
    writer.read(static_cast<const Process::ProcessModel&>(model)); writer.read(model);
    DataStream::Deserializer binaryReader{bytes}; ProcessModel binary{binaryReader, nullptr};
    for(auto* copy : {&restored, &binary}) {
      REQUIRE(copy->settings().assignment == Assignment::Fixed);
      auto& mode = static_cast<Process::ComboBox&>(control(*copy, VoiceAssignment));
      const auto mono = std::find_if(mode.alternatives.begin(), mode.alternatives.end(),
          [](const auto& choice) { return choice.second == ossia::value{int(Assignment::Mono)}; });
      REQUIRE(mono != mode.alternatives.end());
      mode.setValue(mono->second);
      REQUIRE(copy->settings().assignment == Assignment::Mono);
      const auto preset = copy->savePreset();
      mode.setValue(0); copy->loadPreset(preset);
      Engine engine{48000., copy->settings()};
      const std::array<uint8_t, 3> note{0x90, 60, 100}; engine.midi(note);
      CHECK(engine.activeVoices() == 3);
    }
  });
}
