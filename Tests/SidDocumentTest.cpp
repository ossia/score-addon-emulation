// SPDX-License-Identifier: GPL-3.0-or-later
#include <Process/ExecutionTransaction.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <Process/Commands/LoadPresetCommandFactory.hpp>
#include <Process/Dataflow/ControlWidgets.hpp>
#include <Process/Preset.hpp>
#include <Process/ProcessList.hpp>
#include <Process/Dataflow/PortItem.hpp>
#include <Process/DocumentPlugin.hpp>
#include <Process/ProcessMimeSerialization.hpp>
#include <Scenario/Commands/CommandAPI.hpp>
#include <Scenario/Commands/Interval/AddProcessToInterval.hpp>
#include <Scenario/Document/ScenarioDocument/ScenarioDocumentPresenter.hpp>
#include <Scenario/Document/ScenarioDocument/ScenarioDocumentView.hpp>
#include <score/document/DocumentInterface.hpp>
#include <score/graphics/layouts/GraphicsGridLayout.hpp>
#include <score/graphics/layouts/GraphicsStripDetailLayout.hpp>
#include <Emulation/Sid/Controls.hpp>
#include <Emulation/Sid/Executor/Node.hpp>
#include <Emulation/Sid/ProcessModel.hpp>
#include <Emulation/Volume.hpp>
#include <Execution/DocumentPlugin.hpp>
#include <score/model/EntitySerialization.hpp>
#include <score/serialization/VisitorCommon.hpp>
#include <core/command/CommandStack.hpp>
#include <core/document/Document.hpp>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsSceneMouseEvent>
#include <ossia/dataflow/execution_state.hpp>
#include <ossia/dataflow/data_copy.hpp>
#include <libremidi/ump_events.hpp>
#include <catch2/catch_test_macros.hpp>
#include <score_test/App.hpp>
#include <score_test/Project.hpp>
#include <score_test/Events.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
namespace
{
using namespace Emulation::Sid;
Process::ControlInlet& control(ProcessModel& p, int c)
{ return *static_cast<Process::ControlInlet*>(p.inlets()[c + 1]); }
constexpr char uuid[] = "ce87e734-2cf5-4044-bff4-c849e98bce4a";
void drain(Execution::DocumentPlugin& plug)
{
  const auto& context = plug.context();
  Execution::ExecutionCommand command;
  ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
  while(context.executionQueue.try_dequeue(command))
  { command(); context.gcQueue.enqueue(Execution::gc(std::move(command))); }
  ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
  Execution::GCCommand retired;
  while(context.gcQueue.try_dequeue(retired)) retired();
}
template<typename T> T* childItem(QGraphicsItem* item)
{
  if(auto* result = dynamic_cast<T*>(item)) return result;
  for(auto* child : item->childItems())
    if(auto* result = childItem<T>(child)) return result;
  return nullptr;
}
}
TEST_CASE("SID gain domains migrate without rescaling saved values and presets", "[emulation][sid][document][preset]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    for(float gain : {0.f, .375f, 1.f, 4.5f, Emulation::MaxVolume}) {
      CAPTURE(gain);
      control(model, Volume).setValue(gain); control(model, Volume).setInit(.625f);
      control(model, Volume).setDomain(ossia::make_domain(0.f, gain <= 1.f ? 1.f : Emulation::MaxVolume));
      REQUIRE(model.patch().volume == gain);
      const auto json = toValue(score::marshall<JSONObject>(*base));
      JSONObject::Deserializer reader{json}; ProcessModel restored{reader, nullptr};
      QByteArray bytes; DataStream::Serializer writer{&bytes};
      TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
      writer.read(static_cast<const Process::ProcessModel&>(model)); writer.read(model);
      DataStream::Deserializer binaryReader{bytes}; ProcessModel binary{binaryReader, nullptr};
      for(auto* copy : {&restored, &binary}) {
        CHECK(copy->patch().volume == gain);
        CHECK(control(*copy, Volume).value() == ossia::value{gain});
        CHECK(control(*copy, Volume).init() == ossia::value{.625f});
        CHECK(control(*copy, Volume).id() == Id<Process::Port>{Volume + 1});
        CHECK(control(*copy, Volume).domain().get().convert_min<float>() == 0.f);
        CHECK(control(*copy, Volume).domain().get().convert_max<float>() == Emulation::MaxVolume);
        CHECK(control(*copy, ChipVolume).domain().get().convert_max<int>() == 15);
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
      CHECK(restored.error().isEmpty()); CHECK(restored.patch().volume == gain);
      for(float invalid : {-1.f, Emulation::MaxVolume + 1.f,
          std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        control(restored, Volume).setValue(invalid);
        CHECK(restored.patch().volume == gain);
        CHECK(control(restored, Volume).value() == ossia::value{gain});
        CHECK_FALSE(restored.error().isEmpty());
      }
      for(float invalid : {-1.f, Emulation::MaxVolume + 1.f}) {
        auto rejected = preset;
        rejected.data = "[[" + QByteArray::number(Volume + 1) + ", {\"Float\":"
            + QByteArray::number(invalid) + "}]]";
        restored.loadPreset(rejected);
        CHECK(restored.patch().volume == gain); CHECK_FALSE(restored.error().isEmpty());
      }
    }
  });
}

TEST_CASE("SID semantic patch and volume survive JSON and binary document state", "[emulation][sid][document]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model, VoiceMode).setValue(int(Allocation::Poly));
    control(model, oscillatorControl(0, Ring)).setValue(true);
    control(model, oscillatorControl(2, Semitones)).setValue(7);
    control(model, FilterCurve).setValue(77);
    control(model, Volume).setValue(.25f);
    const auto expected = model.patch();
    CHECK(model.patch().allocation == Allocation::Poly);
    CHECK(model.patch().oscillators[0].ring);
    CHECK(model.error().isEmpty());
    const auto json = toValue(score::marshall<JSONObject>(*base));
    JSONObject::Deserializer reader{json};
    ProcessModel restored{reader, nullptr};
    CHECK(restored.patch() == expected);
    CHECK(ossia::convert<float>(control(restored, Volume).value()) == .25f);
    control(restored, oscillatorControl(0, Ring)).setValue(false);
    CHECK_FALSE(restored.patch().oscillators[0].ring);
    CHECK(model.patch().oscillators[0].ring);
    QByteArray bytes;
    DataStream::Serializer writer{&bytes};
    TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
    writer.read(static_cast<const Process::ProcessModel&>(model));
    writer.read(model);
    DataStream::Deserializer binaryReader{bytes};
    ProcessModel binary{binaryReader, nullptr};
    CHECK(binary.patch() == expected);
    CHECK(ossia::convert<float>(control(binary, Volume).value()) == .25f);
  });
}
TEST_CASE("SID native token windows preserve timestamped MIDI, gain and pause cleanup", "[emulation][sid][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc));
    drain(plug);
    auto& state = *plug.context().execState;
    REQUIRE(state.bufferSize >= 64);
    const int frames = state.bufferSize, quarter = frames / 4;
    Executor::Node reference{state}, adjusted{state};
    for(auto* n : {&reference, &adjusted})
    {
      auto engine = std::make_shared<Engine>(state.sampleRate, defaultPatch());
      n->install(engine); REQUIRE(n->ready());
      auto on = libremidi::from_midi1::note_on(0, 60, 100); on.timestamp = quarter;
      n->midi->data.messages.push_back(on);
    }
    adjusted.controls[Volume]->data.write_value(Emulation::MaxVolume, 2 * quarter);
    adjusted.controls[Volume]->data.write_value(0.f, 3 * quarter);
    adjusted.controls[Volume]->data.write_value(Emulation::MaxVolume + 1.f, 2 * quarter + 1);
    adjusted.controls[Volume]->data.write_value(-1.f, 2 * quarter + 2);
    adjusted.controls[Volume]->data.write_value(std::numeric_limits<float>::infinity(), 2 * quarter + 3);
    adjusted.controls[Volume]->data.write_value(std::numeric_limits<float>::quiet_NaN(), 2 * quarter + 4);
    ossia::token_request token;
    token.prev_date = ossia::time_value{0};
    token.date = ossia::time_value{int64_t(frames * ossia::flicks_per_second<double> / state.sampleRate)};
    token.start_sample = 0; token.length_sample = frames;
    auto first = token; first.length_sample = 2 * quarter;
    first.date = ossia::time_value{int64_t(2 * quarter * ossia::flicks_per_second<double> / state.sampleRate)};
    auto second = token; second.prev_date = first.date;
    second.start_sample = 2 * quarter; second.length_sample = frames - 2 * quarter;
    state.samples_since_start += frames;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    reference.run(token, ossia::exec_state_facade{&state});
    adjusted.run(first, ossia::exec_state_facade{&state});
    adjusted.run(second, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    const auto& a = reference.audio->data.channel(0);
    const auto& b = adjusted.audio->data.channel(0);
    for(int i = 0; i < frames; ++i)
    {
      const double gain = i < 2 * quarter ? 1. : i < 3 * quarter ? Emulation::MaxVolume : 0.;
      CHECK(b[i] == gain * a[i]);
      if(i < quarter) CHECK(a[i] == 0.);
    }
    const std::vector<double> before{b.begin(), b.end()};
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    adjusted.run(token, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    CHECK(std::equal(before.begin(), before.end(), b.begin()));
    reference.midi->data.messages.clear();
    auto paused = token; paused.date = paused.prev_date; paused.length_sample = 0;
    state.samples_since_start += frames;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    reference.run(paused, ossia::exec_state_facade{&state});
    state.samples_since_start += frames;
    reference.run(token, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    CHECK(std::all_of(a.begin(), a.end(), [](double x) { return x == 0.; }));
    for(int i = 0; i < 8193; ++i)
      reference.midi->data.messages.push_back(libremidi::from_midi1::note_on(0, 60, 100));
    state.samples_since_start += frames;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    reference.run(token, ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    CHECK(reference.error());
    CHECK(std::all_of(a.begin(), a.end(), [](double x) { return x == 0.; }));
  });
}

TEST_CASE("SID restores stereo output after graph buffer recycling", "[emulation][sid][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    for(auto model : {Model::Mos6581, Model::Csg8580}) {
      auto patch = defaultPatch(); patch.model = model;
      Executor::Node node{state}; node.edit(patch);
      auto engine = std::make_shared<Engine>(state.sampleRate, patch); node.install(engine);
      Engine reference{double(state.sampleRate), patch};
      const std::array<uint8_t, 3> note{0x90, 60, 100}; reference.midi(note);
      std::vector<double> left(state.bufferSize), right(state.bufferSize);
      ossia::token_request token;
      token.prev_date = ossia::time_value{0};
      token.date = ossia::time_value{int64_t(state.bufferSize * ossia::flicks_per_second<double> / state.sampleRate)};
      token.start_sample = 0; token.length_sample = state.bufferSize;
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
        if(tick == 0) node.midi->data.messages.push_back(libremidi::from_midi1::note_on(0, 60, 100));
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

TEST_CASE("SID Three pulse keys keeps every modulation control editable and undoable", "[emulation][sid][sync][document]")
{
  score::test::run_in_gui_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* presenter = score::IDocument::try_presenterDelegate<Scenario::ScenarioDocumentPresenter>(*doc);
    REQUIRE(presenter);
    presenter->setNodalMode(true);
    Scenario::Command::Macro macro{new Scenario::Command::DropProcessInIntervalMacro, doc->context()};
    auto* base = macro.createProcessInNewSlot(score::test::base_interval(*doc),
        Process::ProcessData{UuidKey<Process::ProcessModel>{uuid}, QStringLiteral("SID"), {}},
        QPointF{20., 20.});
    REQUIRE(base);
    macro.commit();
    auto& model = static_cast<ProcessModel&>(*base);
    model.setFoldMode(Process::FoldMode::Unfolded);
    const auto directory = QFileInfo{QString::fromUtf8(__FILE__)}.absoluteDir();
    QFile file{directory.filePath(QStringLiteral("../Presets/SID/Three pulse keys.scp"))};
    REQUIRE(file.open(QIODevice::ReadOnly));
    auto preset = Process::Preset::fromJson(ctx.interfaces<Process::ProcessFactoryList>(), file.readAll());
    REQUIRE(preset);
    auto* command = ctx.interfaces<Process::LoadPresetCommandFactoryList>().make(
        &Process::LoadPresetCommandFactory::make, model, *preset, doc->context());
    REQUIRE(command);
    CommandDispatcher<>{doc->context().commandStack}.submit(command);
    REQUIRE(model.patch().allocation == Allocation::Poly);
    score::test::settle();
    // Deliver scene events directly so the regression also runs on Qt's offscreen platform.
    const auto clickGraphics = [](QGraphicsItem* item, QPointF position) {
      QGraphicsSceneMouseEvent press{QEvent::GraphicsSceneMousePress};
      press.setButton(Qt::LeftButton); press.setButtons(Qt::LeftButton);
      press.setPos(position); press.setScenePos(item->mapToScene(position));
      item->scene()->sendEvent(item, &press);
      QGraphicsSceneMouseEvent release{QEvent::GraphicsSceneMouseRelease};
      release.setButton(Qt::LeftButton);
      release.setPos(position); release.setScenePos(item->mapToScene(position));
      item->scene()->sendEvent(item, &release);
    };

    for(int voice = 0; voice < 3; ++voice)
    {
      CAPTURE(voice);
      auto* wavePort = presenter->context().dataflow.ports().at(&control(model, oscillatorControl(voice, Waveform)));
      auto* row = wavePort->parentItem();
      while(row && !dynamic_cast<score::GraphicsSelectableRow*>(row)) row = row->parentItem();
      REQUIRE(row);
      clickGraphics(row, QPointF{16., 10.});
      score::test::settle();
      for(auto field : {Sync, Ring})
      {
        CAPTURE(field);
        auto& inlet = control(model, oscillatorControl(voice, field));
        auto* port = presenter->context().dataflow.ports().at(&inlet);
        auto* graphics = childItem<score::QGraphicsCheckBox>(port->parentItem());
        REQUIRE(graphics);
        auto* item = static_cast<QGraphicsItem*>(graphics);
        REQUIRE(item->isVisible());
        REQUIRE(item->isEnabled());
        QWidget inspector;
        auto* checkbox = WidgetFactory::Toggle::make_widget(inlet, doc->context(), &inspector, &inspector);
        const auto check = [&](bool value) {
          CHECK(bool(controlValue(model.patch(), oscillatorControl(voice, field))) == value);
          CHECK(ossia::convert<bool>(inlet.value()) == value);
          CHECK(checkbox->isChecked() == value);
          CHECK(model.patch().allocation == Allocation::Poly);
          CHECK(model.error().isEmpty());
        };
        const auto before = doc->commandStack().currentIndex();
        clickGraphics(item, item->boundingRect().center());
        score::test::settle();
        check(true);
        CHECK(doc->commandStack().currentIndex() == before + 1);
        doc->commandStack().undo(); check(false);
        doc->commandStack().redo(); check(true);
        checkbox->click(); check(false);
        doc->commandStack().undo(); check(true);
        doc->commandStack().redo(); check(false);
      }
    }
  });
}

TEST_CASE("SID native factory presets transition and undo across all allocations", "[emulation][sid][preset][document]")
{
  score::test::run_in_gui_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    const std::array names{"Copper unison", "Glass triangle", "Three pulse keys", "Ring circuit", "Channel laboratory"};
    std::array<Process::Preset, 5> presets;
    std::array<Patch, 5> expected;
    auto& processFactories = ctx.interfaces<Process::ProcessFactoryList>();
    auto& commands = ctx.interfaces<Process::LoadPresetCommandFactoryList>();
    const auto directory = QFileInfo{QString::fromUtf8(__FILE__)}.absoluteDir();
    for(std::size_t i = 0; i < names.size(); ++i)
    {
      QFile file{directory.filePath(QStringLiteral("../Presets/SID/%1.scp").arg(QString::fromLatin1(names[i])))};
      REQUIRE(file.open(QIODevice::ReadOnly));
      auto preset = Process::Preset::fromJson(processFactories, file.readAll());
      REQUIRE(preset);
      CHECK(model.presetMatches(*preset));
      presets[i] = *preset;
      model.loadPreset(presets[i]);
      REQUIRE(model.error().isEmpty());
      expected[i] = model.patch();
      Engine engine{48000., expected[i]};
      for(uint8_t channel = 0; channel < 3; ++channel)
      {
        const std::array<uint8_t, 3> note{uint8_t(0x90 | channel), uint8_t(60 + channel * 4), 100};
        engine.midi(note);
      }
      std::array<double, 4096> left{}, right{};
      engine.render(left.data(), right.data(), left.size());
      CHECK(left == right);
      CHECK(std::any_of(left.begin(), left.end(), [](double sample) { return std::abs(sample) > .001; }));
      CHECK(std::all_of(left.begin(), left.end(), [](double sample) { return std::isfinite(sample); }));
    }
    CHECK(expected[1].oscillators[2].semitones == 19);
    CHECK(expected[2].allocation == Allocation::Poly);
    CHECK(expected[3].oscillators[0].ring);
    CHECK(expected[4].allocation == Allocation::Fixed);
    for(std::size_t from = 0; from < presets.size(); ++from)
      for(std::size_t to = 0; to < presets.size(); ++to)
      {
        CAPTURE(from, to);
        model.loadPreset(presets[from]);
        auto* command = commands.make(&Process::LoadPresetCommandFactory::make, model, presets[to], doc->context());
        REQUIRE(command);
        const auto index = doc->commandStack().currentIndex();
        CommandDispatcher<>{doc->context().commandStack}.submit(command);
        CHECK(model.patch() == expected[to]);
        CHECK(model.error().isEmpty());
        CHECK(doc->commandStack().currentIndex() == index + 1);
        doc->commandStack().undo();
        CHECK(model.patch() == expected[from]);
        CHECK(model.error().isEmpty());
        doc->commandStack().redo();
        CHECK(model.patch() == expected[to]);
        CHECK(model.error().isEmpty());
      }
    model.loadPreset(presets[0]);
    control(model, oscillatorControl(0, Sync)).setValue(true);
    control(model, Volume).setValue(.25f);
    const auto savedPatch = model.patch();
    const auto saved = model.savePreset();
    auto* command = commands.make(&Process::LoadPresetCommandFactory::make, model, presets[2], doc->context());
    REQUIRE(command);
    CommandDispatcher<>{doc->context().commandStack}.submit(command);
    CHECK(model.patch() == expected[2]);
    doc->commandStack().undo();
    CHECK(model.patch() == savedPatch);
    CHECK(ossia::convert<float>(control(model, Volume).value()) == .25f);
    doc->commandStack().redo();
    model.loadPreset(saved);
    CHECK(model.patch() == savedPatch);
    Engine restored{48000., model.patch()}, reference{48000., savedPatch};
    const std::array<uint8_t, 3> note{0x90, 60, 100};
    restored.midi(note); reference.midi(note);
    std::array<double, 4096> actual{}, expectedAudio{}, right{};
    restored.render(actual.data(), right.data(), actual.size());
    reference.render(expectedAudio.data(), right.data(), expectedAudio.size());
    CHECK(actual == expectedAudio);
  });
}

TEST_CASE("SID preset validation precedes every published control and reopened sound", "[emulation][sid][qualification][preset]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    ProcessModel source{TimeVal::fromMsecs(1000), {}, Id<Process::ProcessModel>{999}, nullptr};
    control(source, VoiceMode).setValue(int(Allocation::Poly));
    control(source, oscillatorControl(0, Ring)).setValue(true);
    control(source, oscillatorControl(1, Sync)).setValue(true);
    control(source, Volume).setValue(.25f);
    const auto expected = source.patch();
    int publications{}, inletChanges{};
    QObject::connect(&model, &ProcessModel::patchChanged, &model, [&] {
      ++publications; CHECK(model.patch() == expected);
    });
    for(int c = 0; c < ControlCount; ++c)
      QObject::connect(&control(model, c), &Process::ControlInlet::valueChanged, &model, [&] {
        ++inletChanges; CHECK(model.patch() == expected);
      });
    model.loadPreset(source.savePreset());
    REQUIRE(publications == 1); REQUIRE(inletChanges > 0);
    const int acceptedChanges = inletChanges;
    auto invalid = source.savePreset();
    invalid.data = "[[" + QByteArray::number(Volume + 1) + ", {\"Float\":0.75}],["
        + QByteArray::number(Cutoff + 1) + ", {\"Int\":9999}]]";
    model.loadPreset(invalid);
    CHECK(publications == 1); CHECK(inletChanges == acceptedChanges);
    CHECK(model.patch() == expected); CHECK_FALSE(model.error().isEmpty());
    auto json = toValue(score::marshall<JSONObject>(static_cast<const Process::ProcessModel&>(model)));
    JSONObject::Deserializer reader{json}; ProcessModel reopened{reader, nullptr};
    Engine before{48000., expected}, after{48000., reopened.patch()};
    const std::array<uint8_t, 3> note{0x90, 60, 110}; before.midi(note); after.midi(note);
    std::array<double, 2048> a{}, ar{}, b{}, br{};
    before.render(a.data(), ar.data(), a.size()); after.render(b.data(), br.data(), b.size());
    CHECK(a == b); CHECK(ar == br);
  });
}

TEST_CASE("SID stale preparation replies cannot replace the accepted live engine", "[emulation][sid][qualification][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    Executor::Node node{state};
    auto patch = defaultPatch();
    auto engine = std::make_shared<Engine>(state.sampleRate, patch), retained = engine;
    node.install(engine); REQUIRE(node.ready());
    const std::array<uint8_t, 3> note{0x90, 60, 110}; retained->midi(note);
    patch.model = patch.model == Model::Mos6581 ? Model::Csg8580 : Model::Mos6581;
    auto stale = std::make_shared<Engine>(state.sampleRate, patch), rejected = stale;
    node.install(stale);
    CHECK(node.ready()); CHECK(stale == rejected); CHECK(retained->activeVoices() == 3);
    node.all_notes_off();
    CHECK(retained->activeVoices() == 0);
  });
}

TEST_CASE("SID native execution publishes MIDI and pitch recovery without masking other errors", "[emulation][sid][execution][errors]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plugin = doc->context().plugin<Execution::DocumentPlugin>();
    plugin.reload(true, score::test::base_interval(*doc)); drain(plugin);
    auto& state = *plugin.context().execState;
    Patch patch; patch.allocation = Allocation::Fixed;
    Executor::Node node{state}; node.edit(patch);
    auto engine = std::make_shared<Engine>(state.sampleRate, patch);
    node.install(engine); REQUIRE(node.ready());
    ossia::token_request token;
    token.prev_date = ossia::time_value{0};
    token.date = ossia::time_value{int64_t(state.bufferSize * ossia::flicks_per_second<double> / state.sampleRate)};
    token.start_sample = 0; token.length_sample = state.bufferSize;
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
    patch.volume = .5; node.edit(patch);
    CHECK(node.error());
    send(libremidi::from_midi1::note_on(0, 106, 100));
    CHECK_FALSE(node.error());
    send(libremidi::ump_events::pitch_bend(0, 0, 0xffffffffu));
    REQUIRE(node.error());
    patch.volume = .25; node.edit(patch);
    CHECK(node.error());
    send(libremidi::from_midi1::note_on(1, 60, 100));
    CHECK(node.error());
    send(libremidi::ump_events::pitch_bend(0, 0, 0x80000000u));
    CHECK_FALSE(node.error());
    send(libremidi::from_midi1::note_on(3, 60, 100));
    REQUIRE(node.error());
    send(libremidi::from_midi1::note_off(1, 60, 0));
    CHECK_FALSE(node.error());
    auto invalid = patch; invalid.cutoff = 2048;
    node.edit(invalid); REQUIRE(node.error());
    send(libremidi::from_midi1::note_off(0, 106, 0));
    CHECK(node.error());
    node.edit(patch); CHECK_FALSE(node.error());
  });
}

TEST_CASE("SID authored edits and preset recall cannot clear preparation or execution errors", "[emulation][sid][document][errors]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    const auto preset = model.savePreset();
    const auto preparation = QStringLiteral("SID preparation unavailable");
    const auto runtime = QStringLiteral("SID pitch out of range");
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
