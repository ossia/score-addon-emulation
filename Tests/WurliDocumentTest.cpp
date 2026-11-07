// SPDX-License-Identifier: GPL-3.0-or-later
#include <Process/Commands/LoadPresetCommandFactory.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <Process/Preset.hpp>
#include <Process/ProcessList.hpp>
#include <Process/ExecutionTransaction.hpp>
#include <Emulation/ExecutionProcess.hpp>
#include <Emulation/Wurli/Controls.hpp>
#include <Emulation/Volume.hpp>
#include <Emulation/Wurli/Executor/Node.hpp>
#include <Emulation/Wurli/ProcessModel.hpp>
#include <Execution/DocumentPlugin.hpp>
#include <score/model/EntitySerialization.hpp>
#include <score/serialization/VisitorCommon.hpp>
#include <core/command/CommandStack.hpp>
#include <core/document/Document.hpp>
#include <ossia/dataflow/execution_state.hpp>
#include <libremidi/ump_events.hpp>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <catch2/catch_test_macros.hpp>
#include <score_test/App.hpp>
#include <score_test/Project.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
namespace
{
using namespace Emulation::Wurli;
constexpr char uuid[] = "270d9ee2-2a2d-48fd-8bac-9e8ba73a5f4d";
Process::ControlInlet& control(ProcessModel& p, int c)
{ return *static_cast<Process::ControlInlet*>(p.inlets()[c + 1]); }
void drain(Execution::DocumentPlugin& plugin)
{
  const auto& context = plugin.context();
  Execution::ExecutionCommand command;
  ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
  while(context.executionQueue.try_dequeue(command)) {
    command(); context.gcQueue.enqueue(Execution::gc(std::move(command)));
  }
  ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
  Execution::GCCommand retired;
  while(context.gcQueue.try_dequeue(retired)) retired();
}
ossia::token_request tokenFor(const ossia::execution_state& state, int frames)
{
  ossia::token_request token;
  token.prev_date = ossia::time_value{0};
  token.date = ossia::time_value{int64_t(frames * ossia::flicks_per_second<double> / state.sampleRate)};
  token.start_sample = 0; token.length_sample = frames; return token;
}
}
TEST_CASE("Wurlitzer native semantic controls round-trip JSON and binary documents", "[emulation][wurli][document]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    REQUIRE(model.inlets().size() == ControlCount + 1); REQUIRE(model.outlets().size() == 1);
    control(model, Volume).setValue(.25f); control(model, Amplifier).setValue(.82f);
    control(model, Tremolo).setValue(.13f); control(model, Speaker).setValue(.67f);
    control(model, Correction).setValue(true); control(model, VelocityCurve).setValue(2);
    control(model, VoiceAllocation).setValue(2);
    const auto expected = model.settings();
    REQUIRE(model.error().isEmpty());
    const auto json = toValue(score::marshall<JSONObject>(*base));
    JSONObject::Deserializer reader{json}; ProcessModel restored{reader, nullptr};
    CHECK(restored.settings() == expected);
    control(restored, Correction).setValue(false);
    CHECK_FALSE(restored.settings().correction); CHECK(model.settings().correction);
    QByteArray bytes; DataStream::Serializer writer{&bytes};
    TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
    writer.read(static_cast<const Process::ProcessModel&>(model));
    writer.read(model);
    DataStream::Deserializer binaryReader{bytes};
    ProcessModel binary{binaryReader, nullptr};
    CHECK(binary.settings() == expected);
    CHECK(ossia::convert<float>(control(binary, Volume).value()) == .25f);
  });
}

TEST_CASE("Wurlitzer boosted Volume round-trips without scaling old values or Amplifier", "[emulation][wurli][document][preset]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    for(float gain : {0.f, .375f, 1.f, 4.5f, Emulation::MaxVolume}) {
      control(model, Volume).setValue(gain); control(model, Volume).setInit(.625f);
      control(model, Volume).setDomain(ossia::make_domain(0.f, gain <= 1.f ? 1.f : Emulation::MaxVolume));
      const auto expected = model.settings(); REQUIRE(expected.volume == gain);
      const auto json = toValue(score::marshall<JSONObject>(*base));
      JSONObject::Deserializer reader{json}; ProcessModel restored{reader, nullptr};
      QByteArray bytes; DataStream::Serializer writer{&bytes};
      TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
      writer.read(static_cast<const Process::ProcessModel&>(model)); writer.read(model);
      DataStream::Deserializer binaryReader{bytes}; ProcessModel binary{binaryReader, nullptr};
      for(auto* copy : {&restored, &binary}) {
        CHECK(copy->settings() == expected);
        CHECK(control(*copy, Volume).value() == ossia::value{gain});
        CHECK(control(*copy, Volume).init() == ossia::value{.625f});
        CHECK(control(*copy, Volume).domain().get().convert_min<float>() == 0.f);
        CHECK(control(*copy, Volume).domain().get().convert_max<float>() == Emulation::MaxVolume);
        CHECK(control(*copy, Amplifier).domain().get().convert_max<float>() == 1.f);
        ossia::value_port source; source.write_value(gain, 7);
        ossia::value_inlet sink; control(*copy, Volume).setupExecution(sink, nullptr);
        sink.data.add_port_values(source);
        REQUIRE(sink.data.get_data().size() == 1);
        CHECK(sink.data.get_data().front().value == ossia::value{gain});
        CHECK(sink.data.get_data().front().timestamp == 7);
      }
      const auto preset = model.savePreset();
      control(restored, Volume).setValue(gain == 0.f ? 1.f : 0.f);
      restored.loadPreset(preset); CHECK(restored.settings() == expected); CHECK(restored.error().isEmpty());
      for(float bad : {-1.f, Emulation::MaxVolume + .01f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        control(model, Volume).setValue(bad); CHECK(model.settings() == expected);
        CHECK(control(model, Volume).value() == ossia::value{gain}); CHECK_FALSE(model.error().isEmpty());
      }
    }
  });
}
TEST_CASE("Wurlitzer original native presets load atomically and undo restores all controls", "[emulation][wurli][preset][document]")
{
  score::test::run_in_gui_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    auto& factories = ctx.interfaces<Process::ProcessFactoryList>();
    auto& commands = ctx.interfaces<Process::LoadPresetCommandFactoryList>();
    const auto directory = QFileInfo{QString::fromUtf8(__FILE__)}.absoluteDir();
    for(const char* name : {"Reed room", "Dry felt", "Evening vibrato", "Bright cabinet"}) {
      QFile file{directory.filePath(QStringLiteral("../Presets/Wurli/%1.scp").arg(QString::fromLatin1(name)))};
      REQUIRE(file.open(QIODevice::ReadOnly));
      auto preset = Process::Preset::fromJson(factories, file.readAll()); REQUIRE(preset);
      CHECK(model.presetMatches(*preset));
      control(model, VoiceAllocation).setValue(0);
      const auto before = model.settings();
      int changes{};
      const auto connection = QObject::connect(&model, &ProcessModel::settingsChanged, &model, [&] { ++changes; });
      auto* command = commands.make(&Process::LoadPresetCommandFactory::make, model, *preset, doc->context());
      REQUIRE(command); CommandDispatcher<>{doc->context().commandStack}.submit(command);
      CHECK(model.error().isEmpty()); CHECK(changes == 1);
      const auto after = model.settings();
      CHECK(after.voiceAllocation == Allocation::Poly);
      doc->commandStack().undo(); CHECK(model.settings() == before);
      doc->commandStack().redo(); CHECK(model.settings() == after);
      QObject::disconnect(connection);
      Engine engine{48000., after}; const std::array<uint8_t, 3> on{0x90, 60, 100}; engine.midi(on);
      std::array<double, 2048> left{}, right{}; engine.render(left.data(), right.data(), left.size());
      CHECK(left == right);
      CHECK(std::any_of(left.begin(), left.end(), [](double v) { return std::abs(v) > 1e-6; }));
      CHECK(std::all_of(left.begin(), left.end(), [](double v) { return std::isfinite(v); }));
    }
    const auto saved = model.savePreset(); const auto before = model.settings();
    int changes{};
    const auto connection = QObject::connect(&model, &ProcessModel::settingsChanged, &model, [&] { ++changes; });
    for(const char* malformed : {"{}", "[[1,{\"Float\":0.1}],[6,{\"Int\":4}]]",
        "[[1,{\"Float\":0.1}],[1,{\"Float\":0.2}]]", "[[5,{\"Float\":1}]]", "[[1,{\"Float\":33}]]", "[[1,{\"Float\":-0.1}]]", "[[1]]",
        "[[7,{\"Int\":3}]]", "[[7,{\"Float\":1}]]"}) {
      auto invalid = saved; invalid.data = QByteArray{malformed}; model.loadPreset(invalid);
      CHECK(model.settings() == before); CHECK(changes == 0); CHECK_FALSE(model.error().isEmpty());
      CHECK(ossia::convert<float>(control(model, Volume).value()) == before.volume);
    }
    QObject::disconnect(connection);
    model.loadPreset(saved); CHECK(model.error().isEmpty()); CHECK(model.settings() == before);
  });
}
TEST_CASE("Wurlitzer token windows preserve MIDI timing volume automation and pause cleanup", "[emulation][wurli][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plugin = doc->context().plugin<Execution::DocumentPlugin>();
    plugin.reload(true, score::test::base_interval(*doc)); drain(plugin);
    auto& state = *plugin.context().execState;
    REQUIRE(state.bufferSize >= 64);
    const int frames = state.bufferSize, quarter = frames / 4;
    Settings s; s.volume = 1.f;
    Executor::Node reference{state, s}, adjusted{state, s};
    for(auto* n : {&reference, &adjusted}) {
      auto engine = std::make_shared<Engine>(state.sampleRate, s); n->install(engine); REQUIRE(n->ready());
      auto note = libremidi::from_midi1::note_on(0, 60, 100); note.timestamp = quarter;
      n->midi->data.messages.push_back(note);
    }
    adjusted.controls[Volume]->data.write_value(Emulation::MaxVolume, 2 * quarter);
    adjusted.controls[Volume]->data.write_value(0.f, 3 * quarter);
    auto token = tokenFor(state, frames), first = token, second = token;
    first.length_sample = 2 * quarter;
    first.date = ossia::time_value{int64_t(2 * quarter * ossia::flicks_per_second<double> / state.sampleRate)};
    second.prev_date = first.date; second.start_sample = 2 * quarter; second.length_sample = frames - 2 * quarter;
    state.samples_since_start += frames;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    reference.run(token, ossia::exec_state_facade{&state});
    adjusted.run(first, ossia::exec_state_facade{&state}); adjusted.run(second, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    const auto& a = reference.audio->data.channel(0); const auto& b = adjusted.audio->data.channel(0);
    for(int i = 0; i < frames; ++i) {
      CHECK(b[i] == a[i] * (i < 2 * quarter ? 1. : i < 3 * quarter ? Emulation::MaxVolume : 0.));
      if(i < quarter) CHECK(a[i] == 0.);
    }
    reference.midi->data.messages.clear();
    auto paused = token; paused.date = paused.prev_date; paused.length_sample = 0;
    state.samples_since_start += frames;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    reference.run(paused, ossia::exec_state_facade{&state});
    state.samples_since_start += frames; reference.run(token, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    CHECK(std::all_of(a.begin(), a.end(), [](double v) { return v == 0.; }));
    for(int i = 0; i < 8193; ++i) reference.midi->data.messages.push_back(libremidi::from_midi1::note_on(0, 60, 100));
    state.samples_since_start += frames;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0); reference.run(token, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    CHECK(reference.error()); CHECK(std::all_of(a.begin(), a.end(), [](double v) { return v == 0.; }));
  });
}
TEST_CASE("Wurlitzer restores recycled stereo buffers and transport pause silences without a paused token", "[emulation][wurli][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plugin = doc->context().plugin<Execution::DocumentPlugin>();
    plugin.reload(true, score::test::base_interval(*doc)); drain(plugin);
    auto& state = *plugin.context().execState;
    auto node = std::make_shared<Executor::Node>(state);
    auto engine = std::make_shared<Engine>(state.sampleRate); auto retained = engine;
    node->install(engine); Emulation::ExecutionProcess process{node};
    const int frames = std::max(128, int(state.bufferSize) * 2);
    state.bufferSize = frames;
    auto token = tokenFor(state, frames);
    node->audio->data.set_channels(0);
    node->midi->data.messages.push_back(libremidi::from_midi1::note_on(0, 60, 100));
    state.samples_since_start += frames;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    node->run(token, ossia::exec_state_facade{&state});
    process.pause();
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    CHECK(retained->activeVoices() == 0);
    REQUIRE(node->audio->data.channels() == 2);
    CHECK(node->audio->data.channel(0).size() >= std::size_t(frames));
    node->midi->data.messages.clear();
    // Same-tick restart after a transport hook must not be rejected as a duplicate.
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0); node->run(token, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    const auto& output = node->audio->data.channel(0);
    CHECK(std::all_of(output.begin(), output.end(), [](double v) { return v == 0.; }));
  });
}

TEST_CASE("Wurlitzer native execution recovers rejected MIDI independently of automation errors", "[emulation][wurli][execution][errors]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plugin = doc->context().plugin<Execution::DocumentPlugin>();
    plugin.reload(true, score::test::base_interval(*doc)); drain(plugin);
    auto& state = *plugin.context().execState;
    Settings settings;
    Executor::Node node{state, settings};
    auto engine = std::make_shared<Engine>(state.sampleRate, settings);
    node.install(engine); REQUIRE(node.ready());
    const auto token = tokenFor(state, state.bufferSize);
    const auto send = [&](libremidi::ump message) {
      node.midi->data.messages.clear(); message.timestamp = 0;
      node.midi->data.messages.push_back(message);
      state.samples_since_start += state.bufferSize;
      ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
      node.run(token, ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    };
    send(libremidi::ump_events::note_on(1, 0, 60, 50000));
    REQUIRE(node.error());
    settings.volume = .5f; node.update(settings);
    CHECK(node.error());
    send(libremidi::from_midi1::note_on(0, 60, 100));
    CHECK_FALSE(node.error());
    auto invalid = settings; invalid.speaker = 2.f;
    node.update(invalid); REQUIRE(node.error());
    send(libremidi::from_midi1::control_change(0, 121, 0));
    CHECK(node.error());
    node.update(settings); CHECK_FALSE(node.error());
  });
}

TEST_CASE("Wurlitzer authored edits and preset recall cannot clear preparation or execution errors", "[emulation][wurli][document][errors]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    const auto preset = model.savePreset();
    const auto preparation = QStringLiteral("Wurlitzer preparation unavailable");
    const auto runtime = QStringLiteral("Wurlitzer circuit error");
    model.setPreparationError(preparation);
    control(model, Volume).setValue(.5f);
    CHECK(model.error() == preparation);
    model.loadPreset(preset);
    CHECK(model.error() == preparation);
    model.setEngineError(runtime);
    model.setEngineError({});
    CHECK(model.error() == preparation);
    model.setEngineError(runtime);
    model.setPreparationError({});
    CHECK(model.error() == runtime);
    control(model, Volume).setValue(.25f);
    model.loadPreset(preset);
    CHECK(model.error() == runtime);
    model.setEngineError({});
    CHECK(model.error().isEmpty());
  });
}

TEST_CASE("Wurlitzer legacy documents append Poly allocation without changing saved controls", "[emulation][wurli][document][allocation]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model, Volume).setValue(.35f);
    control(model, VelocityCurve).setValue(3);
    const auto expected = model.settings();
    auto json = toValue(score::marshall<JSONObject>(*base));
    json["Schema"].SetInt(1); json["Inlets"].PopBack();
    JSONObject::Deserializer reader{json}; ProcessModel restored{reader, nullptr};
    auto ports = model.inlets(); ports.pop_back();
    QByteArray bytes; DataStream::Serializer writer{&bytes};
    TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
    writer.read(static_cast<const Process::ProcessModel&>(model));
    writer.m_stream << quint32{1} << QStringLiteral("openwurli") << QStringLiteral("200A");
    readPorts(writer, ports, model.outlets()); writer.insertDelimiter();
    DataStream::Deserializer binaryReader{bytes}; ProcessModel binary{binaryReader, nullptr};
    for(auto* copy : {&restored, &binary}) {
      CHECK(copy->settings() == expected);
      REQUIRE(copy->inlets().size() == ControlCount + 1);
      CHECK(control(*copy, VoiceAllocation).id().val() == 7);
      CHECK(control(*copy, VoiceAllocation).value() == ossia::value{1});
      control(*copy, VoiceAllocation).setValue(0);
      CHECK(copy->settings().voiceAllocation == Allocation::Mono);
      const auto preset = copy->savePreset();
      control(*copy, VoiceAllocation).setValue(2);
      copy->loadPreset(preset);
      CHECK(copy->settings().voiceAllocation == Allocation::Mono);
      control(*copy, VoiceAllocation).setValue(3);
      CHECK(copy->settings().voiceAllocation == Allocation::Mono);
      CHECK_FALSE(copy->error().isEmpty());
    }
  });
}

TEST_CASE("Wurlitzer allocation automation stops old voices at its sample and retains FIFO ownership", "[emulation][wurli][execution][allocation]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plugin = doc->context().plugin<Execution::DocumentPlugin>();
    plugin.reload(true, score::test::base_interval(*doc)); drain(plugin);
    auto& state = *plugin.context().execState;
    REQUIRE(state.bufferSize >= 64);
    const int frames = state.bufferSize, half = frames / 2;
    Executor::Node node{state};
    auto engine = std::make_shared<Engine>(state.sampleRate); auto retained = engine;
    node.install(engine); REQUIRE(node.ready());
    auto on = libremidi::from_midi1::note_on(0, 60, 100); on.timestamp = 0;
    node.midi->data.messages.push_back(on);
    node.controls[VoiceAllocation]->data.write_value(0, half);
    const auto token = tokenFor(state, frames);
    const auto run = [&] {
      state.samples_since_start += frames;
      ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
      node.run(token, ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
      CHECK_FALSE(node.error());
    };
    run(); CHECK(retained->activeVoices() == 0);
    const auto& audio = node.audio->data.channel(0);
    CHECK(std::any_of(audio.begin(), audio.begin() + half, [](double v) { return std::abs(v) > 1e-6; }));
    CHECK(std::all_of(audio.begin() + half, audio.begin() + frames, [](double v) { return v == 0.; }));
    node.controls[VoiceAllocation]->data.get_data().clear();
    node.midi->data.messages.clear();
    node.midi->data.messages.push_back(on);
    auto other = libremidi::from_midi1::note_on(1, 67, 90); other.timestamp = 1;
    node.midi->data.messages.push_back(other);
    run(); CHECK(retained->heldVoices() == 1);
    node.midi->data.messages.clear();
    auto off = libremidi::from_midi1::note_off(0, 60, 0); off.timestamp = 0;
    node.midi->data.messages.push_back(off);
    run(); CHECK(retained->heldVoices() == 1); // Consumes pre-change debt only.
    node.midi->data.messages.clear();
    auto otherOff = libremidi::from_midi1::note_off(1, 67, 0); otherOff.timestamp = 0;
    node.midi->data.messages.push_back(otherOff);
    run(); CHECK(retained->heldVoices() == 1); // Falls back to the new 60.
    node.midi->data.messages.clear(); node.midi->data.messages.push_back(off);
    run(); CHECK(retained->heldVoices() == 0);
  });
}
