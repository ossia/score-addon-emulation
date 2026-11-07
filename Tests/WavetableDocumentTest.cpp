// SPDX-License-Identifier: GPL-3.0-or-later
#include <Process/ExecutionTransaction.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <Process/Dataflow/PortItem.hpp>
#include <Process/ProcessContext.hpp>
#include <Control/DefaultEffectItem.hpp>
#include <Scenario/Document/ScenarioDocument/ScenarioDocumentPresenter.hpp>
#include <score/graphics/RectItem.hpp>
#include <score/graphics/layouts/GraphicsTabLayout.hpp>
#include <score/graphics/widgets/QGraphicsEnum.hpp>
#include <Process/ProcessList.hpp>
#include <score/document/DocumentInterface.hpp>
#include <Dataflow/CurveInlet.hpp>
#include <Dataflow/WaveformInlet.hpp>
#include <Curve/CurveModel.hpp>
#include <Curve/CurveView.hpp>
#include <Curve/ApplicationPlugin.hpp>
#include <Curve/Segment/Power/PowerSegment.hpp>
#include <Emulation/Wavetable/Controls.hpp>
#include <Emulation/Volume.hpp>
#include <Emulation/WaveformInlet.hpp>
#include <Process/Commands/SetControlValue.hpp>
#include <Emulation/Wavetable/Executor/Node.hpp>
#include <Emulation/Wavetable/ProcessModel.hpp>
#include <Execution/BaseScenarioComponent.hpp>
#include <Execution/DocumentPlugin.hpp>
#include <Scenario/Document/Interval/IntervalExecution.hpp>
#include <Scenario/Commands/LoadPresetCommand.hpp>
#include <score/command/Dispatchers/CommandDispatcher.hpp>
#include <score/selection/SelectionStack.hpp>
#include <core/command/CommandStack.hpp>
#include <score/model/EntitySerialization.hpp>
#include <score/serialization/VisitorCommon.hpp>
#include <ossia/dataflow/execution_state.hpp>
#include <ossia/dataflow/data_copy.hpp>
#include <ossia/network/generic/generic_device.hpp>
#include <ossia/network/generic/generic_node.hpp>
#include <ossia/network/generic/generic_parameter.hpp>
#include <libremidi/ump_events.hpp>
#include <catch2/catch_test_macros.hpp>
#include <score_test/App.hpp>
#include <score_test/Events.hpp>
#include <score_test/Mouse.hpp>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QApplication>
#include <QKeyEvent>
#include <score_test/Project.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <limits>
#include <stdexcept>
#include <vector>
namespace
{
using namespace Emulation::Wavetable;
Process::ControlInlet& control(ProcessModel& p, int index)
{ return *static_cast<Process::ControlInlet*>(p.inlets()[index + 1]); }
constexpr auto uuid = "989bc228-c2e7-40a8-a56e-4700b946d864";
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
Process::WavetableData drawnWave()
{
  Process::WavetableData data{32, std::vector<float>(32)};
  for(int i = 0; i < 32; ++i)
    data.samples[i] = float(((i * 13 + 5) % 31) / 15. - 1.);
  return data;
}
std::array<int, 32> drawnSamples(bool scc)
{
  std::array<int, 32> samples;
  REQUIRE(Process::prepareWaveform(drawnWave(), samples, scc ? -128. : 0., scc ? 127. : 31.));
  return samples;
}
Dataflow::WaveformInlet& waveformControl(ProcessModel& model, int c)
{
  auto* inlet = dynamic_cast<Dataflow::WaveformInlet*>(&control(model, c));
  REQUIRE(inlet);
  return *inlet;
}
void checkDrawnWave(ProcessModel& model, int c)
{
  const auto& inlet = waveformControl(model, c);
  CHECK(inlet.wavetable() == drawnWave());
  CHECK(inlet.frameSizeConstraint() == 32);
  CHECK(inlet.wavetable().frameCount() == 1);
  for(auto chip : {Model::SccPlus, Model::HuC6280}) {
    if(chip != Model::HuC6280 && (c - ChannelBase) / ChannelStride == 5) continue;
    auto settings = model.settings(); settings.model = chip;
    Engine engine{48000, settings};
    for(int i = 0; i < 32; ++i)
      CHECK(engine.nativeWave((c - ChannelBase) / ChannelStride, i) == drawnSamples(chip != Model::HuC6280)[i]);
  }
}
}

TEST_CASE("Wavetable shared authored channels retain unavailable work through model changes", "[emulation][wavetable][document]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    CommandDispatcher<> dispatcher{doc->context().commandStack};
    dispatcher.submit<Process::SetControlValue>(waveformControl(model, channelControl(5, Wave)), Process::wavetableToValue(drawnWave()));
    const auto authored = control(model, channelControl(5, Wave)).value();
    control(model, channelControl(5, Level)).setValue(.375f);
    control(model, channelControl(5, ChannelTuning)).setValue(123);
    for(auto chip : {Model::HuC6280, Model::Scc, Model::SccPlus, Model::HuC6280}) {
      dispatcher.submit<Process::SetControlValue>(control(model, ChipModel), int(chip));
      CHECK(control(model, channelControl(5, Wave)).value() == authored);
      CHECK(model.settings().channels[5].level == .375);
      CHECK(model.settings().channels[5].tuning == 123);
      CHECK(control(model, channelControl(0, Wave)).value() == Emulation::Waveform::triangle());
      for(int ch = 0; ch < 6; ++ch)
        CHECK(waveformControl(model, channelControl(ch, Wave)).amplitudeLevels() == (chip == Model::HuC6280 ? 32 : 256));
    }
    auto json = toValue(score::marshall<JSONObject>(*base));
    JSONObject::Deserializer reader{json}; ProcessModel restored{reader, nullptr};
    CHECK(restored.settings() == model.settings());
    CHECK(control(restored, channelControl(5, Wave)).value() == authored);
    QByteArray binary; DataStream::Serializer writer{&binary};
    TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
    writer.read(static_cast<const Process::ProcessModel&>(model)); writer.read(model);
    DataStream::Deserializer binaryReader{binary}; ProcessModel binaryRestored{binaryReader, nullptr};
    CHECK(binaryRestored.settings() == model.settings());
    json["Model"].SetString("SCC"); JSONObject::Deserializer mismatch{json};
    CHECK_THROWS_AS((ProcessModel{mismatch, nullptr}), std::runtime_error);
  });
}
TEST_CASE("Wavetable native preset recall and undo publish one atomic complete state", "[emulation][wavetable][document][preset]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base); const auto original = model.settings();
    CommandDispatcher<> dispatcher{doc->context().commandStack};
    for(auto chip : {Model::SccPlus, Model::HuC6280}) {
      ProcessModel source{TimeVal::fromMsecs(1000), {}, Id<Process::ProcessModel>{999}, nullptr};
      control(source, ChipModel).setValue(int(chip)); control(source, Volume).setValue(.5f);
      waveformControl(source, channelControl(3, Wave)).setWaveform(drawnWave());
      const auto desired = source.settings();
      int notifications{}; QObject::connect(&model, &ProcessModel::settingsChanged, &source, [&] { ++notifications; });
      dispatcher.submit<Scenario::Command::LoadPreset>(model, source.savePreset()); CHECK(notifications == 1);
      CHECK(model.settings() == desired);
      doc->commandStack().undo(); CHECK(notifications == 2); CHECK(model.settings() == original);
      auto invalid = source.savePreset();
      auto invalidJson = readJson(invalid.data);
      for(auto& entry : invalidJson["Controls"].GetArray())
        if(entry[0].GetInt() == portId(channelControl(0, Wave)))
          entry[1]["Tuple"][0]["Tuple"][0]["Float"].SetFloat(99.f);
      JSONReader invalidWriter; invalidJson.Accept(invalidWriter.stream); invalid.data = invalidWriter.toByteArray();
      model.loadPreset(invalid); CHECK_FALSE(model.error().isEmpty()); CHECK(notifications == 2);
      CHECK(model.settings() == original);
      model.loadPreset(model.savePreset()); CHECK(model.error().isEmpty());
    }
  });
}

TEST_CASE("Wavetable Volume migrates old domains without rescaling native gain", "[emulation][wavetable][document][preset]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    for(float gain : {0.f, .375f, 1.f, 4.5f, Emulation::MaxVolume}) {
      control(model, Volume).setValue(gain);
      control(model, Volume).setInit(.625f);
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
        CHECK(control(*copy, channelControl(0, Level)).domain().get().convert_max<float>() == 1.f);
        // Native Automation emits its stored numeric range on a domainless outlet.
        ossia::value_port source; source.write_value(gain, 7);
        ossia::value_inlet sink; control(*copy, Volume).setupExecution(sink, nullptr);
        sink.data.add_port_values(source);
        REQUIRE(sink.data.get_data().size() == 1);
        CHECK(sink.data.get_data().front().value == ossia::value{gain});
        CHECK(sink.data.get_data().front().timestamp == 7);
      }
      const auto preset = model.savePreset();
      control(restored, Volume).setValue(gain == 0.f ? 1.f : 0.f);
      restored.loadPreset(preset); CHECK(restored.settings() == expected);
      CHECK(restored.error().isEmpty());
      control(binary, Volume).setValue(Emulation::MaxVolume + 1.f);
      restored.loadPreset(binary.savePreset());
      CHECK_FALSE(restored.error().isEmpty()); CHECK(restored.settings() == expected);
    }
  });
}

TEST_CASE("Wavetable timestamped volume preserves split-token clocks and duplicate-window output", "[emulation][wavetable][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    REQUIRE(state.bufferSize >= 64);
    for(auto model : {Model::Scc, Model::SccPlus, Model::HuC6280}) {
      Settings settings; settings.model = model; if(model == Model::HuC6280) settings.clock = 3579545;
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
TEST_CASE("Wavetable paused windows clean up sustained voices without replaying note-ons", "[emulation][wavetable][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    for(auto model : {Model::Scc, Model::HuC6280}) {
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

TEST_CASE("Wavetable restores stereo output after graph buffer recycling", "[emulation][wavetable][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    for(auto model : {Model::Scc, Model::SccPlus, Model::HuC6280}) {
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

TEST_CASE("Wavetable accepted group-zero MIDI clears a rejected UMP error", "[emulation][wavetable][execution]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    Settings settings; Executor::Node node{state, settings};
    auto engine = std::make_shared<Engine>(state.sampleRate, settings);
    node.install(engine, settings.model, settings.clock);
    auto token = tokenFor(state);
    auto invalid = libremidi::from_midi1::note_on(0, 69, 127);
    invalid.data[0] |= uint32_t{1} << 24; // UMP group 1, not this process's MIDI port.
    node.midi->data.messages.push_back(invalid);
    state.samples_since_start += state.bufferSize;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    node.run(token, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    REQUIRE(node.error() != nullptr);
    node.midi->data.messages.clear();
    node.midi->data.messages.push_back(libremidi::from_midi1::note_on(0, 69, 127));
    state.samples_since_start += state.bufferSize;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    node.run(token, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    CHECK(node.error() == nullptr);
    CHECK(std::any_of(node.audio->data.channel(0).begin(), node.audio->data.channel(0).end(),
        [](double value) { return value != 0.; }));
  });
}

TEST_CASE("Wavetable sample edits preserve native undo through presets and documents", "[emulation][wavetable][document][samples]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    CommandDispatcher<> dispatcher{doc->context().commandStack};
    for(int channel : {0, 3, 4, 5}) {
      const int c = channelControl(channel, Wave);
      auto& inlet = waveformControl(model, c);
      const auto originalValue = inlet.value();
      dispatcher.submit<Process::SetControlValue>(inlet, Process::wavetableToValue(drawnWave()));
      checkDrawnWave(model, c);
      const auto authored = inlet.value();
      doc->commandStack().undo();
      CHECK(inlet.value() == originalValue);
      doc->commandStack().redo();
      checkDrawnWave(model, c);
      CHECK(inlet.value() == authored);

      auto json = toValue(score::marshall<JSONObject>(*base));
      JSONObject::Deserializer reader{json}; ProcessModel restored{reader, nullptr};
      checkDrawnWave(restored, c);
      CHECK(control(restored, c).value() == authored);
      QByteArray binary; DataStream::Serializer writer{&binary};
      TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
      writer.read(static_cast<const Process::ProcessModel&>(model)); writer.read(model);
      DataStream::Deserializer binaryReader{binary}; ProcessModel binaryRestored{binaryReader, nullptr};
      checkDrawnWave(binaryRestored, c);

      ProcessModel source{TimeVal::fromMsecs(1000), {}, Id<Process::ProcessModel>{999}, nullptr};
      source.loadPreset(model.savePreset());
      checkDrawnWave(source, c);
      CHECK(control(source, c).value() == authored);
      const auto drawnPreset = source.savePreset();
      control(source, c).setValue(Emulation::Waveform::triangle());
      const auto changed = source.settings();
      dispatcher.submit<Scenario::Command::LoadPreset>(model, source.savePreset());
      CHECK(portValue(model.settings(), c) == portValue(changed, c));
      doc->commandStack().undo();
      checkDrawnWave(model, c);
      CHECK(inlet.value() == authored);
      model.loadPreset(drawnPreset);
      checkDrawnWave(model, c);
    }
  });
}

TEST_CASE("Wavetable wave inlets drive native RAM from execution values", "[emulation][wavetable][execution][samples]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    CommandDispatcher<> dispatcher{doc->context().commandStack};
    dispatcher.submit<Process::SetControlValue>(waveformControl(model, channelControl(0, Wave)), Process::wavetableToValue(drawnWave()));
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    // What a multislider sends: one cycle of numbers, here eight in [0, 1].
    const std::vector<ossia::value> slider{0.f, 1.f, .25f, .75f, .5f, 1.f, 0.f, .125f};
    std::array<double, 32> received;
    REQUIRE(Process::prepareWaveform(ossia::value{slider}, received, 0., 1.));
    for(auto chip : {Model::Scc, Model::SccPlus, Model::HuC6280}) {
      CAPTURE(int(chip));
      Settings defaults; defaults.model = chip;
      Settings expected = defaults;
      expected.channels[0].wave = model.settings().channels[0].wave;
      const int c = channelControl(0, Wave);
      Executor::Node node{state, defaults};
      auto engine = std::make_shared<Engine>(state.sampleRate, defaults);
      auto* native = engine.get();
      node.install(engine, chip, defaults.clock);
      node.modelChanged(expected);
      CHECK(node.controls[c]->data.is_event);
      node.midi->data.messages.push_back(libremidi::from_midi1::note_on(0, 69, 127));
      Engine reference{double(state.sampleRate), defaults};
      REQUIRE(reference.configure(expected));
      reference.midi(std::array<uint8_t, 3>{0x90, 69, 127});
      std::vector<double> left(state.bufferSize), right(state.bufferSize);
      auto token = tokenFor(state);
      double peak{};
      uint32_t seen{};
      std::array<float, 32> published;
      CHECK_FALSE(node.receivedWave(0).take(seen, published));
      for(int tick = 0; tick < 8; ++tick) {
        if(tick == 4) {
          node.controls[c]->data.write_value(ossia::value{slider}, 0);
          expected.channels[0].wave = received;
          REQUIRE(reference.configure(expected));
        }
        state.samples_since_start += state.bufferSize;
        ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
        node.run(token, ossia::exec_state_facade{&state});
        reference.render(left.data(), right.data(), left.size());
        ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
        CHECK(node.error() == nullptr);
        for(int i = 0; i < 32; ++i) CHECK(native->nativeWave(0, i) == reference.nativeWave(0, i));
        CHECK(std::equal(left.begin(), left.end(), node.audio->data.channel(0).begin(), node.audio->data.channel(0).end()));
        CHECK(std::equal(right.begin(), right.end(), node.audio->data.channel(1).begin(), node.audio->data.channel(1).end()));
        for(double sample : left) peak = std::max(peak, std::abs(sample));
        node.controls[c]->data.clear();
        node.midi->data.messages.clear();
      }
      CHECK(peak > .001);
      REQUIRE(node.receivedWave(0).take(seen, published));
      for(int i = 0; i < 32; ++i) CHECK(published[i] == float(received[i]));
      CHECK_FALSE(node.receivedWave(0).take(seen, published));

      // A document snapshot that leaves the wave as it was keeps what execution
      // delivered; one that changes it wins.
      auto snapshot = expected;
      snapshot.channels[0].wave = model.settings().channels[0].wave;
      snapshot.volume = .5;
      node.modelChanged(snapshot);
      for(int i = 0; i < 32; ++i) CHECK(native->nativeWave(0, i) == reference.nativeWave(0, i));
      snapshot.channels[0].wave.fill(1.);
      node.modelChanged(snapshot);
      REQUIRE(reference.configure(snapshot));
      for(int i = 0; i < 32; ++i) CHECK(native->nativeWave(0, i) == reference.nativeWave(0, i));

      // Values that are not a cycle are reported and change nothing.
      node.controls[c]->data.write_value(ossia::value{std::string{"saw"}}, 0);
      state.samples_since_start += state.bufferSize;
      ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
      node.run(token, ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
      CHECK(node.error() != nullptr);
      for(int i = 0; i < 32; ++i) CHECK(native->nativeWave(0, i) == reference.nativeWave(0, i));
    }
  });
}

TEST_CASE("Wavetable wave ports are registered for execution and report what they receive", "[emulation][wavetable][execution][samples]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    auto& wave = waveformControl(model, channelControl(1, Wave));
    State::AddressAccessor address;
    address.address.device = QStringLiteral("osc");
    address.address.path = QStringList{QStringLiteral("wave")};
    wave.setAddress(address);
    CHECK(wave.address() == address);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    const auto& processes = plug.baseScenario()->baseInterval().processes();
    const auto found = processes.find(model.id()); REQUIRE(found != processes.end());
    const auto node = std::static_pointer_cast<Executor::Node>(found->second->node);
    REQUIRE(node);
    const auto registered = plug.context().setup.inlets.find(&wave);
    REQUIRE(registered != plug.context().setup.inlets.end());
    CHECK(registered->second.first == node);
    CHECK(registered->second.second == node->controls[channelControl(1, Wave)]);

    std::vector<ossia::value> executionValues;
    QObject::connect(&wave, &Process::ControlInlet::executionValueChanged, &wave,
        [&](const ossia::value& v) { executionValues.push_back(v); });
    const auto before = wave.value();
    const auto commands = doc->commandStack().currentIndex();
    node->controls[channelControl(1, Wave)]->data.write_value(
        ossia::value{std::vector<ossia::value>{1.f, -1.f}}, 0);
    auto& state = *plug.context().execState;
    state.samples_since_start += state.bufferSize;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    node->run(tokenFor(state), ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    CHECK(score::test::wait_until([&] { return !executionValues.empty(); }, 5000));
    REQUIRE(executionValues.size() == 1);
    std::array<double, 32> expected;
    REQUIRE(Process::prepareWaveform(
        ossia::value{std::vector<ossia::value>{1.f, -1.f}}, expected, 0., 1.));
    CHECK(executionValues.front() == Emulation::Waveform::fromSamples(expected, 0., 1.));
    // Execution never edits the document.
    CHECK(wave.value() == before);
    CHECK(doc->commandStack().currentIndex() == commands);
  });
}

TEST_CASE("Wavetable rejects obsolete document and preset schemas atomically", "[emulation][wavetable][document][preset]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    const auto original = model.settings();
    for(int schema : {1, 2, 3, 5}) {
      auto json = toValue(score::marshall<JSONObject>(*base));
      json["Schema"].SetInt(schema);
      JSONObject::Deserializer reader{json};
      CHECK_THROWS_AS((ProcessModel{reader, nullptr}), std::runtime_error);
      auto preset = model.savePreset();
      auto data = readJson(preset.data); data["Schema"].SetInt(schema);
      JSONReader writer; data.Accept(writer.stream); preset.data = writer.toByteArray();
      model.loadPreset(preset);
      CHECK_FALSE(model.error().isEmpty()); CHECK(model.settings() == original);
    }
    model.loadPreset(model.savePreset()); CHECK(model.error().isEmpty());
  });
}

namespace {
QGraphicsItem& onlyWaveformEditor(const QGraphicsScene& scene)
{
  QGraphicsItem* result{};
  int count{};
  for(auto* item : scene.items())
    if(item->data(0).toString() == QStringLiteral("WaveformEditor")) { result = item; ++count; }
  REQUIRE(count == 1);
  return *result;
}
void sampleMouse(QGraphicsView& window, QGraphicsItem& editor, QEvent::Type type, QPointF point)
{
  const auto rect = Dataflow::waveformItemState(editor).plot;
  const auto position = window.mapFromScene(editor.mapToScene(
      {rect.left() + point.x() * rect.width(), rect.top() + (1. - point.y()) * rect.height()}));
  score::test::mouseEvent(*window.viewport(), type, position,
      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
  score::test::settle();
}
struct SampleEdit { ossia::value before, after; };
SampleEdit dragSamples(score::Document& doc, QGraphicsView& window,
    QGraphicsItem& editor, Dataflow::WaveformInlet& inlet)
{
  SampleEdit edit{inlet.value(), {}};
  const auto commands = doc.commandStack().size();
  sampleMouse(window, editor, QEvent::MouseButtonPress, {.12, .85});
  CHECK(window.scene()->mouseGrabberItem() == &editor);
  sampleMouse(window, editor, QEvent::MouseMove, {.42, .15});
  CHECK(inlet.value() == edit.before);
  CHECK(window.scene()->mouseGrabberItem() == &editor);
  sampleMouse(window, editor, QEvent::MouseMove, {.86, .8});
  CHECK(inlet.value() == edit.before);
  CHECK(window.scene()->mouseGrabberItem() == &editor);
  sampleMouse(window, editor, QEvent::MouseButtonRelease, {.86, .8});
  edit.after = inlet.value();
  CHECK(edit.after != edit.before);
  CHECK(window.scene()->mouseGrabberItem() == nullptr);
  CHECK(doc.commandStack().size() == commands + 1);
  CHECK(inlet.wavetable().valid());
  for(int i = 4; i <= 26; ++i) {
    const auto amplitude = inlet.wavetable().samples[i];
    const auto step = (amplitude + 1.) * .5 * (inlet.amplitudeLevels() - 1);
    CHECK(std::abs(step - std::round(step)) < 1e-4);
  }
  return edit;
}
}

TEST_CASE("Wavetable graphics and inspector editors share native sample undo paths", "[emulation][wavetable][document][samples][gui]") {
  score::test::run_in_gui_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    auto& first = waveformControl(model, channelControl(0, Wave));
    auto& second = waveformControl(model, channelControl(1, Wave));
    const auto initial = first.value();
    QGraphicsScene scene;
    auto* editor = WidgetFactory::WaveformInletItems::make_item(first, first, doc->context(), nullptr, &model);
    scene.addItem(editor); Dataflow::setWaveformItemRect(*editor, {0., 0., 400., 160.});
    QGraphicsView window{&scene}; window.setSceneRect(-20., -20., 440., 200.); window.resize(480, 240);
    REQUIRE(score::test::showAndActivate(window));
    const auto firstEdit = dragSamples(*doc, window, *editor, first);
    CHECK(second.value() == initial);
    Dataflow::setWaveformItemModel(*editor, second);
    const auto secondEdit = dragSamples(*doc, window, *editor, second);
    CHECK(first.value() == firstEdit.after);
    Dataflow::setWaveformItemModel(*editor, first);
    doc->commandStack().undo(); CHECK(second.value() == initial); CHECK(first.value() == firstEdit.after);
    doc->commandStack().undo(); CHECK(first.value() == initial);
    doc->commandStack().redo(); doc->commandStack().redo();
    CHECK(first.value() == firstEdit.after); CHECK(second.value() == secondEdit.after);
    // A rebind during a pending stroke rolls it back before changing its target.
    sampleMouse(window, *editor, QEvent::MouseButtonPress, {.2, .1});
    CHECK(first.value() == firstEdit.after);
    Dataflow::setWaveformItemModel(*editor, second);
    sampleMouse(window, *editor, QEvent::MouseButtonRelease, {.2, .1});
    CHECK(first.value() == firstEdit.after); CHECK(second.value() == secondEdit.after);
    std::unique_ptr<QWidget> inspector{WidgetFactory::WaveformInletItems::make_widget(first, doc->context(), nullptr, &model)};
    auto* inspectorView = inspector->findChild<QGraphicsView*>(); REQUIRE(inspectorView); REQUIRE(inspectorView->scene());
    inspector->resize(460, 210); REQUIRE(score::test::showAndActivate(*inspector));
    auto& inspectorEditor = onlyWaveformEditor(*inspectorView->scene());
    const Selection inspected{&first}; doc->context().selectionStack.pushNewSelection(inspected);
    const auto commands = doc->commandStack().size();
    sampleMouse(*inspectorView, inspectorEditor, QEvent::MouseButtonPress, {.65, .1});
    sampleMouse(*inspectorView, inspectorEditor, QEvent::MouseMove, {.9, .2});
    CHECK(first.value() == firstEdit.after);
    QKeyEvent escape{QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier};
    QApplication::sendEvent(inspectorView->viewport(), &escape);
    sampleMouse(*inspectorView, inspectorEditor, QEvent::MouseButtonRelease, {.9, .2});
    CHECK(first.value() == firstEdit.after); CHECK(doc->commandStack().size() == commands);
    sampleMouse(*inspectorView, inspectorEditor, QEvent::MouseButtonPress, {.65, .1});
    sampleMouse(*inspectorView, inspectorEditor, QEvent::MouseButtonRelease, {.65, .1});
    const auto inspectorEdit = first.value();
    CHECK(inspectorEdit != firstEdit.after); CHECK(second.value() == secondEdit.after);
    CHECK(doc->context().selectionStack.currentSelection() == inspected);
    doc->commandStack().undo(); CHECK(first.value() == firstEdit.after);
    doc->commandStack().redo(); CHECK(first.value() == inspectorEdit);
    ProcessModel restored{TimeVal::fromMsecs(1000), {}, Id<Process::ProcessModel>{999}, nullptr};
    restored.loadPreset(model.savePreset());
    CHECK(control(restored, channelControl(0, Wave)).value() == inspectorEdit);
    CHECK(control(restored, channelControl(1, Wave)).value() == secondEdit.after);
  });
}

TEST_CASE("Chip waveform ports reject invalid samples without splitting value and typed state", "[emulation][wavetable][document][samples]") {
  score::test::run_in_app([](const score::GUIApplicationContext&) {
    std::unique_ptr<Dataflow::WaveformInlet> inlet{Emulation::makeWaveformInlet(
        Emulation::Waveform::triangle(), 256, "Wave", Id<Process::Port>{0}, nullptr)};
    inlet->setWaveform(drawnWave());
    const auto original = inlet->value();
    for(float invalid : {2.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
      auto data = drawnWave(); data.samples[13] = invalid;
      inlet->setWaveform(data);
      CHECK(inlet->value() == original); CHECK(inlet->wavetable() == drawnWave());
      auto value = original;
      (*(*value.target<std::vector<ossia::value>>())[0].target<std::vector<ossia::value>>())[13] = invalid;
      inlet->setValue(value);
      CHECK(inlet->value() == original); CHECK(inlet->wavetable() == drawnWave());
    }
    inlet->setValue(std::vector<ossia::value>{0.f, 1.f});
    CHECK(inlet->value() == original);
    inlet->setWaveform(Process::makeTriangleWavetable(64));
    CHECK(inlet->value() == original);
    auto bank = drawnWave(); bank.samples.insert(bank.samples.end(), 32, 0.f);
    inlet->setWaveform(bank);
    CHECK(inlet->value() == original); CHECK(inlet->wavetable() == drawnWave());
  });
}

TEST_CASE("Wavetable musical level keeps domainless cable and address meaning after preset recall", "[emulation][wavetable][execution][migration]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    for(double maximum : {15., 31.}) for(bool address : {false, true}) {
      auto& inlet = control(model, channelControl(0, Level));
      inlet.setDomain(ossia::make_domain(0.f, float(maximum)));
      ProcessModel sound{TimeVal::fromMsecs(1000), {}, Id<Process::ProcessModel>{999}, nullptr};
      control(sound, channelControl(0, Level)).setValue(.25f);
      model.loadPreset(sound.savePreset());
      CHECK(model.levelMaxima()[0] == maximum);
      CHECK(model.settings().channels[0].level == .25);
      CHECK(ossia::convert<double>(inlet.value()) == maximum * .25);
      const auto settings = model.settings();
      Executor::Node node{state, settings, model.levelMaxima()};
      auto engine = std::make_shared<Engine>(state.sampleRate, settings); node.install(engine, settings.model, settings.clock);
      const int c = channelControl(0, Level), q = state.bufferSize / 3;
      inlet.setupExecution(*node.controls[c], &model);
      if(address) {
        ossia::net::generic_device device{"legacy"};
        auto* parameter = device.create_child("level")->create_parameter(ossia::val_type::FLOAT);
        node.controls[c]->data.add_global_value(*parameter, float(maximum * .5));
      } else {
        ossia::value_port source;
        source.write_value(float(maximum * .5), 0); source.write_value(0.f, q); source.write_value(float(maximum), 2 * q);
        node.controls[c]->data.add_port_values(source);
      }
      node.midi->data.messages.push_back(libremidi::from_midi1::note_on(0, 69, 127));
      Engine reference{double(state.sampleRate), settings};
      REQUIRE(reference.setScalarControl(c, .5)); reference.midi(std::array<uint8_t, 3>{0x90, 69, 127});
      std::vector<double> left(state.bufferSize), right(state.bufferSize);
      if(address) reference.render(left.data(), right.data(), state.bufferSize);
      else {
        reference.render(left.data(), right.data(), q); REQUIRE(reference.setScalarControl(c, 0.));
        reference.render(left.data() + q, right.data() + q, q); REQUIRE(reference.setScalarControl(c, 1.));
        reference.render(left.data() + 2 * q, right.data() + 2 * q, state.bufferSize - 2 * q);
      }
      state.samples_since_start += state.bufferSize;
      ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
      node.run(tokenFor(state), ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
      CHECK(node.error() == nullptr);
      CHECK(std::equal(left.begin(), left.end(), node.audio->data.channel(0).begin(), node.audio->data.channel(0).end()));
      CHECK(std::equal(right.begin(), right.end(), node.audio->data.channel(1).begin(), node.audio->data.channel(1).end()));
    }
  });
}

TEST_CASE("Wavetable panel does not grow during ordinary edits or model round trips", "[emulation][wavetable][document][gui]") {
  score::test::run_in_gui_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    auto* presenter = score::IDocument::try_presenterDelegate<Scenario::ScenarioDocumentPresenter>(*doc); REQUIRE(presenter);
    auto* factory = presenter->context().processList.findDefaultFactory(model); REQUIRE(factory);
    QGraphicsScene scene;
    std::unique_ptr<score::ResizeableItem> item{factory->makeItem(model, presenter->context(), nullptr)};
    REQUIRE(item); scene.addItem(item.get());
    const auto bounds = item->boundingRect();
    for(int i = 0; i < 20; ++i) {
      control(model, Volume).setValue(i % 2 ? .8f : .6f);
      CHECK(item->boundingRect() == bounds);
      control(model, ChipModel).setValue(2);
      control(model, ChipModel).setValue(0);
      CHECK(item->boundingRect() == bounds);
    }
  });
}

TEST_CASE("Wavetable channel tabs keep one sample editor and stable resource identities", "[emulation][wavetable][document][gui]") {
  score::test::run_in_gui_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    auto* presenter = score::IDocument::try_presenterDelegate<Scenario::ScenarioDocumentPresenter>(*doc); REQUIRE(presenter);
    auto* factory = presenter->context().processList.findDefaultFactory(model); REQUIRE(factory);
    QGraphicsScene scene;
    std::unique_ptr<score::ResizeableItem> item{factory->makeItem(model, presenter->context(), nullptr)};
    REQUIRE(item); scene.addItem(item.get());
    auto* editor = &onlyWaveformEditor(scene);
    QGraphicsView window{&scene};
    window.setSceneRect(-20., -20., 1100., 650.); window.resize(1140, 700);
    REQUIRE(score::test::showAndActivate(window));
    std::array<Dataflow::PortItem*, 6> endpoints{};
    for(auto* child : scene.items())
      if(auto* port = dynamic_cast<Dataflow::PortItem*>(child))
        for(int ch = 0; ch < 6; ++ch)
          if(&port->port() == &control(model, channelControl(ch, Wave))) endpoints[ch] = port;
    for(auto* endpoint : endpoints) REQUIRE(endpoint);
    const auto select = [&](int channel, int channelCount) {
      score::GraphicsTabLayout* tabs{};
      for(auto* child : scene.items())
        if(auto* tab = dynamic_cast<score::GraphicsTabLayout*>(child)) tabs = tab;
      REQUIRE(tabs);
      score::QGraphicsEnum* bar{};
      for(auto* child : tabs->childItems())
        if(auto* candidate = dynamic_cast<score::QGraphicsEnum*>(child)) bar = candidate;
      REQUIRE(bar);
      CHECK(bar->columns == channelCount);
      const auto sound = model.savePreset().data;
      bar->currentIndexChanged(channel);
      CHECK(tabs->currentIndex() == channel);
      CHECK(model.savePreset().data == sound);
      CHECK(&onlyWaveformEditor(scene) == editor);
    };
    const auto checkEndpoint = [&](int channel) {
      auto& inlet = control(model, channelControl(channel, Wave));
      auto* portFactory = ctx.interfaces<Process::PortFactoryList>().get(inlet.concreteKey());
      REQUIRE(portFactory);
      CHECK(endpoints[channel]->isVisible());
      CHECK(endpoints[channel]->acceptDrops());
      CHECK(&endpoints[channel]->port() == &inlet);
      CHECK(endpoints[channel]->parentItem() == editor->parentItem());
      CHECK(endpoints[channel]->pos() == portFactory->defaultLayout().port);
      CHECK(editor->pos() == portFactory->defaultLayout().control);
      for(int ch = 0; ch < 6; ++ch)
        if(ch != channel) CHECK_FALSE(endpoints[ch]->isVisible());
    };
    select(3, 5); checkEndpoint(3);
    const auto sharedBounds = item->boundingRect();
    select(4, 5); checkEndpoint(3);
    CHECK(item->boundingRect() == sharedBounds);
    auto& shared = control(model, channelControl(3, Wave));
    const auto original = shared.value();
    score::test::settle();
    sampleMouse(window, *editor, QEvent::MouseButtonPress, {.375, .8});
    sampleMouse(window, *editor, QEvent::MouseButtonRelease, {.375, .8});
    const auto edited = shared.value();
    CHECK(edited != original);
    select(0, 5); checkEndpoint(0);
    doc->commandStack().undo(); CHECK(shared.value() == original);
    doc->commandStack().redo(); CHECK(shared.value() == edited);
    control(model, ChipModel).setValue(2);
    select(5, 6); checkEndpoint(5);
    const auto waveBounds = item->boundingRect();
    control(model, channelControl(5, Noise)).setValue(true);
    CHECK_FALSE(editor->isVisible());
    for(auto* endpoint : endpoints) CHECK_FALSE(endpoint->isVisible());
    CHECK(item->boundingRect().height() < waveBounds.height());
    control(model, channelControl(5, Noise)).setValue(false);
    CHECK(item->boundingRect() == waveBounds);
    checkEndpoint(5);
    control(model, LfoDepth).setValue(1);
    select(1, 6); checkEndpoint(1);
    control(model, LfoDepth).setValue(0);
    select(5, 6);
    control(model, ChipModel).setValue(0);
    select(4, 5); checkEndpoint(3);
    CHECK(item->boundingRect() == sharedBounds);
  });
}

namespace
{
struct NativeCurveEdit
{
  ossia::value before, after;
};

NativeCurveEdit dragNativeCurve(
    score::Document& doc, QGraphicsView& window, Curve::View& view,
    Dataflow::CurveInlet& inlet, int gesture)
{
  auto& settings = doc.context().app.guiApplicationPlugin<Curve::ApplicationPlugin>().editionSettings();
  struct RestoreTool {
    Curve::EditionSettings& settings;
    Curve::Tool previous;
    ~RestoreTool() { settings.setTool(previous); }
  } restore{settings, settings.tool()};
  inlet.setValue(std::vector<ossia::value>{
      std::vector<ossia::value>{ossia::vec2f{0.f, .3f}, ossia::vec2f{.25f, .4f}, 1.f},
      std::vector<ossia::value>{ossia::vec2f{.25f, .4f}, ossia::vec2f{.5f, .6f}, 1.f},
      std::vector<ossia::value>{ossia::vec2f{.5f, .6f}, ossia::vec2f{.75f, .35f}, 1.f},
      std::vector<ossia::value>{ossia::vec2f{.75f, .35f}, ossia::vec2f{1.f, .3f}, 1.f}});
  settings.setTool(Curve::Tool::Select);
  // The stock convention is plain drag for points / segment translation,
  // Shift-drag for curvature, not a different editor or custom drag handler.
  if(gesture == 2) {
    QKeyEvent shift{QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier};
    QApplication::sendEvent(&window, &shift);
    CHECK(settings.tool() == Curve::Tool::SetSegment);
  }
  score::test::settle();
  auto& scene = *window.scene();
  const auto rect = view.boundingRect();
  const auto mouse = [&](QEvent::Type type, QPointF point) {
    const auto pos = window.mapFromScene(view.mapToScene(
        {point.x() * rect.width(), (1. - point.y()) * rect.height()}));
    score::test::mouseEvent(*window.viewport(), type, pos,
        type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
        gesture == 2 ? Qt::ShiftModifier : Qt::NoModifier);
    score::test::settle();
  };
  const QPointF from = gesture == 0 ? QPointF{.5, .6} : QPointF{.375, .5};
  const QPointF first = gesture == 0 ? QPointF{.54, .55}
      : gesture == 1 ? QPointF{.405, .6} : QPointF{.375, .35};
  const QPointF last = gesture == 0 ? QPointF{.59, .45}
      : gesture == 1 ? QPointF{.435, .7} : QPointF{.375, .2};
  NativeCurveEdit edit{inlet.value(), {}};
  const auto before = inlet.m_curve->toCurveData();
  const auto commands = doc.commandStack().size();
  mouse(QEvent::MouseButtonPress, from);
  CHECK(scene.mouseGrabberItem() == &view);
  mouse(QEvent::MouseMove, first);
  const auto intermediate = inlet.value();
  CHECK(intermediate != edit.before);
  // Relayout must not hide the page and implicitly release the gesture.
  CHECK(scene.mouseGrabberItem() == &view);
  mouse(QEvent::MouseMove, last);
  edit.after = inlet.value();
  CHECK(edit.after != intermediate);
  CHECK(scene.mouseGrabberItem() == &view);
  mouse(QEvent::MouseButtonRelease, last);
  CHECK(scene.mouseGrabberItem() == nullptr);
  CHECK(inlet.value() == edit.after);
  CHECK(doc.commandStack().size() == commands + 1);
  const auto after = inlet.m_curve->toCurveData();
  REQUIRE(after.size() == before.size());
  const auto close = [&](QPointF actual, QPointF expected) {
    CHECK(std::abs(actual.x() - expected.x()) <= 2. / rect.width());
    CHECK(std::abs(actual.y() - expected.y()) <= 2. / rect.height());
  };
  if(gesture == 0) {
    close(after[1].end, last);
    CHECK(after[1].start == before[1].start);
  } else if(gesture == 1) {
    close(after[1].start, before[1].start + last - from);
    close(after[1].end, before[1].end + last - from);
  } else {
    for(std::size_t i = 0; i < before.size(); ++i) {
      CHECK(after[i].start == before[i].start);
      CHECK(after[i].end == before[i].end);
    }
    CHECK(after[1].specificSegmentData.value<Curve::PowerSegmentData>().gamma
          != before[1].specificSegmentData.value<Curve::PowerSegmentData>().gamma);
    QKeyEvent shift{QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier};
    QApplication::sendEvent(&window, &shift);
    CHECK(settings.tool() == Curve::Tool::Select);
  }
  return edit;
}

Curve::View& onlyCurveView(const QGraphicsScene& scene)
{
  Curve::View* result{};
  int count{};
  for(auto* child : scene.items())
    if(auto* view = dynamic_cast<Curve::View*>(child)) { result = view; ++count; }
  REQUIRE(count == 1);
  return *result;
}
}

TEST_CASE("Wavetable native sample strokes retain the grab through channel and model changes", "[emulation][wavetable][document][gui][gesture]") {
  score::test::run_in_gui_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    auto* documentPresenter = score::IDocument::try_presenterDelegate<Scenario::ScenarioDocumentPresenter>(*doc);
    REQUIRE(documentPresenter);
    auto* factory = documentPresenter->context().processList.findDefaultFactory(model); REQUIRE(factory);
    const auto channelValues = [&] {
      std::array<ossia::value, 6> result;
      for(int ch = 0; ch < 6; ++ch)
        result[ch] = control(model, channelControl(ch, Wave)).value();
      return result;
    };
    QGraphicsScene scene;
    std::unique_ptr<score::ResizeableItem> item{factory->makeItem(model, documentPresenter->context(), nullptr)};
    REQUIRE(item); scene.addItem(item.get());
    QGraphicsView window{&scene};
    window.setSceneRect(-20., -20., 1100., 650.);
    window.resize(1140, 700);
    REQUIRE(score::test::showAndActivate(window));
    score::test::settle();
    auto* editor = &onlyWaveformEditor(scene);
    const auto select = [&](int channel, int count) {
      score::GraphicsTabLayout* tabs{};
      for(auto* child : scene.items())
        if(auto* candidate = dynamic_cast<score::GraphicsTabLayout*>(child)) tabs = candidate;
      REQUIRE(tabs);
      score::QGraphicsEnum* bar{};
      for(auto* child : tabs->childItems())
        if(auto* candidate = dynamic_cast<score::QGraphicsEnum*>(child)) bar = candidate;
      REQUIRE(bar);
      CHECK(bar->columns == count);
      const auto before = channelValues();
      const auto rect = bar->boundingRect().adjusted(2., 2., -2., -2.);
      score::test::mouseClick(*window.viewport(), window.mapFromScene(bar->mapToScene(
          {rect.left() + (channel + .5) * rect.width() / count, rect.center().y()})));
      score::test::settle();
      CHECK(tabs->currentIndex() == channel);
      CHECK(channelValues() == before);
      CHECK(&onlyWaveformEditor(scene) == editor);
    };
    for(auto chip : {Model::Scc, Model::SccPlus, Model::HuC6280, Model::Scc}) {
      control(model, ChipModel).setValue(int(chip));
      const int channels = chip == Model::HuC6280 ? 6 : 5;
      for(int ch = 0; ch < channels; ++ch) {
        select(ch, channels);
        const int waveChannel = chip == Model::Scc && ch == 4 ? 3 : ch;
        auto& inlet = waveformControl(model, channelControl(waveChannel, Wave));
        CHECK(inlet.amplitudeLevels() == (chip == Model::HuC6280 ? 32 : 256));
        {
          CAPTURE(int(chip), ch, waveChannel);
          // Reset only the selected authoring resource, then test real mouse
          // delivery and undo after the shared editor switches channel.
          inlet.setWaveform(drawnWave());
          const auto otherChannels = channelValues();
          const auto edit = dragSamples(*doc, window, *editor, inlet);
          for(int other = 0; other < 6; ++other)
            if(other != waveChannel)
              CHECK(control(model, channelControl(other, Wave)).value() == otherChannels[other]);
          // Undo must still address the real inlet after the shared editor has
          // been rebound to a different channel.
          select(ch == 0 ? 1 : 0, channels);
          doc->commandStack().undo(); CHECK(inlet.value() == edit.before);
          doc->commandStack().redo(); CHECK(inlet.value() == edit.after);
          select(ch, channels);
        }
      }
      const auto edited = channelValues();
      ProcessModel preset{TimeVal::fromMsecs(1000), {}, Id<Process::ProcessModel>{999}, nullptr};
      preset.loadPreset(model.savePreset()); CHECK(preset.error().isEmpty());
      const auto json = toValue(score::marshall<JSONObject>(*base));
      JSONObject::Deserializer reader{json}; ProcessModel restored{reader, nullptr};
      QByteArray bytes; DataStream::Serializer writer{&bytes};
      TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
      writer.read(static_cast<const Process::ProcessModel&>(model)); writer.read(model);
      DataStream::Deserializer binaryReader{bytes}; ProcessModel binary{binaryReader, nullptr};
      for(auto* copy : {&preset, &restored, &binary})
        for(int ch = 0; ch < 6; ++ch) {
          CHECK(control(*copy, channelControl(ch, Wave)).value() == edited[ch]);
          CHECK(waveformControl(*copy, channelControl(ch, Wave)).wavetable()
              == waveformControl(model, channelControl(ch, Wave)).wavetable());
        }
    }
  });
}

TEST_CASE("Stock Wavecycle uses the same native point segment and curvature gestures", "[emulation][wavetable][document][gui][wavecycle-gesture]") {
  score::test::run_in_gui_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* model = score::test::add_process(*doc, QStringLiteral("494bd8a3-e973-4fb0-b84b-b4ed3c0068a1"), {});
    if(!model) SKIP("Stock Wavecycle is not built in this configuration");
    REQUIRE(model->inlets().size() >= 1);
    auto* inlet = dynamic_cast<Dataflow::CurveInlet*>(model->inlets()[0]); REQUIRE(inlet);
    auto* presenter = score::IDocument::try_presenterDelegate<Scenario::ScenarioDocumentPresenter>(*doc); REQUIRE(presenter);
    auto* factory = presenter->context().processList.findDefaultFactory(*model);
    QGraphicsScene scene;
    std::unique_ptr<score::ResizeableItem> item;
    if(factory) item.reset(factory->makeItem(*model, presenter->context(), nullptr));
    // Wavecycle has no custom layout: NodeItem uses this stock control panel.
    if(!item) item.reset(new Process::DefaultEffectItem{false, *model, presenter->context(), nullptr});
    REQUIRE(item); scene.addItem(item.get());
    QGraphicsView window{&scene};
    window.setSceneRect(-20., -20., 1100., 650.);
    window.resize(1140, 700);
    REQUIRE(score::test::showAndActivate(window));
    score::test::settle();
    for(int gesture = 0; gesture < 3; ++gesture) {
      CAPTURE(gesture);
      const auto edit = dragNativeCurve(*doc, window, onlyCurveView(scene), *inlet, gesture);
      doc->commandStack().undo(); CHECK(inlet->value() == edit.before);
      doc->commandStack().redo(); CHECK(inlet->value() == edit.after);
    }
  });
}

TEST_CASE("Wavetable legacy allocation selectors gain mono without changing saved routing", "[emulation][wavetable][document][allocation]")
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
      CHECK(engine.activeVoices() == 5);
    }
  });
}
