// SPDX-License-Identifier: GPL-3.0-or-later
#include <Emulation/La/Controls.hpp>
#include <Emulation/La/ProcessModel.hpp>
#include <Emulation/La/Executor/Node.hpp>
#include <Emulation/Volume.hpp>
#include <Emulation/ExecutionProcess.hpp>
#include <Process/ExecutionTransaction.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <Process/Dataflow/PortItem.hpp>
#include <Process/ProcessContext.hpp>
#include <Process/Commands/LoadPresetCommandFactory.hpp>
#include <Process/Commands/SetControlValue.hpp>
#include <Process/DocumentPlugin.hpp>
#include <Process/ProcessList.hpp>
#include <Process/ProcessMimeSerialization.hpp>
#include <Process/ExternalFiles.hpp>
#include <Process/ProjectConsolidation.hpp>
#include <Process/MissingFiles.hpp>
#include <score/tools/ProjectFiles.hpp>
#include <Execution/DocumentPlugin.hpp>
#include <Scenario/Document/ScenarioDocument/ScenarioDocumentPresenter.hpp>
#include <score/document/DocumentInterface.hpp>
#include <score/graphics/RectItem.hpp>
#include <score/graphics/layouts/GraphicsTabLayout.hpp>
#include <score/graphics/widgets/QGraphicsToggle.hpp>
#include <score/model/EntitySerialization.hpp>
#include <score/serialization/VisitorCommon.hpp>
#include <core/command/CommandStack.hpp>
#include <core/document/Document.hpp>
#include <score_test/App.hpp>
#include <score_test/Project.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ossia/dataflow/execution_state.hpp>
#include <ossia/dataflow/data_copy.hpp>
#include <libremidi/ump_events.hpp>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
namespace
{
using namespace Emulation::La;
constexpr char uuid[] = "c39b18a6-7840-45b1-a5bc-7e884ecfea02";
Process::ControlInlet& control(ProcessModel& p, int c)
{ return *static_cast<Process::ControlInlet*>(p.inlets()[c + 1]); }
void drain(Execution::DocumentPlugin& plug)
{
  const auto& ctx = plug.context();
  Execution::ExecutionCommand command;
  ossia::set_thread_pinned(ossia::thread_type::Audio,0);
  while(ctx.executionQueue.try_dequeue(command)) { command(); ctx.gcQueue.enqueue(Execution::gc(std::move(command))); }
  ossia::set_thread_pinned(ossia::thread_type::Ui,0);
  Execution::GCCommand retired; while(ctx.gcQueue.try_dequeue(retired)) retired();
}
}
TEST_CASE("LA document binary and JSON preserve native timbres, ROM hashes, selectors and types", "[emulation][la][document]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc,QString::fromLatin1(uuid),{}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    CHECK(model.patch().values[partControl(0, Channel)] == 0);
    CHECK(model.patch().values[RhythmChannel] == 16);
    for(int part = 1; part < 8; ++part)
      CHECK(model.patch().values[partControl(part, Channel)] == 16);
    // Authored multichannel state must not be mistaken for new-object defaults.
    control(model, RhythmChannel).setValue(9);
    for(int part = 0; part < 8; ++part)
      control(model, partControl(part, Channel)).setValue(part + 1);
    control(model,ControlROM).setValue(std::string{"user/control.rom"});
    control(model,PcmROM).setValue(std::string{"user/pcm.rom"});
    auto refs = *model.patch().roms; refs.controlHash = std::string(40,'a'); refs.pcmHash = std::string(40,'b');
    model.acceptROMIdentity(refs,QStringLiteral("state-test"));
    control(model,EditTimbre).setValue(12);
    control(model,TimbreBase).setValue(8);
    control(model,partialControl(3,23)).setValue(36);
    control(model,EditTimbre).setValue(7);
    control(model,partialControl(2,53)).setValue(73);
    control(model,partControl(4,Bank)).setValue(1);
    control(model,partControl(4,Program)).setValue(12);
    control(model,Volume).setValue(.25f);
    control(model,VoiceAllocation).setValue(int(Allocation::MidiChannel));
    const auto expected = model.patch();
    const auto json = toValue(score::marshall<JSONObject>(*base));
    JSONObject::Deserializer reader{json}; ProcessModel restored{reader,nullptr};
    REQUIRE(restored.patch() == expected);
    CHECK(ossia::convert<float>(control(restored,Volume).value()) == .25f);
    CHECK(control(restored,ControlROM).value().get_type() == ossia::val_type::STRING);
    CHECK(control(restored,Volume).value().get_type() == ossia::val_type::FLOAT);
    CHECK(control(restored,EditTimbre).value().get_type() == ossia::val_type::INT);
    CHECK(control(restored,ReverbEnabled).value().get_type() == ossia::val_type::BOOL);
    control(restored,EditTimbre).setValue(12);
    CHECK(ossia::convert<int>(control(restored,partialControl(3,23)).value()) == 36);
    CHECK(restored.patch().timbres[7][14 + 2*58 + 53] == 73);
    QByteArray bytes; DataStream::Serializer writer{&bytes};
    TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
    writer.read(static_cast<const Process::ProcessModel&>(model));
    writer.read(model);
    DataStream::Deserializer binaryReader{bytes};
    ProcessModel binary{binaryReader, nullptr};
    CHECK(binary.patch() == expected);
    CHECK(encodePatch(expected).size() < 20000); // semantic state and references, never ROM payloads
  });
}

TEST_CASE("LA native Volume round-trips boost and migrates old domains without ROMs", "[emulation][la][document][preset]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    for(float gain : {0.f, .375f, 1.f, 4.5f, Emulation::MaxVolume}) {
      control(model, Volume).setValue(gain); control(model, Volume).setInit(.625f);
      control(model, Volume).setDomain(ossia::make_domain(0.f, gain <= 1.f ? 1.f : Emulation::MaxVolume));
      const auto expected = model.patch(); REQUIRE(expected.volume == gain);
      const auto json = toValue(score::marshall<JSONObject>(*base));
      JSONObject::Deserializer reader{json}; ProcessModel restored{reader, nullptr};
      QByteArray bytes; DataStream::Serializer writer{&bytes};
      TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
      writer.read(static_cast<const Process::ProcessModel&>(model)); writer.read(model);
      DataStream::Deserializer binaryReader{bytes}; ProcessModel binary{binaryReader, nullptr};
      for(auto* copy : {&restored, &binary}) {
        CHECK(copy->patch() == expected);
        CHECK(control(*copy, Volume).value() == ossia::value{gain});
        CHECK(control(*copy, Volume).init() == ossia::value{.625f});
        CHECK(control(*copy, Volume).domain().get().convert_min<float>() == 0.f);
        CHECK(control(*copy, Volume).domain().get().convert_max<float>() == Emulation::MaxVolume);
        CHECK(control(*copy, MasterLevel).domain().get().convert_max<int>() == 100);
        ossia::value_port source; source.write_value(gain, 7);
        ossia::value_inlet sink; control(*copy, Volume).setupExecution(sink, nullptr);
        sink.data.add_port_values(source);
        REQUIRE(sink.data.get_data().size() == 1);
        CHECK(sink.data.get_data().front().value == ossia::value{gain});
        CHECK(sink.data.get_data().front().timestamp == 7);
      }
      const auto preset = model.savePreset();
      control(restored, Volume).setValue(gain == 0.f ? 1.f : 0.f);
      restored.loadPreset(preset); CHECK(restored.patch() == expected);
      for(float bad : {-1.f, Emulation::MaxVolume + .01f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        control(model, Volume).setValue(bad); CHECK(model.patch() == expected);
        CHECK(control(model, Volume).value() == ossia::value{gain});
      }
    }
  });
}
TEST_CASE("LA native preset changes are atomic and undo restores every edited slot", "[emulation][la][document]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc,QString::fromLatin1(uuid),{}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model,EditTimbre).setValue(2); control(model,partialControl(0,23)).setValue(31);
    control(model,EditTimbre).setValue(3); control(model,partialControl(1,49)).setValue(49);
    for(int part = 0; part < 8; ++part)
      control(model, partControl(part, Channel)).setValue(part + 1);
    control(model, RhythmChannel).setValue(9);
    control(model,VoiceAllocation).setValue(int(Allocation::Mono));
    const auto a = model.patch(); const auto preset = model.savePreset();
    control(model,EditTimbre).setValue(2); control(model,partialControl(0,23)).setValue(72);
    control(model, partControl(1, Channel)).setValue(16);
    control(model, partControl(0, Channel)).setValue(0);
    control(model,VoiceAllocation).setValue(int(Allocation::MidiChannel));
    control(model,Volume).setValue(0.f); const auto b = model.patch();
    int changes{};
    QObject observer;
    QObject::connect(&model,&ProcessModel::patchChanged,&observer,[&] { ++changes; });
    auto* command = ctx.interfaces<Process::LoadPresetCommandFactoryList>().make(
        &Process::LoadPresetCommandFactory::make,model,preset,doc->context());
    REQUIRE(command); CommandDispatcher<>{doc->context().commandStack}.submit(command);
    CHECK(model.patch() == a); CHECK(changes == 1);
    doc->commandStack().undo(); CHECK(model.patch() == b); CHECK(changes == 2);
    doc->commandStack().redo(); CHECK(model.patch() == a); CHECK(changes == 3);
    auto bad = preset;
    auto object = QJsonDocument::fromJson(bad.data).object(); object[QStringLiteral("Patch")] = QStringLiteral("not-base64");
    bad.data = QJsonDocument{object}.toJson(); model.loadPreset(bad);
    CHECK(model.patch() == a); CHECK(changes == 3); CHECK_FALSE(model.error().isEmpty());
    const auto directory = QFileInfo{QString::fromUtf8(__FILE__)}.absoluteDir();
    QFile file{directory.filePath(QStringLiteral("../Presets/La/Original saw.scp"))}; REQUIRE(file.open(QIODevice::ReadOnly));
    auto original = Process::Preset::fromJson(ctx.interfaces<Process::ProcessFactoryList>(),file.readAll()); REQUIRE(original);
    control(model,ControlROM).setValue(std::string{"private/control.rom"});
    const auto references = model.patch().roms;
    model.loadPreset(*original);
    CHECK(model.patch().roms == references);
    CHECK(model.patch().values[partControl(0,Bank)] == 1);
    CHECK(model.patch().values[partControl(0,Channel)] == 0);
    CHECK(model.patch().values[RhythmChannel] == 16);
    for(int part = 1; part < 8; ++part)
    {
      CHECK(model.patch().values[partControl(part, Channel)] == 16);
      CHECK(model.patch().bankPrograms[part][1] == part);
    }
    CHECK(model.patch().timbres[0] == defaultPatch().timbres[0]);
  });
}
TEST_CASE("LA unavailable engine still restores pooled stereo channels and bounds event input", "[emulation][la][execution]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true,score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    Executor::Node node{state};
    ossia::token_request token; token.prev_date = ossia::time_value{0};
    token.date = ossia::time_value{int64_t(state.bufferSize * ossia::flicks_per_second<double> / state.sampleRate)};
    token.start_sample = 0; token.length_sample = state.bufferSize;
    for(int tick = 0; tick < 3; ++tick)
    {
      ossia::clear_data{}(node.audio->data); state.samples_since_start += state.bufferSize;
      ossia::set_thread_pinned(ossia::thread_type::Audio,0); node.run(token,ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui,0);
      REQUIRE(node.audio->data.channels() == 2);
      CHECK(node.audio->data.channel(0).size() >= std::size_t(state.bufferSize));
      CHECK(std::all_of(node.audio->data.channel(0).begin(),node.audio->data.channel(0).end(),[](double v) { return v == 0.; }));
    }
    for(int i = 0; i < 8193; ++i) node.midi->data.messages.push_back(libremidi::from_midi1::note_on(0,60,100));
    state.samples_since_start += state.bufferSize;
    ossia::set_thread_pinned(ossia::thread_type::Audio,0); node.run(token,ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui,0); CHECK(node.error());
    node.midi->data.messages.clear();
    node.controls[Volume]->data.write_value(Emulation::MaxVolume + 1.f,0);
    state.samples_since_start += state.bufferSize;
    ossia::set_thread_pinned(ossia::thread_type::Audio,0); node.run(token,ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui,0);
    REQUIRE(node.error());
    node.controls[Volume]->data.get_data().clear();
    node.controls[Volume]->data.write_value(Emulation::MaxVolume,0);
    state.samples_since_start += state.bufferSize;
    ossia::set_thread_pinned(ossia::thread_type::Audio,0); node.run(token,ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui,0);
    CHECK_FALSE(node.error()); CHECK_FALSE(node.ready());
    node.controls[Volume]->data.get_data().clear();
    auto invalid = libremidi::from_midi1::note_on(0, 60, 100);
    invalid.data[0] |= 1u << 24;
    node.midi->data.messages.push_back(invalid);
    state.samples_since_start += state.bufferSize;
    ossia::set_thread_pinned(ossia::thread_type::Audio,0); node.run(token,ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui,0); REQUIRE(node.error());
    node.midi->data.messages.clear();
    node.midi->data.messages.push_back(libremidi::from_midi1::note_on(0, 60, 100));
    state.samples_since_start += state.bufferSize;
    ossia::set_thread_pinned(ossia::thread_type::Audio,0); node.run(token,ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui,0);
    CHECK_FALSE(node.error()); CHECK_FALSE(node.ready());
  });
}
TEST_CASE("LA real execution preserves gain timestamps and stops voices on native pause and seek", "[emulation][la][document][rom]")
{
  const auto* controlPath = std::getenv("SCORE_EMULATION_LA_CONTROL_ROM");
  const auto* pcmPath = std::getenv("SCORE_EMULATION_LA_PCM_ROM");
  if(!controlPath && !pcmPath) SKIP("Lawful matching LA ROM files required for native playback; no substitute engine is used.");
  REQUIRE(controlPath); REQUIRE(pcmPath);
  score::test::run_in_app([=](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true,score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    auto patch = defaultPatch(); auto refs = std::make_shared<RomReferences>();
    refs->controlPath = controlPath; refs->pcmPath = pcmPath; patch.roms = refs;
    patch.values[partControl(0,Bank)] = 1; patch.values[ReverbEnabled] = 0;
    auto node = ossia::make_node<Executor::Node>(state,state); node->edit(patch);
    auto engine = std::make_shared<Engine>(state.sampleRate,patch); auto observing = engine;
    node->install(engine); REQUIRE(node->ready());
    Emulation::ExecutionProcess process{node};
    Engine reference{double(state.sampleRate),patch};
    const std::array<uint8_t,3> note{0x90,60,100}; reference.midi(note);
    std::vector<double> left(state.bufferSize),right(state.bufferSize);
    ossia::token_request token; token.prev_date = ossia::time_value{0};
    token.date = ossia::time_value{int64_t(state.bufferSize * ossia::flicks_per_second<double> / state.sampleRate)};
    token.start_sample = 0; token.length_sample = state.bufferSize;
    double peak{};
    for(int tick = 0; tick < 48; ++tick)
    {
      ossia::clear_data{}(node->audio->data); node->midi->data.messages.clear();
      if(tick == 0) node->midi->data.messages.push_back(libremidi::from_midi1::note_on(0,60,100));
      const auto half = state.bufferSize / 2;
      node->controls[Volume]->data.get_data().clear();
      node->controls[Volume]->data.write_value(1.f,0); node->controls[Volume]->data.write_value(Emulation::MaxVolume,half);
      state.samples_since_start += state.bufferSize;
      ossia::set_thread_pinned(ossia::thread_type::Audio,0);
      node->run(token,ossia::exec_state_facade{&state}); reference.render(left.data(),right.data(),left.size());
      ossia::set_thread_pinned(ossia::thread_type::Ui,0);
      REQUIRE(node->audio->data.channels() == 2);
      for(int i = 0; i < state.bufferSize; ++i)
      {
        CHECK(node->audio->data.channel(0)[i] == left[i] * (i < half ? 1. : Emulation::MaxVolume));
        peak = std::max(peak,std::abs(left[i]));
      }
    }
    CHECK(peak > .0001); CHECK(observing->active());
    ossia::set_thread_pinned(ossia::thread_type::Audio,0); process.pause();
    ossia::set_thread_pinned(ossia::thread_type::Ui,0); CHECK_FALSE(observing->active());
    std::array<uint8_t,246> restored{}; observing->readMemory(0x20000,restored);
    CHECK(restored == patch.timbres[0]);
    ossia::set_thread_pinned(ossia::thread_type::Audio,0); process.transport_impl(ossia::time_value{0});
    ossia::set_thread_pinned(ossia::thread_type::Ui,0); CHECK_FALSE(observing->active());
  });
}

TEST_CASE("LA ROM references are visible read-only assets and survive consolidation and Save As", "[emulation][la][document][assets]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    QTemporaryDir originalDir, destinationDir;
    REQUIRE(originalDir.isValid()); REQUIRE(destinationDir.isValid());
    const auto originalRoot = score::test::canonical(originalDir.path());
    const auto destinationRoot = score::test::canonical(destinationDir.path());
    const QString controlPath = originalRoot + QStringLiteral("/control.rom");
    const QString pcmPath = originalRoot + QStringLiteral("/pcm.rom");
    const QByteArray controlBytes{"Not a ROM: control asset policy regression"};
    const QByteArray pcmBytes{"Not a ROM: PCM asset policy regression"};
    score::test::write_file(controlPath,controlBytes);
    score::test::write_file(pcmPath,pcmBytes);
    auto* doc = score::test::project_document(ctx,originalRoot); REQUIRE(doc);
    auto* base = score::test::add_process(*doc,QString::fromLatin1(uuid),{}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model,ControlROM).setValue(std::string{"<PROJECT>:control.rom"});
    control(model,PcmROM).setValue(std::string{"pcm.rom"});
    CHECK(model.patch().roms->controlPath == controlPath.toStdString());
    CHECK(model.patch().roms->pcmPath == pcmPath.toStdString());
    CHECK(score::test::control_string(control(model,ControlROM)) == controlPath);
    CHECK(score::test::control_string(control(model,PcmROM)) == pcmPath);
    auto identity = *model.patch().roms;
    identity.controlHash = QCryptographicHash::hash(controlBytes,QCryptographicHash::Sha1).toHex().toStdString();
    identity.pcmHash = QCryptographicHash::hash(pcmBytes,QCryptographicHash::Sha1).toHex().toStdString();
    model.acceptROMIdentity(identity,{});
    auto expected = model.patch();
    std::vector<Process::ExternalFileRef> references;
    Process::ExternalFileMap map{[&](const Process::ExternalFileRef& ref) -> QString {
      references.push_back(ref);
      return destinationRoot + QStringLiteral("/must-not-repoint.rom");
    }};
    model.mapExternalFiles(map);
    REQUIRE(references.size() == 2);
    for(const auto& ref : references) CHECK_FALSE(ref.rewritable);
    CHECK_FALSE(map.hasCommands()); CHECK(model.patch() == expected);

    const auto analysis = Process::analyzeProjectFiles(doc->context(),{},destinationRoot);
    const auto collected = Process::consolidateProjectFiles(doc->context(),{},destinationRoot);
    for(const auto* report : {&analysis,&collected})
    {
      for(const auto& path : {controlPath,pcmPath})
      {
        const auto* entry = score::test::entry_for(*report,path); REQUIRE(entry);
        CHECK(entry->action == Process::FileAction::Unsupported);
        CHECK_FALSE(entry->copyNeeded); CHECK(entry->destinationPath.isEmpty());
        CHECK(entry->newStoredPath.isEmpty());
      }
      CHECK(report->bytesToCopy() == 0);
    }
    CHECK(QDir{destinationRoot}.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    CHECK(model.patch() == expected);
    CHECK(Process::reanchorProjectFiles(doc->context()) == 0);

    // Legacy absolute or relative preset references never replace the local
    // setup; their hashes remain sound provenance, independent of Save As.
    auto preset = model.savePreset();
    auto object = QJsonDocument::fromJson(preset.data).object();
    auto relativePatch = expected;
    auto relativeRefs = std::make_shared<RomReferences>(*expected.roms);
    relativeRefs->controlPath = "<PROJECT>:control.rom"; relativeRefs->pcmPath = "pcm.rom";
    relativePatch.roms = relativeRefs;
    const auto encoded = encodePatch(relativePatch);
    const QByteArray payload{reinterpret_cast<const char*>(encoded.data()),qsizetype(encoded.size())};
    object[QStringLiteral("Patch")] = QString::fromLatin1(payload.toBase64());
    preset.data = QJsonDocument{object}.toJson();
    expected.soundROMHashes = std::make_shared<const RomHashes>(RomHashes{expected.roms->controlHash, expected.roms->pcmHash});
    model.loadPreset(preset); CHECK(model.patch() == expected);
    REQUIRE(ctx.docManager.saveDocumentAs(*doc,destinationRoot + QStringLiteral("/saved-as.score")));
    CHECK(model.patch() == expected);
    for(const auto& path : {model.patch().roms->controlPath,model.patch().roms->pcmPath})
      CHECK(score::locateFilePath(QString::fromStdString(path),score::pathRoots(doc->context())) == QString::fromStdString(path));
    CHECK_FALSE(QFileInfo::exists(destinationRoot + QStringLiteral("/control.rom")));
    CHECK_FALSE(QFileInfo::exists(destinationRoot + QStringLiteral("/pcm.rom")));
    REQUIRE(QFile::remove(pcmPath));
    const auto missing = Process::scanMissingFiles(doc->context());
    const auto* missingEntry = score::test::entry_for(missing,pcmPath); REQUIRE(missingEntry);
    // Host policy classifies non-rewritable references as Unsupported, including
    // absent ones. Keep their missing status explicit in the visible owner label.
    CHECK(missingEntry->action == Process::FileAction::Unsupported);
    CHECK(missingEntry->owner.contains(QStringLiteral("missing")));
    REQUIRE(score::test::entry_for(missing,controlPath));
    CHECK(model.patch() == expected);
  });
}

TEST_CASE("LA named hardware selection remains integer-valued and transient across documents", "[emulation][la][document][rom][catalog]")
{
  const auto* controlPath = std::getenv("SCORE_EMULATION_LA_CONTROL_ROM");
  const auto* pcmPath = std::getenv("SCORE_EMULATION_LA_PCM_ROM");
  if(!controlPath && !pcmPath) SKIP("Lawful matching LA ROMs required to qualify actual program names and selection.");
  REQUIRE(controlPath); REQUIRE(pcmPath);
  score::test::run_in_app([=](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc,QString::fromLatin1(uuid),{}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model,ControlROM).setValue(std::string{controlPath});
    control(model,PcmROM).setValue(std::string{pcmPath});
    QFile presetFile{QFileInfo{QString::fromUtf8(__FILE__)}.absoluteDir().filePath(QStringLiteral("../Presets/La/Original saw.scp"))};
    REQUIRE(presetFile.open(QIODevice::ReadOnly));
    auto preset = Process::Preset::fromJson(ctx.interfaces<Process::ProcessFactoryList>(),presetFile.readAll()); REQUIRE(preset);
    model.loadPreset(*preset);
    Engine prepared{48000.,model.patch()};
    model.acceptROMIdentity(prepared.resolvedROMs(),QString::fromStdString(prepared.machine()),prepared.programCatalog());
    // The supplied original preset must play the default score MIDI channel,
    // independently of hardware's factory receive-channel assignments.
    const std::array<uint8_t,3> note{0x90,60,100}; prepared.midi(note);
    std::array<double,4096> left{},right{}; prepared.render(left.data(),right.data(),left.size());
    CHECK(std::any_of(left.begin(),left.end(),[](double n) { return std::abs(n) > .0001; }));
    prepared.panic();
    control(model,partControl(0,Bank)).setValue(0);
    auto* selector = qobject_cast<Process::ComboBox*>(&control(model,partControl(0,Program))); REQUIRE(selector);
    const auto& catalog = prepared.programCatalog(); REQUIRE(catalog);
    int chosen = 127;
    while(chosen > 0 && catalog->factory[chosen].empty()) --chosen;
    REQUIRE_FALSE(catalog->factory[chosen].empty());
    REQUIRE(selector->alternatives.size() == 128);
    const auto selectedValue = selector->alternatives[chosen].second;
    REQUIRE(selectedValue.get_type() == ossia::val_type::INT);
    CHECK(selector->alternatives[chosen].first.contains(QString::fromStdString(catalog->factory[chosen])));
    selector->setValue(selectedValue);
    REQUIRE(prepared.setPatch(model.patch()));
    std::array<uint8_t,10> selectedName{}; prepared.readMemory(0x10000,selectedName);
    CHECK(QString::fromLatin1(reinterpret_cast<const char*>(selectedName.data()),10).trimmed()
        == QString::fromStdString(catalog->factory[chosen]));
    const auto expected = model.patch();
    const auto liveAlternatives = selector->alternatives;
    const auto json = toValue(score::marshall<JSONObject>(*base));
    CHECK(selector->alternatives == liveAlternatives); // serialization never mutates the live selector
    JSONObject::Deserializer reader{json}; ProcessModel restored{reader,nullptr};
    REQUIRE(restored.patch() == expected);
    auto* restoredSelector = qobject_cast<Process::ComboBox*>(&control(restored,partControl(0,Program))); REQUIRE(restoredSelector);
    CHECK(restoredSelector->alternatives[chosen].first == QString::number(chosen)); // no stale ROM-name cache
    CHECK(restoredSelector->value() == selectedValue);
    QByteArray bytes; DataStream::Serializer binaryWriter{&bytes};
    TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(binaryWriter,model);
    binaryWriter.read(static_cast<const Process::ProcessModel&>(model));
    binaryWriter.read(model);
    DataStream::Deserializer binaryReader{bytes}; ProcessModel binary{binaryReader,nullptr};
    REQUIRE(binary.patch() == expected);
    auto* binarySelector = qobject_cast<Process::ComboBox*>(&control(binary,partControl(0,Program))); REQUIRE(binarySelector);
    CHECK(binarySelector->alternatives[chosen].first == QString::number(chosen));
    CHECK(binarySelector->value() == selectedValue);
    restored.acceptROMIdentity(prepared.resolvedROMs(),QString::fromStdString(prepared.machine()),catalog);
    CHECK(restoredSelector->alternatives[chosen] == liveAlternatives[chosen]);
    CHECK(restored.patch() == expected);
    control(restored,ControlROM).setValue(std::string{});
    CHECK(restoredSelector->alternatives[chosen].first == QString::number(chosen));
    CHECK(restoredSelector->value() == selectedValue);
  });
}

TEST_CASE("LA native bank undo retains destination programs and the selected memory target", "[emulation][la][document]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model, partControl(0, Program)).setValue(100);
    const auto changeBank = [&](int bank) {
      CommandDispatcher<>{doc->context().commandStack}.submit(
          new Process::SetControlValue{control(model, partControl(0, Bank)), bank});
    };
    changeBank(1);
    CHECK(model.patch().values[partControl(0, Program)] == 0);
    CHECK(ossia::convert<int>(control(model, partControl(0, Program)).value()) == 0);
    control(model, partControl(0, Program)).setValue(23);
    REQUIRE(model.patch().values[EditTimbre] == 23);
    control(model, partialControl(0, 23)).setValue(37);
    changeBank(0);
    CHECK(model.patch().values[partControl(0, Program)] == 100);
    doc->commandStack().undo();
    CHECK(model.patch().values[partControl(0, Bank)] == 1);
    CHECK(model.patch().values[partControl(0, Program)] == 23);
    CHECK(model.patch().values[EditTimbre] == 23);
    CHECK(model.patch().timbres[23][14 + 23] == 37);
    doc->commandStack().redo();
    CHECK(model.patch().values[partControl(0, Program)] == 100);
    control(model, partControl(0, Bank)).setValue(1);
    const auto preset = model.savePreset();
    control(model, partControl(0, Bank)).setValue(0);
    control(model, partControl(0, Program)).setValue(7);
    model.loadPreset(preset);
    control(model, partControl(0, Bank)).setValue(0);
    CHECK(model.patch().values[partControl(0, Program)] == 100);
    control(model, partControl(1, Bank)).setValue(1);
    control(model, partControl(1, Program)).setValue(12);
    CHECK(model.patch().values[EditTimbre] == 23);
  });
}

TEST_CASE("LA MIDI and centered labels retain native automation values through old port migration", "[emulation][la][document]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    const int channel = partControl(0, Channel);
    Process::IntSlider legacy{0, 16, 1, QStringLiteral("Part 1 Channel"), Id<Process::Port>{channel + 1}, nullptr};
    legacy.setExposed(QStringLiteral("my-midi-routing"));
    legacy.setDescription(QStringLiteral("Authored routing"));
    legacy.setScriptable(false);
    const auto check = [&](ProcessModel& restored) {
      auto* selector = qobject_cast<Process::ComboBox*>(&control(restored, channel)); REQUIRE(selector);
      CHECK(selector->exposed() == legacy.exposed());
      CHECK(selector->description() == legacy.description());
      CHECK(selector->scriptable() == legacy.scriptable());
      CHECK(selector->alternatives[0].first == QStringLiteral("1"));
      CHECK(selector->alternatives[15].first == QStringLiteral("16"));
      CHECK(selector->alternatives[16].first == QStringLiteral("Off"));
      for(int value : {0, 15, 16})
      {
        selector->setValue(selector->alternatives[value].second);
        CHECK(restored.patch().values[channel] == value);
      }
      auto* tuning = qobject_cast<Process::ComboBox*>(&control(restored, partControl(0, FineTune))); REQUIRE(tuning);
      CHECK(tuning->alternatives[50].first == QStringLiteral("0 cents"));
      tuning->setValue(tuning->alternatives[0].second);
      CHECK(restored.patch().values[partControl(0, FineTune)] == 0);
    };
    auto json = toValue(score::marshall<JSONObject>(*base));
    auto oldPort = toValue(score::marshall<JSONObject>(static_cast<const Process::Inlet&>(legacy)));
    json["Inlets"][channel + 1].CopyFrom(oldPort, json.GetAllocator());
    JSONObject::Deserializer reader{json}; ProcessModel restored{reader, nullptr}; check(restored);
    auto ports = model.inlets(); ports[channel + 1] = &legacy;
    const auto encoded = encodePatch(model.patch());
    const QByteArray patch{reinterpret_cast<const char*>(encoded.data()), qsizetype(encoded.size())};
    QByteArray bytes; DataStream::Serializer writer{&bytes};
    TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
    writer.read(static_cast<const Process::ProcessModel&>(model));
    writer.m_stream << quint32{1} << QStringLiteral("munt") << patch;
    readPorts(writer, ports, model.outlets()); writer.insertDelimiter();
    DataStream::Deserializer binaryReader{bytes}; ProcessModel binary{binaryReader, nullptr}; check(binary);
  });
}

TEST_CASE("LA partial enables preserve other bits and timbres through native undo", "[emulation][la][document]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model, EditTimbre).setValue(7);
    auto& mask = control(model, TimbreBase + 2);
    mask.setValue(5);
    const auto authored = model.patch().timbres;
    CommandDispatcher<>{doc->context().commandStack}.submit(new Process::SetControlValue{mask, 5 | 2});
    CHECK(model.patch().timbres[7][12] == 7);
    doc->commandStack().undo();
    CHECK(ossia::convert<int>(mask.value()) == 5);
    CHECK(model.patch().timbres == authored);
    doc->commandStack().redo();
    CHECK(ossia::convert<int>(mask.value()) == 7);
    control(model, EditTimbre).setValue(0);
    CHECK(ossia::convert<int>(mask.value()) == authored[0][12]);
    control(model, EditTimbre).setValue(7);
    CHECK(ossia::convert<int>(mask.value()) == 7);
  });
}

TEST_CASE("LA presets retain local resources and successful edits cannot clear readiness failures", "[emulation][la][document]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model, ControlROM).setValue(std::string{"local/control.rom"});
    control(model, PcmROM).setValue(std::string{"local/pcm.rom"});
    auto local = *model.patch().roms;
    local.controlHash = std::string(40, 'a'); local.pcmHash = std::string(40, 'b');
    model.acceptROMIdentity(local, {});
    const auto refs = model.patch().roms;
    const QString missing = QStringLiteral("Cannot open local PCM ROM");
    model.setEngineError(missing);
    control(model, partControl(0, Program)).setValue(100);
    CHECK(model.error().contains(missing));
    control(model, Volume).setValue(2.f);
    control(model, Volume).setValue(.5f);
    CHECK(model.error() == missing);
    auto preset = model.savePreset();
    auto json = QJsonDocument::fromJson(preset.data).object();
    const auto data = QByteArray::fromBase64(json[QStringLiteral("Patch")].toString().toLatin1());
    auto sound = decodePatch({reinterpret_cast<const uint8_t*>(data.constData()), std::size_t(data.size())});
    CHECK(sound.roms->controlPath.empty()); CHECK(sound.roms->pcmPath.empty());
    CHECK(sound.roms->controlHash == local.controlHash);
    auto foreign = std::make_shared<RomReferences>(*sound.roms);
    foreign->controlPath = "someone-else/control.rom"; foreign->pcmPath = "someone-else/pcm.rom";
    foreign->controlHash = std::string(40, 'c'); sound.roms = foreign;
    const auto encoded = encodePatch(sound);
    json[QStringLiteral("Schema")] = 1; // Legacy preset with embedded references, no KeepROMs escape hatch.
    json[QStringLiteral("Patch")] = QString::fromLatin1(QByteArray{reinterpret_cast<const char*>(encoded.data()), qsizetype(encoded.size())}.toBase64());
    preset.data = QJsonDocument{json}.toJson(); model.loadPreset(preset);
    CHECK(model.patch().roms == refs);
    CHECK(model.error().contains(missing));
    model.setEngineError({});
    CHECK_FALSE(model.error().isEmpty()); // Truthful incompatible sound identity, not a new ROM selection.
    model.loadPreset(model.savePreset());
    CHECK(model.patch().roms == refs);
    CHECK_FALSE(model.error().isEmpty());
  });
}

TEST_CASE("LA retries ROM preparation only for explicit resource commands including same-path reselection", "[emulation][la][document]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    int requests{};
    QObject observer;
    QObject::connect(&model, &ProcessModel::romPreparationRequested, &observer, [&] { ++requests; });
    const auto choose = [&](int inlet, ossia::value value) {
      CommandDispatcher<>{doc->context().commandStack}.submit(
          new Process::SetControlValue{control(model, inlet), std::move(value)});
    };
    choose(ControlROM, std::string{"missing/control.rom"});
    choose(PcmROM, std::string{"missing/pcm.rom"});
    REQUIRE(requests == 2);
    const QString failure = QStringLiteral("Cannot open LA PCM ROM");
    model.setEngineError(failure);
    const auto preset = model.savePreset();
    choose(Volume, .5f);
    choose(EditTimbre, 5);
    choose(partialControl(0, 23), 40);
    choose(partControl(0, Bank), 1);
    model.loadPreset(preset);
    CHECK(requests == 2);
    CHECK(model.error() == failure);
    // This is the same SetControlValue command used by the native file chooser.
    // An unchanged path still requests exactly one retry; syncControls does not.
    choose(PcmROM, control(model, PcmROM).value());
    CHECK(requests == 3);
    CHECK(model.error() == failure);
    doc->commandStack().undo();
    CHECK(requests == 4);
    doc->commandStack().redo();
    CHECK(requests == 5);
    const auto checkRestored = [&](ProcessModel& restored) {
      int restoredRequests{};
      QObject receiver;
      QObject::connect(&restored, &ProcessModel::romPreparationRequested, &receiver, [&] { ++restoredRequests; });
      const auto samePath = control(restored, PcmROM).value();
      control(restored, EditTimbre).setValue(9);
      CHECK(restoredRequests == 0);
      control(restored, PcmROM).setValue(samePath);
      CHECK(restoredRequests == 1);
    };
    const auto json = toValue(score::marshall<JSONObject>(*base));
    JSONObject::Deserializer reader{json}; ProcessModel restored{reader, nullptr}; checkRestored(restored);
    QByteArray bytes; DataStream::Serializer writer{&bytes};
    TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
    writer.read(static_cast<const Process::ProcessModel&>(model)); writer.read(model);
    DataStream::Deserializer binaryReader{bytes}; ProcessModel binary{binaryReader, nullptr}; checkRestored(binary);
  });
}

TEST_CASE("LA same-timestamp bank and program automation preserves both destinations regardless of enqueue order", "[emulation][la][execution][rom]")
{
  const auto* controlPath = std::getenv("SCORE_EMULATION_LA_CONTROL_ROM");
  const auto* pcmPath = std::getenv("SCORE_EMULATION_LA_PCM_ROM");
  if(!controlPath && !pcmPath) SKIP("Lawful matching LA ROM files required to observe native automation selection.");
  REQUIRE(controlPath); REQUIRE(pcmPath);
  score::test::run_in_app([=](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc)); drain(plug);
    auto& state = *plug.context().execState;
    auto patch = defaultPatch();
    auto refs = std::make_shared<RomReferences>();
    refs->controlPath = controlPath; refs->pcmPath = pcmPath; patch.roms = refs;
    REQUIRE(setControl(patch, partControl(0, Program), 10));
    REQUIRE(setControl(patch, partControl(0, Bank), 1));
    REQUIRE(setControl(patch, partControl(0, Program), 7));
    REQUIRE(setControl(patch, partControl(0, Bank), 0));
    Executor::Node node{state}; node.edit(patch);
    auto prepared = std::make_shared<Engine>(state.sampleRate, patch), observed = prepared;
    node.install(prepared); REQUIRE(node.ready());
    ossia::token_request token;
    token.prev_date = ossia::time_value{0};
    token.date = ossia::time_value{int64_t(state.bufferSize * ossia::flicks_per_second<double> / state.sampleRate)};
    token.start_sample = 0; token.length_sample = state.bufferSize;
    const auto bank = partControl(0, Bank), program = partControl(0, Program);
    const auto run = [&] {
      state.samples_since_start += state.bufferSize;
      ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
      node.run(token, ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
      node.controls[bank]->data.get_data().clear();
      node.controls[program]->data.get_data().clear();
      REQUIRE(node.ready()); CHECK_FALSE(node.error());
    };
    node.controls[program]->data.write_value(23, 0);
    node.controls[bank]->data.write_value(1, 0);
    run();
    CHECK(observed->patch().values[bank] == 1);
    CHECK(observed->patch().values[program] == 23);
    node.controls[bank]->data.write_value(0, 0);
    run();
    CHECK(observed->patch().values[program] == 10);
    node.controls[program]->data.write_value(24, 0); // No bank event: edit factory, not a future destination.
    run();
    CHECK(observed->patch().values[program] == 24);
    node.controls[bank]->data.write_value(1, 0);
    run();
    CHECK(observed->patch().values[program] == 23);
    node.controls[program]->data.write_value(52, 0);
    node.controls[bank]->data.write_value(0, 0);
    run();
    CHECK(observed->patch().values[program] == 52);
    node.controls[bank]->data.write_value(1, 0);
    node.controls[program]->data.write_value(31, 0); // Opposite enqueue order has identical destination semantics.
    run();
    CHECK(observed->patch().values[program] == 31);
    node.controls[program]->data.write_value(100, 0);
    node.controls[bank]->data.write_value(0, 0);
    run();
    CHECK(observed->patch().values[program] == 100);
    node.controls[bank]->data.write_value(1, 0);
    run();
    CHECK(observed->patch().values[program] == 31);
  });
}

TEST_CASE("LA native layer tabs select editing without activating parts and enable owns the receive port", "[emulation][la][document][gui]")
{
  score::test::run_in_gui_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    auto* presenter = score::IDocument::try_presenterDelegate<Scenario::ScenarioDocumentPresenter>(*doc);
    REQUIRE(presenter);
    auto* factory = presenter->context().processList.findDefaultFactory(model); REQUIRE(factory);
    QGraphicsScene scene;
    std::unique_ptr<score::ResizeableItem> item{factory->makeItem(model, presenter->context(), nullptr)};
    REQUIRE(item); scene.addItem(item.get());
    score::GraphicsTabLayout* layers{};
    Dataflow::PortItem* cutoff{};
    Dataflow::PortItem* program{};
    Dataflow::PortItem* allocation{};
    for(auto* child : scene.items())
    {
      if(auto* tabs = dynamic_cast<score::GraphicsTabLayout*>(child); tabs && tabs->onCurrentIndexChanged)
        layers = tabs;
      if(auto* port = dynamic_cast<Dataflow::PortItem*>(child))
      {
        if(&port->port() == &control(model, partialControl(0, 23))) cutoff = port;
        if(&port->port() == &control(model, partControl(1, Program))) program = port;
        if(&port->port() == &control(model, VoiceAllocation)) allocation = port;
      }
    }
    REQUIRE(layers); REQUIRE(cutoff); REQUIRE(program);
    REQUIRE(allocation);
    CHECK(allocation->isVisible());
    CHECK(allocation->isEnabled());
    CHECK_FALSE(cutoff->isVisible()); // Factory patches never expose an unrelated memory editor.
    CHECK_FALSE(cutoff->isEnabled());
    const auto initial = model.patch();
    layers->setCurrentIndex(1); layers->onCurrentIndexChanged(1);
    CHECK(model.patch() == initial); // Editing a disabled factory layer is not activation.
    auto& channel = control(model, partControl(1, Channel));
    score::QGraphicsToggle* enable{};
    for(auto* child : program->parentItem()->parentItem()->childItems())
      if(auto* toggle = dynamic_cast<score::QGraphicsToggle*>(child)) enable = toggle;
    REQUIRE(enable); CHECK_FALSE(enable->state());
    enable->toggled(true);
    CHECK(model.patch().values[partControl(1, Channel)] == 0);
    CHECK(enable->state());
    doc->commandStack().undo();
    CHECK(model.patch().values[partControl(1, Channel)] == 16);
    CHECK_FALSE(enable->state());
    doc->commandStack().redo();
    CHECK(model.patch().values[partControl(1, Channel)] == 0);
    channel.setValue(12); // Authored advanced routing remains the only authority.
    CHECK(enable->state());
    enable->toggled(false);
    CHECK(model.patch().values[partControl(1, Channel)] == 16);
    doc->commandStack().undo();
    CHECK(model.patch().values[partControl(1, Channel)] == 12);
    CHECK(enable->state());
    control(model, partControl(1, Bank)).setValue(1);
    CHECK(model.patch().values[partControl(1, Program)] == 1);
    CHECK(cutoff->isEnabled());
    CHECK(model.patch().values[EditTimbre] == 1);
    control(model, partialControl(0, 23)).setValue(37);
    CHECK(model.patch().timbres[1][14 + 23] == 37);
    CHECK(model.patch().timbres[0][14 + 23] == initial.timbres[0][14 + 23]);
    const auto authored = model.patch();
    layers->setCurrentIndex(0); layers->onCurrentIndexChanged(0);
    CHECK_FALSE(cutoff->isVisible());
    CHECK(model.patch() == authored);
    layers->setCurrentIndex(1); layers->onCurrentIndexChanged(1);
    CHECK(model.patch().values[partControl(1, Channel)] == 12);
    control(model, partControl(1, Program)).setValue(7);
    CHECK(model.patch().values[EditTimbre] == 7);
    CHECK(ossia::convert<int>(control(model, partialControl(0, 23)).value()) == initial.timbres[7][14 + 23]);
    control(model, partControl(2, Bank)).setValue(1);
    control(model, partControl(2, Program)).setValue(5);
    control(model, EditTimbre).setValue(5);
    CHECK(layers->currentIndex() == 2);
    const auto stored = model.patch();
    const auto preset = model.savePreset();
    layers->setCurrentIndex(1); layers->onCurrentIndexChanged(1);
    const auto beforeLoad = model.patch();
    CHECK(beforeLoad.timbres == stored.timbres);
    CHECK(beforeLoad.bankPrograms == stored.bankPrograms);
    for(int c = 0; c < TimbreBase; ++c)
      if(c != EditTimbre) CHECK(beforeLoad.values[c] == stored.values[c]);
    auto* load = ctx.interfaces<Process::LoadPresetCommandFactoryList>().make(
        &Process::LoadPresetCommandFactory::make, model, preset, doc->context());
    REQUIRE(load); CommandDispatcher<>{doc->context().commandStack}.submit(load);
    CHECK(model.patch() == stored); CHECK(layers->currentIndex() == 2);
    CHECK(model.patch().values[partControl(2, Channel)] == 16);
    doc->commandStack().undo();
    CHECK(model.patch() == beforeLoad); CHECK(layers->currentIndex() == 1);
    doc->commandStack().redo();
    CHECK(model.patch() == stored); CHECK(layers->currentIndex() == 2);
    control(model, partControl(2, Program)).setValue(63);
    CHECK(model.patch().values[EditTimbre] == 63);
    CHECK(model.patch().values[partControl(2, Channel)] == 16);
  });
}

TEST_CASE("LA allocation is a persisted automatable control with native undo", "[emulation][la][document][allocation]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    auto* allocation = qobject_cast<Process::ComboBox*>(&control(model, VoiceAllocation));
    REQUIRE(allocation);
    CHECK(allocation->id().val() == VoiceAllocation + 1);
    CHECK(model.patch().allocation == Allocation::Poly);
    REQUIRE(allocation->alternatives.size() == 3);
    CHECK(allocation->alternatives[0].first == QStringLiteral("Mono"));
    CHECK(allocation->alternatives[1].first == QStringLiteral("Poly"));
    CHECK(allocation->alternatives[2].first == QStringLiteral("MIDI channel"));
    const auto routes = model.patch().values;
    for(int mode : {0, 2, 1})
    {
      const auto previous = model.patch().allocation;
      CommandDispatcher<>{doc->context().commandStack}.submit(new Process::SetControlValue{*allocation, mode});
      CHECK(model.patch().allocation == Allocation(mode));
      CHECK(model.patch().values == routes);
      doc->commandStack().undo();
      CHECK(model.patch().allocation == previous);
      doc->commandStack().redo();
      CHECK(model.patch().allocation == Allocation(mode));
      ossia::value_port source;
      source.write_value(mode, 17);
      ossia::value_inlet sink;
      allocation->setupExecution(sink, nullptr);
      sink.data.add_port_values(source);
      REQUIRE(sink.data.get_data().size() == 1);
      CHECK(sink.data.get_data().front().value == ossia::value{mode});
      CHECK(sink.data.get_data().front().timestamp == 17);
    }
  });
}

TEST_CASE("LA legacy documents append allocation without renumbering authored routes", "[emulation][la][document][allocation]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    auto* base = score::test::add_process(*doc, QString::fromLatin1(uuid), {}); REQUIRE(base);
    auto& model = static_cast<ProcessModel&>(*base);
    control(model, partControl(0, Channel)).setValue(7);
    control(model, partControl(1, Channel)).setValue(7);
    control(model, RhythmChannel).setValue(7);
    auto encoded = encodePatch(model.patch());
    encoded[3] = '2';
    encoded.resize(encoded.size() - 4);
    const QByteArray patch{reinterpret_cast<const char*>(encoded.data()), qsizetype(encoded.size())};
    auto json = toValue(score::marshall<JSONObject>(*base));
    json["Inlets"].PopBack();
    const auto base64 = patch.toBase64();
    json["Patch"].SetString(base64.constData(), base64.size(), json.GetAllocator());
    JSONObject::Deserializer reader{json};
    ProcessModel restored{reader, nullptr};
    auto ports = model.inlets(); ports.pop_back();
    QByteArray bytes;
    DataStream::Serializer writer{&bytes};
    TSerializer<DataStream, score::Entity<Process::ProcessModel>>::readFrom(writer, model);
    writer.read(static_cast<const Process::ProcessModel&>(model));
    writer.m_stream << quint32{1} << QStringLiteral("munt") << patch;
    readPorts(writer, ports, model.outlets()); writer.insertDelimiter();
    DataStream::Deserializer binaryReader{bytes};
    ProcessModel binary{binaryReader, nullptr};
    for(auto* copy : {&restored, &binary})
    {
      CHECK(copy->patch() == model.patch());
      REQUIRE(copy->inlets().size() == ControlCount + 1);
      CHECK(control(*copy, VoiceAllocation).value() == ossia::value{1});
      CHECK(control(*copy, partControl(1, Channel)).id().val() == partControl(1, Channel) + 1);
      control(*copy, VoiceAllocation).setValue(0);
      CHECK(copy->patch().allocation == Allocation::Mono);
      CHECK(copy->patch().values == model.patch().values);
    }
  });
}
