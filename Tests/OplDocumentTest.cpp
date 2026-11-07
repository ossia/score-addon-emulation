// SPDX-License-Identifier: GPL-3.0-or-later
#include <Process/ExecutionTransaction.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <Process/Dataflow/PortItem.hpp>
#include <Process/ProcessContext.hpp>
#include <Process/Commands/SetControlValue.hpp>
#include <core/command/CommandStack.hpp>
#include <score/command/Dispatchers/CommandDispatcher.hpp>
#include <Emulation/Opl/Controls.hpp>
#include <Emulation/Opl/Executor/Node.hpp>
#include <Emulation/Opl/ProcessModel.hpp>
#include <Emulation/Volume.hpp>
#include <Execution/BaseScenarioComponent.hpp>
#include <Execution/DocumentPlugin.hpp>
#include <Scenario/Document/Interval/IntervalExecution.hpp>
#include <Scenario/Document/ScenarioDocument/ScenarioDocumentPresenter.hpp>
#include <score/document/DocumentInterface.hpp>
#include <score/graphics/RectItem.hpp>
#include <score/graphics/layouts/GraphicsTabLayout.hpp>
#include <score/graphics/layouts/GraphicsGridLayout.hpp>
#include <score/model/EntitySerialization.hpp>
#include <score/serialization/VisitorCommon.hpp>
#include <ossia/dataflow/execution_state.hpp>
#include <ossia/dataflow/data_copy.hpp>
#include <libremidi/ump_events.hpp>
#include <catch2/catch_test_macros.hpp>
#include <score_test/App.hpp>
#include <score_test/Events.hpp>
#include <score_test/Project.hpp>
#include <QFile>
#include <QGraphicsScene>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace
{
using namespace Emulation::Opl;
Process::ControlInlet& control(ProcessModel& p, int index)
{ return *static_cast<Process::ControlInlet*>(p.inlets()[index + 1]); }
constexpr auto uuid = "2fe73b91-4100-4cbb-b8d8-10e82c9f317a";
// Match the production audio tick: execute on Audio, retire captures on UI.
// runAllCommands() is a synchronous setup drain, not a realtime GC harness.
void drain(Execution::DocumentPlugin& plug)
{
  const auto& context = plug.context();
  Execution::ExecutionCommand command;
  ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
  while(context.executionQueue.try_dequeue(command))
  {
    command();
    context.gcQueue.enqueue(Execution::gc(std::move(command)));
  }
  ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
  Execution::GCCommand retired;
  while(context.gcQueue.try_dequeue(retired)) retired();
}
}

TEST_CASE("OPL gain domains migrate without rescaling saved values and presets", "[emulation][opl][document][preset]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    for(float gain : {0.f, .375f, 1.f, 4.5f, Emulation::MaxVolume}) {
      CAPTURE(gain);
      control(model, Volume).setValue(gain);
      control(model, Volume).setInit(.625f);
      control(model, Volume).setDomain(ossia::make_domain(0.f, gain <= 1.f ? 1.f : Emulation::MaxVolume));
      const auto json = toValue(score::marshall<JSONObject>(*base));
      JSONObject::Deserializer reader{json}; ProcessModel restored{reader, nullptr};
      QByteArray bytes; DataStream::Serializer writer{&bytes};
      TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
      writer.read(static_cast<const Process::ProcessModel&>(model)); writer.read(model);
      DataStream::Deserializer binaryReader{bytes}; ProcessModel binary{binaryReader, nullptr};
      for(auto* copy : {&restored, &binary}) {
        CHECK(control(*copy, Volume).value() == ossia::value{gain});
        CHECK(control(*copy, Volume).init() == ossia::value{.625f});
        CHECK(control(*copy, Volume).id() == Id<Process::Port>{Volume + 1});
        CHECK(control(*copy, Volume).domain().get().convert_min<float>() == 0.f);
        CHECK(control(*copy, Volume).domain().get().convert_max<float>() == Emulation::MaxVolume);
        CHECK(control(*copy, operatorControl(0, Level)).domain().get().convert_max<int>() == 63);
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
      CHECK(restored.error().isEmpty());
      CHECK(control(restored, Volume).value() == ossia::value{gain});
      for(float invalid : {-1.f, Emulation::MaxVolume + 1.f,
          std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        control(restored, Volume).setValue(invalid);
        CHECK(control(restored, Volume).value() == ossia::value{gain});
        CHECK_FALSE(restored.error().isEmpty());
      }
      for(float invalid : {-1.f, Emulation::MaxVolume + 1.f}) {
        auto rejected = preset;
        rejected.data = "[[" + QByteArray::number(Volume + 1) + ", {\"Float\":"
            + QByteArray::number(invalid) + "}]]";
        restored.loadPreset(rejected);
        CHECK(control(restored, Volume).value() == ossia::value{gain});
        CHECK_FALSE(restored.error().isEmpty());
      }
    }
  });
}

TEST_CASE("OPL edited banks recall without their original file", "[emulation][opl][document]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {});
    REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("owned.wopl"));
    auto fixtureBank = factoryBank();
    fixtureBank.flags = (fixtureBank.flags & ~3) | 3;
    const auto fixtureBytes = encodeBank(fixtureBank);
    {
      QFile fixture{path};
      REQUIRE(fixture.open(QIODevice::WriteOnly));
      REQUIRE(fixture.write(reinterpret_cast<const char*>(fixtureBytes.data()),
          static_cast<qint64>(fixtureBytes.size())) == static_cast<qint64>(fixtureBytes.size()));
    }
    control(model, DeepVibrato).setValue(false);
    control(model, DeepTremolo).setValue(false);
    model.loadFile(path);
    REQUIRE(score::test::wait_until([&] { return bool(model.bank()); }));
    CHECK((model.bank()->flags & 3) == 3);
    CHECK(model.settings().deepVibrato);
    CHECK(model.settings().deepTremolo);
    control(model, operatorControl(0, Level)).setValue(37);
    control(model, Detune).setValue(7);
    control(model, Volume).setValue(.25f);
    const auto expected = encodeBank(*model.bank());
    const auto preset = model.savePreset();
    const auto json = toValue(score::marshall<JSONObject>(*base));
    REQUIRE(QFile::remove(path));
    JSONObject::Deserializer reader{json};
    ProcessModel restored{reader, nullptr};
    REQUIRE(restored.bank());
    CHECK(encodeBank(*restored.bank()) == expected);
    CHECK((findProgram(*restored.bank(), restored.selectedAddress())->instrument.operators[0].ksl_l_40 & 63) == 37);
    CHECK(restored.filePath() == path);
    CHECK(ossia::convert<float>(control(restored, Volume).value()) == .25f);
    QByteArray binary;
    DataStream::Serializer writer{&binary};
    TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
    writer.read(static_cast<const Process::ProcessModel&>(model));
    writer.read(model);
    DataStream::Deserializer binaryReader{binary};
    ProcessModel binaryRestored{binaryReader, nullptr};
    REQUIRE(binaryRestored.bank());
    CHECK(encodeBank(*binaryRestored.bank()) == expected);
    CHECK(ossia::convert<float>(control(binaryRestored, Volume).value()) == .25f);
    restored.loadFile({});
    REQUIRE_FALSE(restored.bank());
    REQUIRE(restored.presetMatches(preset));
    restored.loadPreset(preset);
    REQUIRE(restored.bank());
    CHECK(encodeBank(*restored.bank()) == expected);
    CHECK(restored.filePath() == path);
    CHECK(restored.error().isEmpty());
    model.loadFile(path);
    const auto pendingError = model.error();
    REQUIRE(score::test::wait_until([&] {
      return !model.error().isEmpty() && model.error() != pendingError;
    }));
    CHECK_FALSE(model.bank());
  });
}

TEST_CASE("OPL document execution honors timestamps and inline patch edits", "[emulation][opl][document]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {});
    REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc));
    drain(plug);
    auto& processes = plug.baseScenario()->baseInterval().processes();
    const auto found = processes.find(base->id());
    REQUIRE(found != processes.end());
    auto node = std::static_pointer_cast<Emulation::Opl::Executor::Node>(found->second->node);
    REQUIRE(score::test::wait_until([&] { drain(plug); return node->ready(); }));
    auto& state = *plug.context().execState;
    REQUIRE(state.bufferSize >= 64);
    const int frames = state.bufferSize;
    auto note = libremidi::from_midi1::note_on(0, 60, 110);
    note.timestamp = frames / 2;
    node->midi->data.messages.push_back(note);
    ossia::token_request token;
    token.prev_date = ossia::time_value{0};
    token.date = ossia::time_value{int64_t(frames * ossia::flicks_per_second<double> / state.sampleRate)};
    token.start_sample = 0;
    token.length_sample = frames;
    state.samples_since_start += frames;
    auto first = token;
    first.length_sample = frames / 2;
    first.date = ossia::time_value{int64_t((frames / 2) * ossia::flicks_per_second<double> / state.sampleRate)};
    auto second = token;
    second.prev_date = first.date;
    second.start_sample = frames / 2;
    second.length_sample = frames - frames / 2;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    node->run(first, ossia::exec_state_facade{&state});
    node->run(second, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    const auto& left = node->audio->data.channel(0);
    REQUIRE(left.size() >= std::size_t(frames));
    CHECK(std::all_of(left.begin(), left.begin() + frames / 2, [](double v) { return v == 0.; }));
    CHECK(std::any_of(left.begin() + frames / 2, left.begin() + frames, [](double v) { return std::abs(v) > 1e-6; }));
    CHECK(std::all_of(left.begin(), left.end(), [](double v) { return std::isfinite(v); }));
    const std::vector<double> firstWindow{left.begin(), left.begin() + frames};
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    node->run(token, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    CHECK(std::equal(firstWindow.begin(), firstWindow.end(), left.begin()));
    node->midi->data.messages.clear();

    // A native patch edit must reach the prepared execution, not just the UI.
    control(model, Blank).setValue(true);
    drain(plug);
    node->midi->data.messages.push_back(libremidi::from_midi1::note_on(0, 64, 110));
    double peak = 0.;
    for(int i = 0; i < 100; ++i)
    {
      state.samples_since_start += frames;
      ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
      node->run(token, ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
      node->midi->data.messages.clear();
      if(i == 99)
        for(double sample : node->audio->data.channel(0)) peak = std::max(peak, std::abs(sample));
    }
    CHECK(peak < 1e-6);
  });
}

TEST_CASE("OPL legacy documents migrate volume and instrument selection without changing sound", "[emulation][opl][document]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model, ProgramNumber).setValue(6);
    control(model, operatorControl(1, Level)).setValue(29);
    control(model, Volume).setValue(.25f);
    const auto expected = encodeBank(*model.bank());
    for(int schema : {1, 2, 3})
    {
      CAPTURE(schema);
      auto json = toValue(score::marshall<JSONObject>(*base));
      json["Schema"].SetInt(schema);
      json["Inlets"].PopBack();
      if(schema <= 2) json["Inlets"].PopBack();
      if(schema == 1) json["Inlets"].PopBack();
      JSONObject::Deserializer reader{json};
      ProcessModel restored{reader, nullptr};
      CHECK(ossia::convert<float>(control(restored, Volume).value()) == (schema == 1 ? 1.f : .25f));
      CHECK(ossia::convert<int>(control(restored, InstrumentIndex).value()) == 6);
      CHECK(ossia::convert<int>(control(restored, VoiceAllocation).value()) == 1);
      CHECK(encodeBank(*restored.bank()) == expected);
      control(restored, Volume).setValue(.3f);
      control(restored, InstrumentIndex).setValue(2);
      CHECK(restored.selectedAddress() == Address{false, 0, 0, 2});
      CHECK(ossia::convert<float>(control(restored, Volume).value()) == .3f);
      CHECK(encodeBank(*restored.bank()) == expected);

      QByteArray binary;
      DataStream::Serializer writer{&binary};
      TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
      writer.read(static_cast<const Process::ProcessModel&>(model));
      writer.m_stream << quint32(schema) << QStringLiteral("ymfm") << model.filePath()
          << QByteArray{reinterpret_cast<const char*>(expected.data()), qsizetype(expected.size())};
      auto legacyPorts = model.inlets();
      legacyPorts.pop_back();
      if(schema <= 2) legacyPorts.pop_back();
      if(schema == 1) legacyPorts.pop_back();
      readPorts(writer, legacyPorts, model.outlets());
      writer.insertDelimiter();
      DataStream::Deserializer binaryReader{binary};
      ProcessModel binaryRestored{binaryReader, nullptr};
      CHECK(ossia::convert<float>(control(binaryRestored, Volume).value()) == (schema == 1 ? 1.f : .25f));
      CHECK(ossia::convert<int>(control(binaryRestored, InstrumentIndex).value()) == 6);
      CHECK(ossia::convert<int>(control(binaryRestored, VoiceAllocation).value()) == 1);
      CHECK(encodeBank(*binaryRestored.bank()) == expected);
    }
  });
}

TEST_CASE("OPL volume automation scales only its timestamped render segments", "[emulation][opl][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc));
    drain(plug);
    auto& state = *plug.context().execState;
    Executor::Node reference{state}, adjusted{state};
    const auto bank = std::make_shared<const Bank>(factoryBank());
    for(auto* node : {&reference, &adjusted})
    {
      auto engine = std::make_shared<Engine>(state.sampleRate, Settings{}, *bank);
      auto published = bank;
      node->install(engine, published, Settings{}, Address{false, 0, 0, 6});
      node->midi->data.messages.push_back(libremidi::from_midi1::note_on(0, 60, 110));
    }
    const int frames = state.bufferSize;
    const int quarter = frames / 4;
    adjusted.controls[Volume]->data.write_value(Emulation::MaxVolume, quarter);
    adjusted.controls[Volume]->data.write_value(0.f, 2 * quarter);
    adjusted.controls[Volume]->data.write_value(1.f, 3 * quarter);
    // Rejected events retain the preceding accepted gain, never clamp it.
    adjusted.controls[Volume]->data.write_value(Emulation::MaxVolume + 1.f, quarter + 1);
    adjusted.controls[Volume]->data.write_value(-1.f, quarter + 2);
    adjusted.controls[Volume]->data.write_value(std::numeric_limits<float>::infinity(), quarter + 3);
    adjusted.controls[Volume]->data.write_value(std::numeric_limits<float>::quiet_NaN(), quarter + 4);
    ossia::token_request token;
    token.prev_date = ossia::time_value{0};
    token.date = ossia::time_value{int64_t(frames * ossia::flicks_per_second<double> / state.sampleRate)};
    token.start_sample = 0;
    token.length_sample = frames;
    auto first = token;
    first.length_sample = 2 * quarter;
    first.date = ossia::time_value{int64_t(2 * quarter * ossia::flicks_per_second<double> / state.sampleRate)};
    auto second = token;
    second.prev_date = first.date;
    second.start_sample = 2 * quarter;
    second.length_sample = frames - 2 * quarter;
    state.samples_since_start += frames;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    reference.run(token, ossia::exec_state_facade{&state});
    adjusted.run(first, ossia::exec_state_facade{&state});
    adjusted.run(second, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    double peak{};
    for(int channel = 0; channel < 2; ++channel)
      for(int i = 0; i < frames; ++i)
      {
        const auto original = reference.audio->data.channel(channel)[i];
        const double gain = i < quarter ? 1. : i < 2 * quarter ? Emulation::MaxVolume : i < 3 * quarter ? 0. : 1.;
        CHECK(std::abs(adjusted.audio->data.channel(channel)[i] - gain * original) < 1e-12);
        peak = std::max(peak, std::abs(original));
      }
    CHECK(peak > .001);
  });
}

TEST_CASE("OPL preserves held notes when the graph recycles smaller output buffers", "[emulation][opl][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    Executor::Node reference{state}, recycled{state};
    const auto bank = std::make_shared<const Bank>(factoryBank());
    for(auto* node : {&reference, &recycled}) {
      auto engine = std::make_shared<Engine>(state.sampleRate, Settings{}, *bank);
      auto published = bank;
      node->install(engine, published, Settings{}, Address{false, 0, 0, 6});
      node->midi->data.messages.push_back(libremidi::from_midi1::note_on(0, 60, 110));
    }
    ossia::token_request token;
    token.prev_date = ossia::time_value{0};
    token.date = ossia::time_value{int64_t(state.bufferSize * ossia::flicks_per_second<double> / state.sampleRate)};
    token.start_sample = 0; token.length_sample = state.bufferSize;
    double peak{};
    for(int tick = 0; tick < 96; ++tick) {
      ossia::clear_data{}(recycled.audio->data);
      if(tick % 2) {
        recycled.audio->data.set_channels(2);
        for(auto& channel : recycled.audio->data.get()) {
          channel.clear(); channel.shrink_to_fit();
        }
      }
      state.samples_since_start += state.bufferSize;
      ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
      reference.run(token, ossia::exec_state_facade{&state});
      recycled.run(token, ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
      REQUIRE(recycled.audio->data.channels() == 2);
      CHECK_FALSE(recycled.error());
      for(int channel = 0; channel < 2; ++channel) {
        const auto& expected = reference.audio->data.channel(channel);
        const auto& actual = recycled.audio->data.channel(channel);
        CHECK(std::equal(expected.begin(), expected.end(), actual.begin(), actual.end()));
        for(double sample : expected) peak = std::max(peak, std::abs(sample));
      }
      reference.midi->data.messages.clear(); recycled.midi->data.messages.clear();
    }
    CHECK(peak > .001);
  });
}

TEST_CASE("OPL instrument selection changes complete bank addresses and supports undo", "[emulation][opl][document]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model, Volume).setValue(.3f);
    const auto bank = model.bank();
    const Address drum{true, 0, 0, 36};
    const auto* program = findProgram(*bank, drum); REQUIRE(program);
    const int index = int(program - bank->programs.data());
    const auto original = model.selectedAddress();
    CommandDispatcher<>{doc->context().commandStack}.submit(
        new Process::SetControlValue{control(model, InstrumentIndex), index});
    CHECK(model.selectedAddress() == drum);
    CHECK(ossia::convert<int>(control(model, ProgramNumber).value()) == 36);
    CHECK(ossia::convert<bool>(control(model, Percussion).value()));
    CHECK(ossia::convert<float>(control(model, Volume).value()) == .3f);
    CHECK(model.bank() == bank);
    doc->commandStack().undo();
    CHECK(model.selectedAddress() == original);
    doc->commandStack().redo();
    CHECK(model.selectedAddress() == drum);
    auto json = toValue(score::marshall<JSONObject>(*base));
    JSONObject::Deserializer reader{json};
    ProcessModel restored{reader, nullptr};
    CHECK(restored.selectedAddress() == drum);
    CHECK(ossia::convert<int>(control(restored, InstrumentIndex).value()) == index);
    // Changing a patch's blank flag must not renumber any bank slot.
    control(model, Blank).setValue(true);
    CHECK(ossia::convert<int>(control(model, InstrumentIndex).value()) == index);
    control(model, Percussion).setValue(false);
    control(model, ProgramNumber).setValue(6);
    CHECK(ossia::convert<int>(control(model, InstrumentIndex).value()) == 6);
  });
}

TEST_CASE("OPL instrument automation selects the sounding patch before same-time MIDI", "[emulation][opl][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    // Three bounded event arenas exceed the default 1 MiB Windows test stack.
    auto storage = std::unique_ptr<std::array<Executor::Node, 3>>{
        new std::array<Executor::Node, 3>{Executor::Node{state}, Executor::Node{state}, Executor::Node{state}}};
    auto& [automated, selected, unchanged] = *storage;
    const auto bank = std::make_shared<const Bank>(factoryBank());
    const int frames = state.bufferSize, quarter = frames / 4;
    for(auto* node : {&automated, &selected, &unchanged})
    {
      auto engine = std::make_shared<Engine>(state.sampleRate, Settings{}, *bank);
      auto published = bank;
      node->install(engine, published, Settings{}, Address{false, 0, 0, uint8_t(node == &selected ? 6 : 0)});
      auto note = libremidi::from_midi1::note_on(0, 60, 110);
      note.timestamp = quarter;
      node->midi->data.messages.push_back(note);
    }
    automated.controls[InstrumentIndex]->data.write_value(6, quarter);
    ossia::token_request token;
    token.date = ossia::time_value{int64_t(frames * ossia::flicks_per_second<double> / state.sampleRate)};
    token.length_sample = frames;
    state.samples_since_start += frames;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    for(auto* node : {&automated, &selected, &unchanged})
      node->run(token, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    for(int channel = 0; channel < 2; ++channel)
    {
      const auto& actual = automated.audio->data.channel(channel);
      CHECK(actual == selected.audio->data.channel(channel));
      CHECK(actual != unchanged.audio->data.channel(channel));
      CHECK(std::all_of(actual.begin(), actual.begin() + quarter, [](double v) { return v == 0.; }));
    }
  });
}

TEST_CASE("OPL instrument undo restores unresolved bank addresses", "[emulation][opl][document][selection]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    for(const Address absent : {Address{false, 12, 45, 67}, Address{true, 0, 201, 44}})
    {
      control(model, BankMSB).setValue(int(absent.msb));
      control(model, BankLSB).setValue(int(absent.lsb));
      control(model, Percussion).setValue(absent.percussion);
      control(model, ProgramNumber).setValue(int(absent.program));
      REQUIRE(model.selectedAddress() == absent);
      REQUIRE_FALSE(findProgram(*model.bank(), absent));
      CommandDispatcher<>{doc->context().commandStack}.submit(
          new Process::SetControlValue{control(model, InstrumentIndex), 6});
      CHECK(model.selectedAddress() == Address{false, 0, 0, 6});
      doc->commandStack().undo();
      CHECK(model.selectedAddress() == absent);
      doc->commandStack().redo();
      CHECK(model.selectedAddress() == Address{false, 0, 0, 6});
    }
  });
}

TEST_CASE("OPL invalid rhythm edits preserve the accepted sounding bank", "[emulation][opl][document][selection]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    const auto original = encodeBank(*model.bank());
    control(model, RhythmMode).setValue(1);
    CHECK(encodeBank(*model.bank()) == original);
    CHECK(ossia::convert<int>(control(model, RhythmMode).value()) == 0);
    CHECK_FALSE(model.error().isEmpty());
    control(model, Percussion).setValue(true);
    control(model, ProgramNumber).setValue(36);
    control(model, RhythmMode).setValue(1);
    REQUIRE(model.error().isEmpty());
    const auto drum = encodeBank(*model.bank());
    control(model, VoiceMode).setValue(3);
    CHECK(encodeBank(*model.bank()) == drum);
    CHECK(ossia::convert<int>(control(model, VoiceMode).value()) == 0);
    CHECK_FALSE(model.error().isEmpty());
  });
}

TEST_CASE("OPL absent editing addresses do not play the prior patch", "[emulation][opl][execution][selection]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    Executor::Node node{state};
    auto bank = std::make_shared<const Bank>(factoryBank());
    auto engine = std::make_shared<Engine>(state.sampleRate, Settings{}, *bank);
    node.install(engine, bank, Settings{}, Address{false, 0, 0, 0});
    node.select(Address{false, 12, 45, 67});
    const int frames = state.bufferSize;
    ossia::token_request token;
    token.date = ossia::time_value{int64_t(frames * ossia::flicks_per_second<double> / state.sampleRate)};
    token.length_sample = frames;
    node.midi->data.messages.push_back(libremidi::from_midi1::note_on(0, 60, 110));
    state.samples_since_start += frames;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    node.run(token, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    double missingPeak{};
    for(double v : node.audio->data.channel(0)) missingPeak = std::max(missingPeak, std::abs(v));
    CHECK(missingPeak == 0.);
    node.select(Address{false, 0, 0, 6});
    REQUIRE_FALSE(node.error());
    state.samples_since_start += frames;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    node.run(token, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    double restoredPeak{};
    for(double v : node.audio->data.channel(0)) restoredPeak = std::max(restoredPeak, std::abs(v));
    CHECK(restoredPeak > .0001);
  });
}

TEST_CASE("OPL native voice commands retain hidden operators across model changes", "[emulation][opl][document]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model, VoiceMode).setValue(3);
    control(model, operatorControl(2, Level)).setValue(29);
    control(model, operatorControl(3, Attack)).setValue(11);
    const auto authored = encodeBank(*model.bank());
    CommandDispatcher<>{doc->context().commandStack}.submit(
        new Process::SetControlValue{control(model, VoiceMode), 0});
    control(model, ChipModel).setValue(0);
    CHECK(model.settings().model == Model::Opl2);
    control(model, ChipModel).setValue(1);
    doc->commandStack().undo();
    CHECK(encodeBank(*model.bank()) == authored);
    doc->commandStack().redo();
    CHECK(instrumentControlValue(findProgram(*model.bank(), model.selectedAddress())->instrument,
        operatorControl(2, Level)) == 29);
    control(model, FourOps).setValue(1);
    control(model, VoiceMode).setValue(1);
    REQUIRE(ossia::convert<int>(control(model, VoiceMode).value()) == 1);
    const auto four = encodeBank(*model.bank());
    control(model, ChipModel).setValue(0);
    CHECK(model.settings().model == Model::Opl3);
    CHECK(encodeBank(*model.bank()) == four);
    CHECK_FALSE(model.error().isEmpty());
    control(model, FourOps).setValue(0);
    CHECK(model.settings().fourOpChannels == 1);
    CHECK(encodeBank(*model.bank()) == four);
  });
}

TEST_CASE("OPL native presets recall the whole edited bank without loading provenance", "[emulation][opl][document][preset]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model, operatorControl(0, Level)).setValue(31);
    control(model, InstrumentIndex).setValue(6);
    control(model, operatorControl(1, Attack)).setValue(9);
    control(model, Volume).setValue(.4f);
    const auto expected = encodeBank(*model.bank());
    const auto preset = model.savePreset();
    REQUIRE(model.presetMatches(preset));
    model.loadFile({});
    REQUIRE_FALSE(model.bank());
    const auto resourceError = model.error();
    model.setRuntimeError(QStringLiteral("Transient MIDI failure"));
    model.setRuntimeError({});
    CHECK(model.error() == resourceError);
    model.loadPreset(preset);
    REQUIRE(model.bank());
    CHECK(encodeBank(*model.bank()) == expected);
    CHECK(model.selectedAddress() == Address{false, 0, 0, 6});
    CHECK(ossia::convert<float>(control(model, Volume).value()) == .4f);
    CHECK(model.error().isEmpty());
    control(model, InstrumentIndex).setValue(0);
    CHECK(ossia::convert<int>(control(model, operatorControl(0, Level)).value()) == 31);
  });
}

TEST_CASE("OPL invalid whole-bank and legacy presets are atomic", "[emulation][opl][document][preset]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model, operatorControl(0, Level)).setValue(31);
    const auto original = model.savePreset();
    const auto envelope = QJsonDocument::fromJson(original.data).object();
    const auto replace = [](QJsonArray& values, int c, ossia::value value) {
      JSONReader writer;
      writer.readFrom(value);
      for(int i = 0; i < values.size(); ++i)
      {
        auto entry = values[i].toArray();
        if(entry[0].toInt() != c + 1) continue;
        entry[1] = QJsonDocument::fromJson(writer.toByteArray()).object();
        values[i] = entry;
        return;
      }
      FAIL("Missing preset control");
    };
    int publications{};
    QObject observer;
    QObject::connect(&model, &ProcessModel::bankChanged, &observer, [&] { ++publications; });
    for(int c = 0; c < ControlCount; ++c)
      QObject::connect(&control(model, c), &Process::ControlInlet::valueChanged,
          &observer, [&] { ++publications; });
    for(bool legacy : {false, true})
      for(int invalid : std::array<int, 7>{ChipModel, Chips, FourOps, SoftPan, VoiceMode, RhythmMode, operatorControl(0, Waveform)})
      {
        CAPTURE(legacy, invalid);
        auto values = envelope.value(QStringLiteral("Controls")).toArray();
        replace(values, File, std::string{"missing-preset-bank.wopl"});
        if(invalid == SoftPan) replace(values, invalid, true);
        else if(invalid == VoiceMode || invalid == RhythmMode) replace(values, invalid, 1);
        else if(invalid == operatorControl(0, Waveform))
        {
          replace(values, ChipModel, 0);
          replace(values, invalid, 7);
        }
        else replace(values, invalid, invalid == Chips ? 0 : 99);
        auto candidate = original;
        auto object = envelope;
        object[QStringLiteral("Controls")] = values;
        candidate.data = legacy ? QJsonDocument{values}.toJson(QJsonDocument::Compact)
            : QJsonDocument{object}.toJson(QJsonDocument::Compact);
        model.loadPreset(candidate);
        CHECK_FALSE(model.error().isEmpty());
        CHECK(model.savePreset().data == original.data);
        CHECK(publications == 0);
      }
    // Also validate non-selected bank slots, not only the controls shown in the panel.
    auto incompatibleBank = *model.bank();
    findProgram(incompatibleBank, Address{false, 0, 0, 6})->instrument.operators[0].waveform_E0 = 7;
    const auto bytes = encodeBank(incompatibleBank);
    auto object = envelope;
    object[QStringLiteral("WOPL")] = QString::fromLatin1(QByteArray{
        reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size())}.toBase64());
    auto values = object.value(QStringLiteral("Controls")).toArray();
    replace(values, ChipModel, 0);
    object[QStringLiteral("Controls")] = values;
    auto candidate = original;
    candidate.data = QJsonDocument{object}.toJson(QJsonDocument::Compact);
    model.loadPreset(candidate);
    CHECK_FALSE(model.error().isEmpty());
    CHECK(model.savePreset().data == original.data);
    CHECK(publications == 0);
    // A rejected preset must not poison the accepted configuration used for later edits.
    control(model, ChipModel).setValue(0);
    CHECK(model.settings().model == Model::Opl2);
    control(model, ChipModel).setValue(1);
    CHECK(model.settings().model == Model::Opl3);
    CHECK(model.error().isEmpty());
  });
}

TEST_CASE("OPL native routing exposes secondary feedback in both four-operator modes", "[emulation][opl][document][gui]")
{
  score::test::run_in_gui_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    auto* presenter = score::IDocument::try_presenterDelegate<Scenario::ScenarioDocumentPresenter>(*doc);
    REQUIRE(presenter);
    auto* factory = presenter->context().processList.findDefaultFactory(model);
    REQUIRE(factory);
    QGraphicsScene scene;
    std::unique_ptr<score::ResizeableItem> item{factory->makeItem(model, presenter->context(), nullptr)};
    REQUIRE(item);
    scene.addItem(item.get());
    control(model, VoiceMode).setValue(3);
    score::GraphicsTabLayout* tabs{};
    score::GraphicsTableLayout* table{};
    Dataflow::PortItem* feedback{};
    for(auto* child : scene.items())
    {
      if(auto* tab = dynamic_cast<score::GraphicsTabLayout*>(child)) tabs = tab;
      if(auto* rows = dynamic_cast<score::GraphicsTableLayout*>(child)) table = rows;
      if(auto* port = dynamic_cast<Dataflow::PortItem*>(child);
         port && &port->port() == &control(model, Feedback2)) feedback = port;
    }
    REQUIRE(tabs);
    REQUIRE(feedback);
    REQUIRE(table);
    control(model, VoiceMode).setValue(0);
    control(model, ChipModel).setValue(0);
    REQUIRE(model.settings().model == Model::Opl2);
    control(model, ChipModel).setValue(1);
    REQUIRE(model.settings().model == Model::Opl3);
    tabs->setCurrentIndex(1);
    CHECK_FALSE(feedback->isVisible());
    control(model, FourOps).setValue(1);
    const auto twoOperatorHeight = table->boundingRect().height();
    const auto twoOperatorBounds = item->boundingRect();
    for(int mode : {3, 1})
    {
      control(model, VoiceMode).setValue(mode);
      REQUIRE(ossia::convert<int>(control(model, VoiceMode).value()) == mode);
      CHECK(table->boundingRect().height() > twoOperatorHeight);
      tabs->setCurrentIndex(0);
      const auto rows = table->childItems();
      REQUIRE(rows.size() == 4);
      for(int i = 0; i < rows.size(); ++i)
      {
        CHECK(rows[i]->isVisible());
        for(int j = i + 1; j < rows.size(); ++j)
          CHECK_FALSE(rows[i]->sceneBoundingRect().intersects(rows[j]->sceneBoundingRect()));
      }
      tabs->setCurrentIndex(1);
      CHECK(feedback->isVisible());
      CHECK(feedback->isEnabled());
    }
    CommandDispatcher<>{doc->context().commandStack}.submit(
        new Process::SetControlValue{control(model, Feedback2), 5});
    CHECK(instrumentControlValue(findProgram(*model.bank(), model.selectedAddress())->instrument, Feedback2) == 5);
    control(model, VoiceMode).setValue(0);
    CHECK_FALSE(feedback->isVisible());
    CHECK(table->boundingRect().height() == twoOperatorHeight);
    CHECK(item->boundingRect() == twoOperatorBounds);
    CHECK(instrumentControlValue(findProgram(*model.bank(), model.selectedAddress())->instrument, Feedback2) == 5);
    doc->commandStack().undo();
    CHECK(instrumentControlValue(findProgram(*model.bank(), model.selectedAddress())->instrument, Feedback2) == 0);
    doc->commandStack().redo();
    CHECK(instrumentControlValue(findProgram(*model.bank(), model.selectedAddress())->instrument, Feedback2) == 5);
    tabs->setCurrentIndex(0);
    CommandDispatcher<>{doc->context().commandStack}.submit(
        new Process::SetControlValue{control(model, BankMSB), 127});
    CHECK_FALSE(model.error().isEmpty());
    CHECK(ossia::convert<int>(control(model, InstrumentIndex).value()) < 0);
    CHECK(table->childItems().empty());
    CHECK_FALSE(table->isVisible());
    doc->commandStack().undo();
    CHECK(model.error().isEmpty());
    CHECK(table->isVisible());
    CHECK(table->childItems().size() == 2);
    CHECK(table->boundingRect().height() == twoOperatorHeight);
    CHECK(instrumentControlValue(findProgram(*model.bank(), model.selectedAddress())->instrument, Feedback2) == 5);
  });
}

TEST_CASE("OPL allocation persists, automates, undoes and migrates legacy presets",
          "[emulation][opl][document][allocation]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    CHECK(control(model, VoiceAllocation).value() == ossia::value{1});
    CHECK(control(model, VoiceAllocation).id() == Id<Process::Port>{76});
    CommandDispatcher<>{doc->context().commandStack}.submit(
        new Process::SetControlValue{control(model, VoiceAllocation), 0});
    CHECK(control(model, VoiceAllocation).value() == ossia::value{0});
    doc->commandStack().undo();
    CHECK(control(model, VoiceAllocation).value() == ossia::value{1});
    doc->commandStack().redo();
    CHECK(control(model, VoiceAllocation).value() == ossia::value{0});
    for(int mode : {0, 1, 2})
    {
      control(model, VoiceAllocation).setValue(mode);
      const auto json = toValue(score::marshall<JSONObject>(*base));
      JSONObject::Deserializer reader{json};
      ProcessModel restored{reader, nullptr};
      QByteArray bytes;
      DataStream::Serializer writer{&bytes};
      TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
      writer.read(static_cast<const Process::ProcessModel&>(model));
      writer.read(model);
      DataStream::Deserializer binaryReader{bytes};
      ProcessModel binary{binaryReader, nullptr};
      CHECK(control(restored, VoiceAllocation).value() == ossia::value{mode});
      CHECK(control(binary, VoiceAllocation).value() == ossia::value{mode});
      auto preset = model.savePreset();
      control(restored, VoiceAllocation).setValue((mode + 1) % 3);
      restored.loadPreset(preset);
      CHECK(restored.error().isEmpty());
      CHECK(control(restored, VoiceAllocation).value() == ossia::value{mode});
      ossia::value_port source; source.write_value(mode, 17);
      ossia::value_inlet sink; control(restored, VoiceAllocation).setupExecution(sink, nullptr);
      sink.data.add_port_values(source);
      REQUIRE(sink.data.get_data().size() == 1);
      CHECK(sink.data.get_data().front().value == ossia::value{mode});
      auto object = QJsonDocument::fromJson(preset.data).object();
      auto controls = object.value(QStringLiteral("Controls")).toArray();
      for(qsizetype i = controls.size(); i-- > 0;)
        if(controls[i].toArray()[0].toInt() == VoiceAllocation + 1) controls.removeAt(i);
      object[QStringLiteral("Controls")] = controls;
      preset.data = QJsonDocument{object}.toJson(QJsonDocument::Compact);
      control(restored, VoiceAllocation).setValue(0);
      restored.loadPreset(preset);
      CHECK(restored.error().isEmpty());
      CHECK(control(restored, VoiceAllocation).value() == ossia::value{1});
    }
  });
}

TEST_CASE("OPL allocation automation switches before same-time MIDI without losing old releases",
          "[emulation][opl][execution][allocation]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc));
    drain(plug);
    auto& state = *plug.context().execState;
    const int frames = state.bufferSize, split = frames / 2;
    REQUIRE(frames >= 64);
    for(int mode : {0, 1, 2})
    {
      Executor::Node manual{state}, automated{state};
      for(auto* node : {&manual, &automated})
      {
        auto bank = std::make_shared<const Bank>(factoryBank());
        auto engine = std::make_shared<Engine>(state.sampleRate, Settings{}, *bank);
        node->install(engine, bank, Settings{}, Address{false, 0, 0, 6});
        node->setAllocation(mode == 1 ? 0 : 1);
        auto on = libremidi::from_midi1::note_on(0, 60, 110);
        node->midi->data.messages.push_back(on);
        on.timestamp = split;
        node->midi->data.messages.push_back(on);
        auto off = libremidi::from_midi1::note_off(0, 60, 0);
        off.timestamp = split + frames / 4; // Release belonging to the retired note.
        node->midi->data.messages.push_back(off);
      }
      automated.controls[VoiceAllocation]->data.write_value(mode, split);
      ossia::token_request token;
      token.prev_date = ossia::time_value{0};
      token.date = ossia::time_value{int64_t(frames * ossia::flicks_per_second<double> / state.sampleRate)};
      token.start_sample = 0;
      token.length_sample = frames;
      auto first = token;
      first.length_sample = split;
      first.date = ossia::time_value{int64_t(split * ossia::flicks_per_second<double> / state.sampleRate)};
      auto second = token;
      second.prev_date = first.date;
      second.start_sample = split;
      second.length_sample = frames - split;
      state.samples_since_start += frames;
      ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
      manual.run(first, ossia::exec_state_facade{&state});
      manual.setAllocation(mode);
      manual.run(second, ossia::exec_state_facade{&state});
      automated.run(token, ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
      for(int channel = 0; channel < 2; ++channel)
        for(int i = 0; i < frames; ++i)
          CHECK(manual.audio->data.channel(channel)[i] == automated.audio->data.channel(channel)[i]);
      CHECK(std::any_of(automated.audio->data.channel(0).begin() + split + frames / 4,
          automated.audio->data.channel(0).begin() + frames,
          [](double sample) { return std::abs(sample) > 1e-6; }));
    }
  });
}
