// SPDX-License-Identifier: GPL-3.0-or-later
#include <Process/ExecutionTransaction.hpp>
#include <Process/ApplicationPlugin.hpp>
#include <Process/Commands/SetControlValue.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <Process/Dataflow/PortItem.hpp>
#include <Process/Dataflow/NodeItem.hpp>
#include <Process/Preset.hpp>
#include <Process/ProcessContext.hpp>
#include <Process/ProcessList.hpp>
#include <Dataflow/WaveformInlet.hpp>
#include <Dataflow/Commands/EditConnection.hpp>
#include <Emulation/Apu/Controls.hpp>
#include <Emulation/Volume.hpp>
#include <Emulation/Waveform.hpp>
#include <Emulation/Apu/Executor/Node.hpp>
#include <Emulation/Apu/ProcessModel.hpp>
#include <Execution/BaseScenarioComponent.hpp>
#include <Execution/DocumentPlugin.hpp>
#include <Scenario/Commands/LoadPresetCommand.hpp>
#include <Scenario/Document/Interval/IntervalExecution.hpp>
#include <Scenario/Document/ScenarioDocument/ScenarioDocumentPresenter.hpp>
#include <score/document/DocumentInterface.hpp>
#include <score/graphics/RectItem.hpp>
#include <score/graphics/layouts/GraphicsTabLayout.hpp>
#include <score/graphics/widgets/QGraphicsEnum.hpp>
#include <score/graphics/widgets/QGraphicsTextButton.hpp>
#include <score/graphics/widgets/QGraphicsSelectablePixmapToggle.hpp>
#include <score/command/Dispatchers/CommandDispatcher.hpp>
#include <core/command/CommandStack.hpp>
#include <score/model/EntitySerialization.hpp>
#include <score/serialization/VisitorCommon.hpp>
#include <ossia/dataflow/execution_state.hpp>
#include <ossia/dataflow/data_copy.hpp>
#include <libremidi/ump_events.hpp>
#include <catch2/catch_test_macros.hpp>
#include <score_test/App.hpp>
#include <score_test/Events.hpp>
#include <score_test/Mouse.hpp>
#include <score_test/Project.hpp>
#include <QEventLoop>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QAction>
#include <QMenu>
#include <QTemporaryFile>
#include <QTimer>
#include <algorithm>
#include <stdexcept>
#include <limits>
namespace {
using namespace Emulation::Apu;
constexpr auto uuid="28b1e0ab-e245-4a25-8fb6-d05e43bf6f08";
Process::ControlInlet& control(ProcessModel& p,int c) {return *static_cast<Process::ControlInlet*>(p.inlets()[c+1]);}
Dataflow::WaveformInlet& waveInlet(ProcessModel& p) {
  auto* inlet=dynamic_cast<Dataflow::WaveformInlet*>(&control(p,Wave));REQUIRE(inlet);return *inlet;
}
ossia::value authoredWave() {
  std::array<uint8_t,32> samples{};
  for(int i=0;i<32;++i)samples[i]=(i*7)%16;
  return Emulation::Waveform::fromSamples(samples,0,15);
}
void checkAuthoredWave(ProcessModel& p,const ossia::value& value) {
  auto& inlet=waveInlet(p);CHECK(inlet.value()==value);
  CHECK(inlet.frameSizeConstraint()==32);CHECK(inlet.amplitudeLevels()==16);
  REQUIRE(inlet.wavetable().frameSize==32);REQUIRE(inlet.wavetable().frameCount()==1);
  for(int i=0;i<32;++i)
    CHECK(p.settings().wave[i]==std::lround(15.*(double(inlet.wavetable().samples[i])+1.)/2.));
}
void drain(Execution::DocumentPlugin& plug) {
  const auto& ctx=plug.context();Execution::ExecutionCommand command;
  ossia::set_thread_pinned(ossia::thread_type::Audio,0);
  while(ctx.executionQueue.try_dequeue(command)){command();ctx.gcQueue.enqueue(Execution::gc(std::move(command)));}
  ossia::set_thread_pinned(ossia::thread_type::Ui,0);
  Execution::GCCommand retired;while(ctx.gcQueue.try_dequeue(retired))retired();
}
ossia::token_request tokenFor(const ossia::execution_state& state) {
  ossia::token_request t;t.prev_date=ossia::time_value{0};
  t.date=ossia::time_value{int64_t(state.bufferSize*ossia::flicks_per_second<double>/state.sampleRate)};
  t.start_sample=0;t.length_sample=state.bufferSize;return t;
}
}
TEST_CASE("APU semantic document state embeds all native roles wave and DMC", "[emulation][apu][document]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);
    auto* base=score::test::add_process(*doc,QString::fromLatin1(uuid),{});REQUIRE(base);
    auto& p=static_cast<ProcessModel&>(*base);
    QByteArray bytes(16384,'\0');for(int i=0;i<bytes.size();++i)bytes[i]=char(i*13);
    REQUIRE(p.importDmcBytes(bytes));
    for(int model=0;model<4;++model) {
      control(p,ChipModel).setValue(model);control(p,Clock).setValue(0);control(p,Volume).setValue(.25f);
      control(p,Routing).setValue(model%2);
      control(p,Sound).setValue(model<2?4:3);
      control(p,roleControl(0,SweepShift)).setValue(3);control(p,roleControl(0,EnvelopeEnable)).setValue(true);
      control(p,roleControl(0,Length)).setValue(model<2?31:40);
      control(p,roleControl(0,EnvelopePeriod)).setValue(model<2?15:7);
      const auto wave=authoredWave();control(p,Wave).setValue(wave);checkAuthoredWave(p,wave);
      REQUIRE(p.error().isEmpty());
      // Older documents serialize their port metadata as well as Routing=0/1.
      // Loading must preserve that route, replacing the choices in place rather
      // than appending a new allocation port or introducing a hardware Poly.
      auto* oldRouting=qobject_cast<Process::ComboBox*>(&control(p,Routing));REQUIRE(oldRouting);
      oldRouting->setName(QStringLiteral("Routing"));
      oldRouting->setAlternatives({{"Instrument · MIDI 1",0},{"Native MIDI",1}});
      auto json=toValue(score::marshall<JSONObject>(*base));JSONObject::Deserializer reader{json};ProcessModel restored{reader,nullptr};
      CHECK(restored.settings()==p.settings());CHECK(restored.dmc()==p.dmc());CHECK(restored.stateData()==p.stateData());
      checkAuthoredWave(restored,wave);
      QByteArray binary;DataStream::Serializer writer{&binary};
      TSerializer<DataStream,score::Entity<Process::ProcessModel>>::readFrom(writer,p);
      writer.read(static_cast<const Process::ProcessModel&>(p));writer.read(p);
      DataStream::Deserializer binaryReader{binary};ProcessModel binaryRestored{binaryReader,nullptr};
      CHECK(binaryRestored.settings()==p.settings());CHECK(binaryRestored.dmc()==p.dmc());
      CHECK(binaryRestored.stateData()==p.stateData());checkAuthoredWave(binaryRestored,wave);
      for(auto* loaded:{&restored,&binaryRestored}) {
        auto* allocation=qobject_cast<Process::ComboBox*>(&control(*loaded,Routing));REQUIRE(allocation);
        CHECK(allocation->id().val()==90);REQUIRE(allocation->count()==2);
        CHECK(allocation->getValues()[0].second==ossia::value{0});
        CHECK(allocation->getValues()[1].second==ossia::value{1});
        CHECK(allocation->value()==ossia::value{model%2});
        Engine engine;REQUIRE(engine.prepare(48000,loaded->settings(),loaded->dmc()));
        const auto channel=uint8_t(loaded->settings().routing==RoutingMode::Instrument
            ?0:int(loaded->settings().sound));
        engine.midi(std::array<uint8_t,3>{uint8_t(0x90+channel),69,41});
        engine.midi(std::array<uint8_t,3>{uint8_t(0x90+channel),72,100});
        CHECK(engine.activeVoices()==1);
        engine.midi(std::array<uint8_t,3>{uint8_t(0x80+channel),72,0});
        CHECK(engine.activeVoices()==1);
        engine.midi(std::array<uint8_t,3>{uint8_t(0x80+channel),69,0});
        CHECK(engine.activeVoices()==0);CHECK(engine.error()==nullptr);
      }
      for(int c=0;c<ControlCount;++c)CHECK(control(restored,c).value()==control(p,c).value());
      json["State"]["Model"].SetInt(99);
      JSONObject::Deserializer invalidReader{json};
      CHECK_THROWS_AS((ProcessModel{invalidReader,nullptr}),std::runtime_error);
    }
  });
}

TEST_CASE("APU Volume preserves native presets and migrates stored unity domains", "[emulation][apu][document][preset]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);
    auto* base=score::test::add_process(*doc,QString::fromLatin1(uuid),{});REQUIRE(base);
    auto& model=static_cast<ProcessModel&>(*base);
    for(float gain:{0.f,.375f,1.f,4.5f,Emulation::MaxVolume}) {
      control(model,Volume).setValue(gain);control(model,Volume).setInit(.625f);
      control(model,Volume).setDomain(ossia::make_domain(0.f,gain<=1.f?1.f:Emulation::MaxVolume));
      const auto expected=model.settings();REQUIRE(expected.volume==gain);
      const auto json=toValue(score::marshall<JSONObject>(*base));
      JSONObject::Deserializer reader{json};ProcessModel restored{reader,nullptr};
      QByteArray bytes;DataStream::Serializer writer{&bytes};
      TSerializer<DataStream,score::Entity<Process::ProcessModel>>::readFrom(writer,model);
      writer.read(static_cast<const Process::ProcessModel&>(model));writer.read(model);
      DataStream::Deserializer binaryReader{bytes};ProcessModel binary{binaryReader,nullptr};
      for(auto* copy:{&restored,&binary}) {
        CHECK(copy->settings()==expected);
        CHECK(control(*copy,Volume).value()==ossia::value{gain});
        CHECK(control(*copy,Volume).init()==ossia::value{.625f});
        CHECK(control(*copy,Volume).domain().get().convert_min<float>()==0.f);
        CHECK(control(*copy,Volume).domain().get().convert_max<float>()==Emulation::MaxVolume);
        CHECK(control(*copy,roleControl(0,Level)).domain().get().convert_max<int>()==15);
        ossia::value_port source;source.write_value(gain,7);
        ossia::value_inlet sink;control(*copy,Volume).setupExecution(sink,nullptr);
        sink.data.add_port_values(source);
        REQUIRE(sink.data.get_data().size()==1);
        CHECK(sink.data.get_data().front().value==ossia::value{gain});
        CHECK(sink.data.get_data().front().timestamp==7);
      }
      const auto preset=model.savePreset();
      control(restored,Volume).setValue(gain==0.f?1.f:0.f);
      restored.loadPreset(preset);CHECK(restored.settings()==expected);CHECK(restored.error().isEmpty());
      auto invalid=preset;
      auto state=QJsonDocument::fromJson(invalid.data).object();state["Volume"]=Emulation::MaxVolume+1.;
      invalid.data=QJsonDocument{state}.toJson(QJsonDocument::Compact);
      restored.loadPreset(invalid);CHECK_FALSE(restored.error().isEmpty());CHECK(restored.settings()==expected);
      for(float bad:{-1.f,Emulation::MaxVolume+.01f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        auto settings=expected;CHECK_FALSE(setScalar(settings,Volume,bad));CHECK(settings==expected);
        control(model,Volume).setValue(bad);CHECK(model.settings()==expected);
        CHECK(control(model,Volume).value()==ossia::value{gain});CHECK_FALSE(model.error().isEmpty());
      }
    }
  });
}
TEST_CASE("APU model controls preserve automatic and custom clocks under native undo", "[emulation][apu][document][undo]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);
    auto* base=score::test::add_process(*doc,QString::fromLatin1(uuid),{});REQUIRE(base);
    auto& p=static_cast<ProcessModel&>(*base);CommandDispatcher<> dispatcher{doc->context().commandStack};
    for(int clock:{0,2000000}) {
      control(p,Clock).setValue(clock);const auto before=p.settings();
      dispatcher.submit<Process::SetControlValue>(control(p,ChipModel),ossia::value{2});
      REQUIRE(p.settings().model==Model::Dmg);CHECK(p.settings().clock==clock);
      CHECK(effectiveClock(p.settings())==(clock?clock:4194304));
      REQUIRE(doc->commandStack().canUndo());doc->commandStack().undo();CHECK(p.settings()==before);
      doc->commandStack().redo();CHECK(p.settings().model==Model::Dmg);CHECK(p.settings().clock==clock);
      doc->commandStack().undo();
    }
  });
}
TEST_CASE("APU architecture banks preserve distinct hardware edits through switching and native undo", "[emulation][apu][document][undo][preset]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);
    auto* base=score::test::add_process(*doc,QString::fromLatin1(uuid),{});REQUIRE(base);auto& p=static_cast<ProcessModel&>(*base);
    CommandDispatcher<> dispatcher{doc->context().commandStack};
    control(p,roleControl(0,Length)).setValue(31);
    control(p,roleControl(0,EnvelopePeriod)).setValue(15);
    control(p,roleControl(4,Rate)).setValue(15);
    const auto nes=p.settings().roles;
    dispatcher.submit<Process::SetControlValue>(control(p,ChipModel),ossia::value{2});
    REQUIRE(p.settings().model==Model::Dmg);REQUIRE(p.error().isEmpty());
    dispatcher.submit<Process::SetControlValue>(control(p,roleControl(0,Length)),ossia::value{40});
    control(p,roleControl(2,Length)).setValue(255);
    control(p,roleControl(3,Length)).setValue(63);
    control(p,roleControl(0,EnvelopePeriod)).setValue(7);
    control(p,roleControl(3,Rate)).setValue(127);
    const auto gb=p.settings().roles;
    dispatcher.submit<Process::SetControlValue>(control(p,ChipModel),ossia::value{0});
    REQUIRE(p.settings().model==Model::NesNtsc);CHECK(p.settings().roles==nes);CHECK(p.settings().inactiveRoles==gb);
    const auto both=p.settings();
    doc->commandStack().undo();CHECK(p.settings().model==Model::Dmg);CHECK(p.settings().roles==gb);CHECK(p.settings().inactiveRoles==nes);
    doc->commandStack().undo();CHECK(p.settings().roles[0].length==0);
    doc->commandStack().redo();CHECK(p.settings().roles==gb);
    doc->commandStack().redo();CHECK(p.settings()==both);
    control(p,ChipModel).setValue(1);CHECK(p.settings().roles==nes);
    control(p,ChipModel).setValue(3);CHECK(p.settings().roles==gb);
    control(p,ChipModel).setValue(2);CHECK(p.settings().roles==gb);
    const auto saved=p.savePreset();
    control(p,roleControl(0,Length)).setValue(50);
    control(p,ChipModel).setValue(0);control(p,roleControl(0,EnvelopePeriod)).setValue(1);
    const auto edited=p.settings();
    dispatcher.submit<Scenario::Command::LoadPreset>(p,saved);
    CHECK(p.settings().roles==gb);CHECK(p.settings().inactiveRoles==nes);
    doc->commandStack().undo();CHECK(p.settings()==edited);
    doc->commandStack().redo();CHECK(p.settings().roles==gb);CHECK(p.settings().inactiveRoles==nes);
  });
}
TEST_CASE("APU offered hardware domains reject invalid values without altering authored state", "[emulation][apu][document]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);
    auto* base=score::test::add_process(*doc,QString::fromLatin1(uuid),{});REQUIRE(base);auto& p=static_cast<ProcessModel&>(*base);
    for(int model=0;model<4;++model) {
      control(p,ChipModel).setValue(model);REQUIRE(p.error().isEmpty());
      const auto checkLimit=[&](int c,int maximum) {
        CHECK(control(p,c).domain().get().convert_max<int>()==maximum);
        control(p,c).setValue(maximum);REQUIRE(p.error().isEmpty());
        const auto accepted=p.stateData();
        control(p,c).setValue(maximum+1);
        CHECK_FALSE(p.error().isEmpty());CHECK(p.stateData()==accepted);
        CHECK(ossia::convert<int>(control(p,c).value())==maximum);
        control(p,c).setValue(maximum-1);CHECK(p.error().isEmpty());
      };
      for(int r=0;r<4;++r)checkLimit(roleControl(r,Length),model<2?31:r==2?255:63);
      for(int r:{0,1,3})checkLimit(roleControl(r,EnvelopePeriod),model<2?15:7);
      checkLimit(roleControl(3,Rate),model<2?15:127);
      if(model<2)checkLimit(roleControl(4,Rate),15);
      else {
        auto* levels=qobject_cast<Process::ComboBox*>(&control(p,roleControl(2,Level)));REQUIRE(levels);
        for(int level=0;level<=3;++level) {
          control(p,roleControl(2,Level)).setValue(level);CHECK(p.settings().roles[2].level==level);
        }
        const auto accepted=p.stateData();control(p,roleControl(2,Level)).setValue(4);CHECK(p.stateData()==accepted);
        control(p,roleControl(2,Level)).setValue(1);
      }
    }
    const auto saved=p.stateData();
    control(p,Wave).setValue(std::vector<ossia::value>(31,ossia::value{std::string{"x"}}));CHECK(p.stateData()==saved);
    control(p,Clock).setValue(1);CHECK(p.stateData()==saved);
    const auto asset=p.dmc();CHECK_FALSE(p.importDmcBytes({}));CHECK(p.dmc()==asset);
    CHECK_FALSE(p.importDmcBytes(QByteArray(16385,'x')));CHECK(p.dmc()==asset);
    for(int failure=0;failure<5;++failure) {
      auto object=QJsonDocument::fromJson(saved).object();
      switch(failure){case 0:object["Model"]=99;break;case 1:object["Wave"]=QJsonArray{0,1};break;
        case 2:object["DmcData"]=QStringLiteral("!");break;case 3:object["NesRoles"]=QJsonArray{};break;case 4:object["Clock"]=1;break;}
      auto preset=p.savePreset();preset.data=QJsonDocument{object}.toJson(QJsonDocument::Compact);p.loadPreset(preset);
      CHECK(p.stateData()==saved);CHECK_FALSE(p.error().isEmpty());
    }
    auto valid=p.savePreset();p.loadPreset(valid);CHECK(p.error().isEmpty());CHECK(p.stateData()==saved);
  });
}
TEST_CASE("APU raw DMC import is bounded asynchronous embedded and native preset undoable", "[emulation][apu][document][preset]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);
    auto* base=score::test::add_process(*doc,QString::fromLatin1(uuid),{});REQUIRE(base);auto& p=static_cast<ProcessModel&>(*base);
    const auto original=p.savePreset();QTemporaryFile file;REQUIRE(file.open());
    const QByteArray bytes=QByteArray::fromHex("1023456789abcdef");REQUIRE(file.write(bytes)==bytes.size());file.close();
    const auto sourcePath=file.fileName();
    QEventLoop loop;bool loaded{};QTimer timeout;timeout.setSingleShot(true);
    QObject::connect(&p,&ProcessModel::assetChanged,&loop,[&]{loaded=true;loop.quit();});
    QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);
    p.importDmc(sourcePath);timeout.start(5000);loop.exec();REQUIRE(loaded);REQUIRE(p.error().isEmpty());
    REQUIRE(file.remove());CHECK(p.dmc().size==bytes.size());
    const auto imported=p.savePreset();const auto asset=p.dmc();p.loadPreset(original);
    CommandDispatcher<> dispatcher{doc->context().commandStack};dispatcher.submit<Scenario::Command::LoadPreset>(p,imported);
    CHECK(p.dmc()==asset);doc->commandStack().undo();CHECK(p.savePreset().data==original.data);
    doc->commandStack().redo();CHECK(p.dmc()==asset);CHECK(p.error().isEmpty());
    QEventLoop failureLoop;QTimer failureTimeout;failureTimeout.setSingleShot(true);bool failed{};
    QObject::connect(&p,&ProcessModel::errorChanged,&failureLoop,[&]{if(!p.error().isEmpty()){failed=true;failureLoop.quit();}});
    QObject::connect(&failureTimeout,&QTimer::timeout,&failureLoop,&QEventLoop::quit);
    p.importDmc(sourcePath);failureTimeout.start(5000);failureLoop.exec();
    REQUIRE(failed);CHECK(p.dmc()==asset);
    control(p,Volume).setValue(.5f);CHECK_FALSE(p.error().isEmpty());
    p.setEngineError({});CHECK_FALSE(p.error().isEmpty());CHECK(p.dmc()==asset);
    p.loadPreset(imported);CHECK(p.error().isEmpty());CHECK(p.dmc()==asset);
  });
}
TEST_CASE("APU timestamped volume and full-buffer split tokens avoid duplicate events", "[emulation][apu][execution]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);auto& plug=doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true,score::test::base_interval(*doc));drain(plug);auto& state=*plug.context().execState;REQUIRE(state.bufferSize>=64);
    for(auto model:{Model::NesNtsc,Model::NesPal,Model::Dmg,Model::Cgb}) {
      Settings settings;settings.model=model;settings.volume=1.;Executor::Node reference{state,settings},adjusted{state,settings};
      for(auto* node:{&reference,&adjusted}) {
        auto engine=std::make_shared<Engine>();REQUIRE(engine->prepare(state.sampleRate,settings));node->install(engine,model,settings.clock);REQUIRE(node->ready());
        node->midi->data.messages.push_back(libremidi::from_midi1::note_on(0,69,127));
      }
      const int frames=state.bufferSize,q=frames/4;
      adjusted.controls[Volume]->data.write_value(Emulation::MaxVolume,q);adjusted.controls[Volume]->data.write_value(0.f,2*q);adjusted.controls[Volume]->data.write_value(1.f,3*q);
      auto token=tokenFor(state),first=token,second=token;first.length_sample=2*q;
      first.date=ossia::time_value{int64_t(2*q*ossia::flicks_per_second<double>/state.sampleRate)};
      second.prev_date=first.date;second.start_sample=2*q;second.length_sample=frames-2*q;state.samples_since_start+=frames;
      ossia::set_thread_pinned(ossia::thread_type::Audio,0);
      reference.run(token,ossia::exec_state_facade{&state});adjusted.run(first,ossia::exec_state_facade{&state});
      const auto fullSize=adjusted.audio->data.channel(0).size();adjusted.run(second,ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui,0);CHECK(fullSize==frames);
      const auto& a=reference.audio->data.channel(0);const auto& b=adjusted.audio->data.channel(0);
      for(int i=0;i<frames;++i)CHECK(b[i]==a[i]*(i<q||i>=3*q?1.:i<2*q?Emulation::MaxVolume:0.));
      const std::vector<double> saved{b.begin(),b.end()};
      ossia::set_thread_pinned(ossia::thread_type::Audio,0);adjusted.run(token,ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui,0);CHECK(std::equal(saved.begin(),saved.end(),b.begin()));
    }
  });
}
TEST_CASE("APU sample edits preserve native undo presets and endpoint identity", "[emulation][apu][document][waveform][undo]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);
    auto* base=score::test::add_process(*doc,QString::fromLatin1(uuid),{});REQUIRE(base);
    auto& p=static_cast<ProcessModel&>(*base);control(p,ChipModel).setValue(2);
    auto& inlet=waveInlet(p);const auto original=p.savePreset();const auto originalValue=inlet.value();
    const auto id=inlet.id();
    CommandDispatcher<> dispatcher{doc->context().commandStack};
    const auto index=doc->commandStack().currentIndex();
    const auto samples=authoredWave();
    dispatcher.submit<Process::SetControlValue>(inlet,samples);
    CHECK(doc->commandStack().currentIndex()==index+1);checkAuthoredWave(p,samples);
    const auto editedSettings=p.settings();
    doc->commandStack().undo();checkAuthoredWave(p,originalValue);
    doc->commandStack().redo();checkAuthoredWave(p,samples);CHECK(p.settings()==editedSettings);
    for(int model:{0,1,2,3}) {
      control(p,ChipModel).setValue(model);
      CHECK(&waveInlet(p)==&inlet);CHECK(inlet.id()==id);checkAuthoredWave(p,samples);
    }
    control(p,Volume).setValue(.375f);control(p,Clock).setValue(2000000);
    const auto edited=p.savePreset();
    dispatcher.submit<Scenario::Command::LoadPreset>(p,original);
    CHECK(p.stateData()==original.data);checkAuthoredWave(p,originalValue);
    doc->commandStack().undo();CHECK(p.stateData()==edited.data);checkAuthoredWave(p,samples);
    doc->commandStack().redo();CHECK(p.stateData()==original.data);checkAuthoredWave(p,originalValue);
    // Distinct normalized sample edits remain authored even if RAM is unchanged.
    Process::WavetableData flat{32,std::vector<float>(32,.1f)};
    inlet.setWaveform(flat);const auto native=p.settings();const auto before=p.stateData();
    flat.samples[0]=.101f;inlet.setWaveform(flat);
    CHECK(p.settings()==native);CHECK(p.stateData()!=before);
    const auto value=inlet.value();const auto saved=p.savePreset();
    p.loadPreset(original);p.loadPreset(saved);checkAuthoredWave(p,value);
    CHECK(p.stateData()==saved.data);
  });
}

TEST_CASE("APU waveform validation preserves samples and current schema atomically", "[emulation][apu][document][waveform]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);
    auto* base=score::test::add_process(*doc,QString::fromLatin1(uuid),{});REQUIRE(base);
    auto& p=static_cast<ProcessModel&>(*base);auto& inlet=waveInlet(p);
    checkAuthoredWave(p,Emulation::Waveform::triangle());CHECK(p.settings().wave==Settings{}.wave);
    inlet.setValue(authoredWave());const auto saved=p.savePreset();const auto value=inlet.value();
    for(int invalid=0;invalid<5;++invalid) {
      auto table=inlet.wavetable();
      switch(invalid) {
        case 0:table.samples[0]=1.01f;break;
        case 1:table.samples[0]=std::numeric_limits<float>::quiet_NaN();break;
        case 2:table.frameSize=16;break;
        case 3:table.samples.pop_back();break;
        case 4:table.samples.clear();break;
      }
      inlet.setWaveform(table);CHECK(inlet.value()==value);CHECK(p.stateData()==saved.data);
    }
    for(int schema:{1,2,3,4,6}) {
      auto object=QJsonDocument::fromJson(saved.data).object();object["Schema"]=schema;
      auto rejected=saved;rejected.data=QJsonDocument{object}.toJson(QJsonDocument::Compact);
      p.loadPreset(rejected);CHECK_FALSE(p.error().isEmpty());CHECK(p.stateData()==saved.data);
    }
    p.loadPreset(saved);REQUIRE(p.error().isEmpty());checkAuthoredWave(p,value);
  });
}

TEST_CASE("APU wave inlet drives wave RAM from execution values", "[emulation][apu][execution][waveform]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);
    auto* base=score::test::add_process(*doc,QString::fromLatin1(uuid),{});REQUIRE(base);
    auto& p=static_cast<ProcessModel&>(*base);control(p,ChipModel).setValue(2);
    control(p,Sound).setValue(int(SoundRole::TriangleWave));
    auto& plug=doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true,score::test::base_interval(*doc));drain(plug);
    const auto& processes=plug.baseScenario()->baseInterval().processes();
    const auto found=processes.find(p.id());REQUIRE(found!=processes.end());
    const auto node=std::static_pointer_cast<Executor::Node>(found->second->node);
    REQUIRE(node->ready());CHECK(node->controls[Wave]->data.is_event);
    const auto registered=plug.context().setup.inlets.find(&waveInlet(p));
    REQUIRE(registered!=plug.context().setup.inlets.end());
    CHECK(registered->second.second==node->controls[Wave]);
    // Document edits still reach the node as prepared snapshots.
    CHECK(node->controls[Wave]->data.get_data().empty());
    waveInlet(p).setValue(authoredWave());drain(plug);
    CHECK(node->controls[Wave]->data.get_data().empty());
    auto& state=*plug.context().execState;
    Engine reference;REQUIRE(reference.prepare(state.sampleRate,p.settings(),p.dmc()));
    reference.midi(std::array<uint8_t,3>{0x90,69,127});
    node->midi->data.messages.push_back(libremidi::from_midi1::note_on(0,69,127));
    // A multislider's cycle: four numbers in [0, 1], resampled to 32 nibbles.
    const ossia::value slider{std::vector<ossia::value>{0.f,1.f,.5f,1.f}};
    auto received=p.settings();REQUIRE(Process::prepareWaveform(slider,received.wave,0,15));
    CHECK(received.wave!=p.settings().wave);
    std::vector<ossia::value> executionValues;
    QObject::connect(&waveInlet(p),&Process::ControlInlet::executionValueChanged,&waveInlet(p),
        [&](const ossia::value& v){executionValues.push_back(v);});
    const auto before=waveInlet(p).value();const auto commands=doc->commandStack().currentIndex();
    for(int tick=0;tick<3;++tick) {
      if(tick==1) {node->controls[Wave]->data.write_value(slider,0);REQUIRE(reference.configure(received));}
      state.samples_since_start+=state.bufferSize;
      ossia::set_thread_pinned(ossia::thread_type::Audio,0);
      node->run(tokenFor(state),ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui,0);
      std::vector<double> left(state.bufferSize),right(state.bufferSize);
      reference.render(left.data(),right.data(),left.size());
      CHECK(std::equal(left.begin(),left.end(),node->audio->data.channel(0).begin()));
      CHECK(std::equal(right.begin(),right.end(),node->audio->data.channel(1).begin()));
      CHECK(node->error()==nullptr);
      node->midi->data.messages.clear();
      for(auto* inlet:node->controls)inlet->data.get_data().clear();
    }
    CHECK(score::test::wait_until([&]{return !executionValues.empty();},5000));
    REQUIRE(executionValues.size()==1);
    CHECK(executionValues.front()==Emulation::Waveform::fromSamples(received.wave,0,15));
    CHECK(waveInlet(p).value()==before);CHECK(doc->commandStack().currentIndex()==commands);
    // An unrelated document edit leaves the received wave playing.
    control(p,Volume).setValue(.5f);drain(plug);
    received.volume=.5;REQUIRE(reference.configure(received));
    state.samples_since_start+=state.bufferSize;
    ossia::set_thread_pinned(ossia::thread_type::Audio,0);
    node->run(tokenFor(state),ossia::exec_state_facade{&state});
    ossia::set_thread_pinned(ossia::thread_type::Ui,0);
    std::vector<double> left(state.bufferSize),right(state.bufferSize);
    reference.render(left.data(),right.data(),left.size());
    CHECK(std::equal(left.begin(),left.end(),node->audio->data.channel(0).begin()));
  });
}

TEST_CASE("APU MIDI recovery clears transient executor errors without hiding rejected controls", "[emulation][apu][execution]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);auto& plug=doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true,score::test::base_interval(*doc));drain(plug);auto& state=*plug.context().execState;
    Settings settings;Executor::Node node{state,settings};
    auto engine=std::make_shared<Engine>();REQUIRE(engine->prepare(state.sampleRate,settings));
    const auto retained=engine;node.install(engine,settings.model,settings.clock);
    const auto run=[&](libremidi::ump message) {
      node.midi->data.messages.clear();node.midi->data.messages.push_back(message);
      state.samples_since_start+=state.bufferSize;
      ossia::set_thread_pinned(ossia::thread_type::Audio,0);node.run(tokenFor(state),ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui,0);
    };
    auto invalid=libremidi::from_midi1::note_on(0,60,127);invalid.data[0]|=uint32_t{1}<<24;
    run(invalid);REQUIRE(node.error());CHECK(retained->activeVoices()==0);
    run(libremidi::from_midi1::note_on(0,60,127));CHECK(node.error()==nullptr);CHECK(retained->activeVoices()==1);
    run(libremidi::from_midi1::note_on(15,60,127));REQUIRE(node.error());
    run(libremidi::from_midi1::note_off(0,60,0));CHECK(node.error()==nullptr);
    node.controls[roleControl(4,Rate)]->data.write_value(16,0);
    run(libremidi::from_midi1::note_on(0,60,127));REQUIRE(node.error());
    CHECK(retained->settings().roles[4].rate==settings.roles[4].rate);
    node.controls[roleControl(4,Rate)]->data.get_data().clear();
    run(libremidi::from_midi1::note_off(0,60,0));CHECK(node.error());
    node.controls[roleControl(4,Rate)]->data.write_value(settings.roles[4].rate,0);
    run(libremidi::from_midi1::note_on(0,60,127));CHECK(node.error()==nullptr);
    node.controls[roleControl(4,Rate)]->data.get_data().clear();
    node.controls[roleControl(4,Rate)]->data.write_value(15,0);
    run(libremidi::from_midi1::note_on(0,60,127));CHECK(node.error()==nullptr);CHECK(retained->settings().roles[4].rate==15);
  });
}

TEST_CASE("APU timestamped model automation selects complete architecture banks atomically", "[emulation][apu][execution]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);auto& plug=doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true,score::test::base_interval(*doc));drain(plug);auto& state=*plug.context().execState;
    Settings initial;initial.roles[0].length=31;initial.roles[0].envelopePeriod=15;
    initial.sound=SoundRole::Dmc;initial.inactiveSound=SoundRole::TriangleWave;
    Executor::Node node{state,initial};
    auto prepared=std::make_shared<Engine>();REQUIRE(prepared->prepare(state.sampleRate,initial));
    node.install(prepared,initial.model,initial.clock);REQUIRE(node.ready());
    std::array<Role,5> gb;
    for(auto model:{Model::Dmg,Model::NesPal,Model::Cgb,Model::NesNtsc}) {
      node.controls[ChipModel]->data.write_value(int(model),0);
      if(model==Model::Dmg) {
        node.controls[roleControl(0,Length)]->data.write_value(40,0);
        node.controls[roleControl(2,Length)]->data.write_value(255,0);
        node.controls[roleControl(0,EnvelopePeriod)]->data.write_value(7,0);
      }
      state.samples_since_start+=state.bufferSize;
      ossia::set_thread_pinned(ossia::thread_type::Audio,0);node.run(tokenFor(state),ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui,0);
      REQUIRE_FALSE(node.ready());
      Model requested;int clock{};REQUIRE(node.takeConfiguration(requested,clock));CHECK(requested==model);CHECK(clock==0);
      Settings setup;selectModel(setup,requested);setup.clock=clock;
      prepared=std::make_shared<Engine>();REQUIRE(prepared->prepare(state.sampleRate,setup));
      const auto installed=prepared;node.install(prepared,requested,clock);REQUIRE(node.ready());CHECK(node.error()==nullptr);
      const auto& selected=installed->settings();
      if(model==Model::Dmg) {
        CHECK(selected.roles[0].length==40);CHECK(selected.roles[2].length==255);CHECK(selected.roles[0].envelopePeriod==7);
        gb=selected.roles;
      }
      CHECK((gameBoy(model)?selected.inactiveRoles:selected.roles)==initial.roles);
      CHECK((gameBoy(model)?selected.roles:selected.inactiveRoles)==gb);
      CHECK(selected.sound==(gameBoy(model)?SoundRole::TriangleWave:SoundRole::Dmc));
      CHECK(selected.inactiveSound==(gameBoy(model)?SoundRole::Dmc:SoundRole::TriangleWave));
      for(auto* inlet:node.controls)inlet->data.get_data().clear();
    }
  });
}

TEST_CASE("APU sound choice remembers both architectures and native undo restores routing", "[emulation][apu][document][routing][undo]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);
    auto* base=score::test::add_process(*doc,QString::fromLatin1(uuid),{});REQUIRE(base);auto& p=static_cast<ProcessModel&>(*base);
    REQUIRE(p.settings().routing==RoutingMode::Instrument);CHECK(p.settings().sound==SoundRole::Pulse1);
    CHECK(control(p,Routing).id().val()==90);CHECK(control(p,Sound).id().val()==91);
    CommandDispatcher<> dispatcher{doc->context().commandStack};
    for(auto routing:{RoutingMode::Instrument,RoutingMode::NativeMidi}) {
      control(p,Routing).setValue(int(routing));
      control(p,ChipModel).setValue(2);control(p,Sound).setValue(int(SoundRole::TriangleWave));
      control(p,ChipModel).setValue(0);control(p,Sound).setValue(int(SoundRole::Dmc));
      const auto nes=p.settings();
      dispatcher.submit<Process::SetControlValue>(control(p,ChipModel),ossia::value{2});
      REQUIRE(p.error().isEmpty());CHECK(p.settings().sound==SoundRole::TriangleWave);
      CHECK(p.settings().inactiveSound==SoundRole::Dmc);
      auto* sounds=qobject_cast<Process::ComboBox*>(&control(p,Sound));REQUIRE(sounds);
      CHECK(sounds->count()==4);CHECK(sounds->getValues()[2].first==QStringLiteral("Wave"));
      const auto gb=p.settings();
      control(p,Sound).setValue(int(SoundRole::Dmc));CHECK_FALSE(p.error().isEmpty());CHECK(p.settings()==gb);
      CHECK(control(p,Sound).value()==ossia::value{int(SoundRole::TriangleWave)});
      doc->commandStack().undo();CHECK(p.settings()==nes);REQUIRE(p.error().isEmpty());
      CHECK(sounds->count()==5);CHECK(sounds->getValues()[2].first==QStringLiteral("Triangle"));
      doc->commandStack().redo();CHECK(p.settings()==gb);
      doc->commandStack().undo();CHECK(p.settings()==nes);
    }
    const auto before=p.savePreset();
    dispatcher.submit<Process::SetControlValue>(control(p,Routing),ossia::value{0});
    CHECK(p.settings().routing==RoutingMode::Instrument);
    doc->commandStack().undo();CHECK(p.stateData()==before.data);
    doc->commandStack().redo();CHECK(p.settings().routing==RoutingMode::Instrument);
    const auto remembered=p.savePreset();
    control(p,ChipModel).setValue(2);control(p,Sound).setValue(3);control(p,Routing).setValue(1);
    const auto edited=p.savePreset();
    dispatcher.submit<Scenario::Command::LoadPreset>(p,remembered);CHECK(p.stateData()==remembered.data);
    doc->commandStack().undo();CHECK(p.stateData()==edited.data);
    doc->commandStack().redo();CHECK(p.stateData()==remembered.data);
    auto invalid=QJsonDocument::fromJson(remembered.data).object();invalid["GbSound"]=4;
    auto preset=remembered;preset.data=QJsonDocument{invalid}.toJson(QJsonDocument::Compact);
    p.loadPreset(preset);CHECK_FALSE(p.error().isEmpty());CHECK(p.stateData()==remembered.data);
  });
}


TEST_CASE("APU native factory presets select their named sound on MIDI channel one", "[emulation][apu][document][preset][routing]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);
    auto* base=score::test::add_process(*doc,QString::fromLatin1(uuid),{});REQUIRE(base);auto& p=static_cast<ProcessModel&>(*base);
    const auto directory=QFileInfo{QString::fromUtf8(__FILE__)}.absoluteDir();
    CommandDispatcher<> dispatcher{doc->context().commandStack};
    struct FactoryPreset { const char* name; Model model; SoundRole sound; bool triangle; };
    const FactoryPreset presets[]{
      {"NES Clear pulse",Model::NesNtsc,SoundRole::Pulse1,true},
      {"GB Triangle wave",Model::Dmg,SoundRole::TriangleWave,true},
      {"GB CGB Alloy snare",Model::Cgb,SoundRole::Noise,false},
      {"GB CGB Pinched pulse",Model::Cgb,SoundRole::Pulse1,false},
      {"GB DMG Hollow reed",Model::Dmg,SoundRole::TriangleWave,false},
      {"GB DMG Sand hat",Model::Dmg,SoundRole::Noise,false},
      {"NES NTSC Quarter bass",Model::NesNtsc,SoundRole::Pulse1,false},
      {"NES NTSC Triangle mallet",Model::NesNtsc,SoundRole::TriangleWave,false},
      {"NES PAL Delta woodblock",Model::NesPal,SoundRole::Dmc,false},
      {"NES PAL Needle lead",Model::NesPal,SoundRole::Pulse2,false}};
    constexpr std::array<uint8_t,32> reed{
      2,3,5,7,9,11,13,13,12,11,10,10,10,10,10,10,
      10,10,9,8,7,6,5,4,3,2,2,2,2,2,2,2};
    for(const auto& [name,model,sound,triangle]:presets) {
      CAPTURE(name);
      QFile file{directory.filePath(QStringLiteral("../Presets/Apu/%1.scp").arg(QString::fromLatin1(name)))};
      REQUIRE(file.open(QIODevice::ReadOnly));
      auto preset=Process::Preset::fromJson(ctx.interfaces<Process::ProcessFactoryList>(),file.readAll());REQUIRE(preset);
      const auto before=p.savePreset();
      dispatcher.submit<Scenario::Command::LoadPreset>(p,*preset);REQUIRE(p.error().isEmpty());
      const auto s=p.settings();CHECK(s.routing==RoutingMode::Instrument);
      CHECK(s.model==model);CHECK(s.sound==sound);
      CHECK(s.wave==(triangle?Settings{}.wave:reed));
      checkAuthoredWave(p,waveInlet(p).value());
      Engine played,reference;REQUIRE(played.prepare(48000,s,p.dmc()));
      auto native=s;native.routing=RoutingMode::NativeMidi;REQUIRE(reference.prepare(48000,native,p.dmc()));
      played.midi(std::array<uint8_t,3>{0x90,69,127});
      reference.midi(std::array<uint8_t,3>{uint8_t(0x90+int(s.sound)),69,127});
      std::array<double,4096> left{},right{},expectedLeft{},expectedRight{};
      played.render(left.data(),right.data(),left.size());reference.render(expectedLeft.data(),expectedRight.data(),expectedLeft.size());
      CHECK(left==expectedLeft);CHECK(right==expectedRight);
      const auto [lo,hi]=std::minmax_element(left.begin(),left.end());CHECK(*hi-*lo>.0001);
      const auto loaded=p.savePreset();
      doc->commandStack().undo();CHECK(p.stateData()==before.data);
      doc->commandStack().redo();CHECK(p.stateData()==loaded.data);
    }
  });
}

TEST_CASE("APU timestamped sound and routing automation retain correct held-note ownership", "[emulation][apu][execution][routing]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);auto& plug=doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true,score::test::base_interval(*doc));drain(plug);auto& state=*plug.context().execState;
    for(auto model:{Model::NesNtsc,Model::NesPal,Model::Dmg,Model::Cgb}) {
      Settings initial;initial.model=model;initial.dmcLoop=true;
      Executor::Node node{state,initial};auto prepared=std::make_shared<Engine>();REQUIRE(prepared->prepare(state.sampleRate,initial));
      const auto engine=prepared;node.install(prepared,model,initial.clock);REQUIRE(node.ready());
      engine->midi(std::array<uint8_t,3>{0x90,60,127});
      const auto run=[&] {
        state.samples_since_start+=state.bufferSize;
        ossia::set_thread_pinned(ossia::thread_type::Audio,0);node.run(tokenFor(state),ossia::exec_state_facade{&state});
        ossia::set_thread_pinned(ossia::thread_type::Ui,0);
        node.midi->data.messages.clear();for(auto* c:node.controls)c->data.get_data().clear();
        REQUIRE(node.ready());CHECK(node.error()==nullptr);
      };
      const int role=gameBoy(model)?3:4;node.controls[Sound]->data.write_value(role,0);
      node.midi->data.messages.push_back(libremidi::from_midi1::note_on(0,60,127));
      auto stale=libremidi::from_midi1::note_off(0,60,0);stale.timestamp=state.bufferSize/2;node.midi->data.messages.push_back(stale);
      run();CHECK(engine->settings().sound==SoundRole(role));CHECK(engine->activeVoices()==1);
      node.midi->data.messages.push_back(libremidi::from_midi1::note_off(0,60,0));run();CHECK(engine->activeVoices()==0);
      node.controls[Routing]->data.write_value(int(RoutingMode::NativeMidi),0);
      node.midi->data.messages.push_back(libremidi::from_midi1::note_on(1,64,127));run();CHECK(engine->activeVoices()==1);
      node.controls[Routing]->data.write_value(int(RoutingMode::Instrument),0);
      node.controls[Sound]->data.write_value(0,0);
      node.midi->data.messages.push_back(libremidi::from_midi1::note_off(1,64,0));
      node.midi->data.messages.push_back(libremidi::from_midi1::note_on(0,67,127));run();CHECK(engine->activeVoices()==1);
      node.midi->data.messages.push_back(libremidi::from_midi1::note_off(0,67,0));run();CHECK(engine->activeVoices()==0);
    }
  });
}

TEST_CASE("APU native tabs do not route notes and import and sample controls use full factory layouts", "[emulation][apu][document][gui][routing]") {
  score::test::run_in_gui_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);
    auto* base=score::test::add_process(*doc,QString::fromLatin1(uuid),{});REQUIRE(base);auto& p=static_cast<ProcessModel&>(*base);
    auto* presenter=score::IDocument::try_presenterDelegate<Scenario::ScenarioDocumentPresenter>(*doc);REQUIRE(presenter);
    auto* factory=presenter->context().processList.findDefaultFactory(p);REQUIRE(factory);
    QGraphicsScene scene;
    std::unique_ptr<score::ResizeableItem> item{factory->makeItem(p,presenter->context(),nullptr)};
    REQUIRE(item);scene.addItem(item.get());
    const auto tabs=[&] {
      score::GraphicsTabLayout* result{};
      for(auto* child:scene.items())if(auto* tab=dynamic_cast<score::GraphicsTabLayout*>(child))result=tab;
      return result;
    };
    const auto endpoint=[&](int c) {
      Dataflow::PortItem* result{};
      for(auto* child:scene.items())
        if(auto* port=dynamic_cast<Dataflow::PortItem*>(child);port && &port->port()==&control(p,c))result=port;
      return result;
    };
    auto* tab=tabs();REQUIRE(tab);
    score::QGraphicsEnum* bar{};
    for(auto* child:tab->childItems())if(auto* value=dynamic_cast<score::QGraphicsEnum*>(child))bar=value;
    REQUIRE(bar);CHECK(bar->columns==6);
    const auto state=p.stateData();
    auto* sound=endpoint(Sound);REQUIRE(sound);CHECK(sound->isVisible());
    control(p,Routing).setValue(int(RoutingMode::NativeMidi));CHECK_FALSE(sound->isVisible());
    control(p,Routing).setValue(int(RoutingMode::Instrument));CHECK(sound->isVisible());
    CHECK(p.stateData()==state);
    for(int i=0;i<6;++i) {bar->currentIndexChanged(i);CHECK(tab->currentIndex()==i);CHECK(p.stateData()==state);}
    bar->currentIndexChanged(4);
    auto* dmc=endpoint(DmcFile);REQUIRE(dmc);CHECK(dmc->isVisible());
    score::QGraphicsTextButton* button{};
    for(auto* child:dmc->parentItem()->childItems())
      if(auto* value=dynamic_cast<score::QGraphicsTextButton*>(child))button=value;
    REQUIRE(button);
    CHECK_FALSE(button->mapRectToParent(button->boundingRect()).intersects(dmc->mapRectToParent(dmc->drawnRect())));
    control(p,ChipModel).setValue(2);
    tab=tabs();REQUIRE(tab);tab->setCurrentIndex(2);
    auto* wave=endpoint(Wave);REQUIRE(wave);CHECK(wave->isVisible());CHECK(wave->acceptDrops());
    QGraphicsItem* editor{};
    for(auto* child:scene.items())
      if(child->data(0).toString()==QStringLiteral("WaveformEditor"))editor=child;
    REQUIRE(editor);CHECK(editor->isVisible());CHECK(editor->boundingRect().width()>=430.);
    const auto gbState=p.stateData();
    for(int i=0;i<5;++i) {tab->setCurrentIndex(i);CHECK(p.stateData()==gbState);}
    tab->setCurrentIndex(2);
    QGraphicsView view{&scene};view.resize(900,650);
    view.setSceneRect(scene.itemsBoundingRect().adjusted(-20.,-20.,20.,20.));
    REQUIRE(score::test::showAndActivate(view));view.ensureVisible(editor);
    const auto before=waveInlet(p).value();const auto index=doc->commandStack().currentIndex();
    const auto drawing=Dataflow::waveformItemState(*editor).plot;
    const auto start=view.mapFromScene(editor->mapToScene(drawing.topLeft()+QPointF{1.,1.}));
    const auto end=view.mapFromScene(editor->mapToScene(drawing.topRight()+QPointF{-1.,1.}));
    REQUIRE(view.viewport()->rect().contains(start));REQUIRE(view.viewport()->rect().contains(end));
    score::test::mouseEvent(*view.viewport(),QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    score::test::mouseEvent(*view.viewport(),QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    score::test::mouseEvent(*view.viewport(),QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QCoreApplication::processEvents();
    CHECK(doc->commandStack().currentIndex()==index+1);
    const auto painted=waveInlet(p).value();CHECK(painted!=before);
    for(auto sample:p.settings().wave)CHECK(sample==15);
    checkAuthoredWave(p,painted);
    doc->commandStack().undo();checkAuthoredWave(p,before);
    doc->commandStack().redo();checkAuthoredWave(p,painted);
  });
}

TEST_CASE("APU live native preset cycling preserves sample bindings during MIDI", "[emulation][apu][document][gui][preset][live-recall]") {
  score::test::run_in_gui_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);
    auto* base=score::test::add_process(*doc,QString::fromLatin1(uuid),{});REQUIRE(base);
    auto& p=static_cast<ProcessModel&>(*base);
    const auto directory=QFileInfo{QString::fromUtf8(__FILE__)}.absoluteDir();
    std::array<Process::Preset,2> presets;
    for(int i=0;i<2;++i) {
      const auto name=i==0?QStringLiteral("GB Triangle wave"):QStringLiteral("GB DMG Sand hat");
      QFile file{directory.filePath(QStringLiteral("../Presets/Apu/%1.scp").arg(name))};
      REQUIRE(file.open(QIODevice::ReadOnly));
      auto preset=Process::Preset::fromJson(ctx.interfaces<Process::ProcessFactoryList>(),file.readAll());
      REQUIRE(preset);presets[i]=std::move(*preset);
    }
    // Exercise the actual stock preset button and command factory, not a
    // control.setValue substitute. Isolate its cycling list from user presets.
    auto& registry=ctx.applicationPlugin<Process::ApplicationPlugin>().presets;
    struct RestorePresets {
      std::vector<Process::Preset>& target;
      std::vector<Process::Preset> saved;
      ~RestorePresets(){target=std::move(saved);}
    } restore{registry,std::move(registry)};
    registry.assign(presets.begin(),presets.end());
    CommandDispatcher<> dispatcher{doc->context().commandStack};
    dispatcher.submit<Scenario::Command::LoadPreset>(p,presets[0]);
    REQUIRE(p.error().isEmpty());REQUIRE(p.settings().sound==SoundRole::TriangleWave);
    auto* sink=score::test::add_process(*doc,QStringLiteral("fa02fc21-80ab-41e7-a3c9-5d2fa34ecda7"),{});REQUIRE(sink);
    auto& documentModel=score::IDocument::get<Scenario::ScenarioDocumentModel>(*doc);
    dispatcher.submit<Dataflow::CreateCable>(documentModel,Id<Process::Cable>{4242},
      Process::CableType::ImmediateGlutton,*p.outlets().front(),*sink->inlets().front());
    REQUIRE(p.outlets().front()->cables().size()==1);
    auto* presenter=score::IDocument::try_presenterDelegate<Scenario::ScenarioDocumentPresenter>(*doc);REQUIRE(presenter);
    QGraphicsScene scene;
    p.setFoldMode(Process::FoldMode::Unfolded);
    std::unique_ptr<Process::NodeItem> item{new Process::NodeItem{
      p,presenter->context(),TimeVal::fromMsecs(1000.),nullptr}};
    scene.addItem(item.get());QCoreApplication::processEvents();
    score::QGraphicsDraggablePixmap* button{};
    for(auto* child:item->childItems())
      if(auto* candidate=dynamic_cast<score::QGraphicsDraggablePixmap*>(child);
         candidate&&candidate->toolTip().startsWith(QStringLiteral("Presets")))button=candidate;
    REQUIRE(button);
    score::GraphicsTabLayout* tabs{};
    for(auto* child:scene.items())if(auto* tab=dynamic_cast<score::GraphicsTabLayout*>(child))tabs=tab;
    REQUIRE(tabs);tabs->setCurrentIndex(2);
    QGraphicsView view{&scene};view.resize(900,650);view.setSceneRect(scene.itemsBoundingRect().adjusted(-20.,-20.,20.,20.));
    REQUIRE(score::test::showAndActivate(view));
    const auto clickPreset=[&](Qt::MouseButton mouseButton) {
      view.ensureVisible(button,20,20);
      const auto position=view.mapFromScene(button->mapToScene(button->boundingRect().center()));
      REQUIRE(view.viewport()->rect().contains(position));
      score::test::mouseClick(*view.viewport(),position,mouseButton);
    };
    auto& plug=doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true,score::test::base_interval(*doc));drain(plug);
    const auto& processes=plug.baseScenario()->baseInterval().processes();
    const auto found=processes.find(p.id());REQUIRE(found!=processes.end());
    const auto node=std::static_pointer_cast<Executor::Node>(found->second->node);
    REQUIRE(node->ready());auto& state=*plug.context().execState;
    const auto render=[&](std::initializer_list<libremidi::ump> messages) {
      node->midi->data.messages.assign(messages.begin(),messages.end());
      state.samples_since_start+=state.bufferSize;
      ossia::set_thread_pinned(ossia::thread_type::Audio,0);
      node->run(tokenFor(state),ossia::exec_state_facade{&state});
      ossia::set_thread_pinned(ossia::thread_type::Ui,0);
      node->midi->data.messages.clear();
      for(auto* inlet:node->controls)inlet->data.get_data().clear();
      CHECK(node->error()==nullptr);
      for(const auto& channel:node->audio->data.get())
        CHECK(std::all_of(channel.begin(),channel.end(),[](double sample){return std::isfinite(sample);}));
    };
    auto* const waveform=&waveInlet(p);
    const auto checkBindings=[&] {
      CHECK(&waveInlet(p)==waveform);
      checkAuthoredWave(p,waveform->value());
      int editors{};
      for(auto* child:scene.items())if(child->data(0).toString()==QStringLiteral("WaveformEditor"))++editors;
      CHECK(editors==1);
    };
    for(int i=0;i<64;++i) {
      INFO("native preset transition "<<i);
      render({libremidi::from_midi1::note_on(0,60,110)});
      render({});
      const auto commandIndex=doc->commandStack().currentIndex();
      clickPreset(i%2==0?Qt::ForwardButton:Qt::BackButton);
      REQUIRE(doc->commandStack().currentIndex()==commandIndex+1);
      REQUIRE(p.settings().sound==(i%2==0?SoundRole::Noise:SoundRole::TriangleWave));
      // MIDI arrives before the queued replacement is installed, then repeats
      // the same pitch after retirement, including the old matching note-off.
      render({});
      drain(plug);REQUIRE(node->ready());
      render({libremidi::from_midi1::note_on(0,60,110),libremidi::from_midi1::note_off(0,60,0)});
      checkBindings();
      score::test::flush_deferred_deletes();
      view.viewport()->repaint();
      checkBindings();
      render({libremidi::from_midi1::note_off(0,60,0)});
      if(i%8==7) {
        doc->commandStack().undo();drain(plug);checkBindings();
        doc->commandStack().redo();drain(plug);checkBindings();
      }
      REQUIRE(p.error().isEmpty());
    }
    // The stock menu runs a nested event loop: library scanning may add a
    // preset while its actions are visible. The chosen action must retain its
    // own preset even when the shared registry reallocates.
    bool menuFound{},actionFound{},registryMoved{};
    render({libremidi::from_midi1::note_on(0,48,100)});
    QTimer::singleShot(2000,&view,[] {
      while(auto* popup=qobject_cast<QMenu*>(QApplication::activePopupWidget()))
        popup->close();
    });
    QTimer::singleShot(0,&view,[&] {
      auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());
      menuFound=menu!=nullptr;
      if(!menu)return;
      std::vector<std::pair<QMenu*,QAction*>> path;
      const auto findAction=[&](auto&& self,QMenu* current)->bool {
        for(auto* action:current->actions()) {
          path.emplace_back(current,action);
          if(action->text()==presets[1].name)return true;
          if(action->menu()&&self(self,action->menu()))return true;
          path.pop_back();
        }
        return false;
      };
      actionFound=findAction(findAction,menu);
      if(!actionFound){menu->close();return;}
      const auto* oldStorage=registry.data();
      const auto additions=registry.capacity()-registry.size()+1;
      for(std::size_t i=0;i<additions;++i) {
        auto indexed=presets[0];
        indexed.name=QStringLiteral("Background indexed preset %1").arg(i);
        ctx.applicationPlugin<Process::ApplicationPlugin>().addPreset(std::move(indexed));
      }
      registryMoved=registry.data()!=oldStorage;
      for(const auto& [owner,action]:path)
        score::test::mouseClick(*owner,owner->actionGeometry(action).center());
    });
    const auto beforeMenu=doc->commandStack().currentIndex();
    clickPreset(Qt::LeftButton);
    REQUIRE(menuFound);REQUIRE(actionFound);REQUIRE(registryMoved);
    REQUIRE(doc->commandStack().currentIndex()==beforeMenu+1);
    REQUIRE(p.settings().sound==SoundRole::Noise);
    drain(plug);REQUIRE(node->ready());
    render({libremidi::from_midi1::note_off(0,48,0)});
    score::test::flush_deferred_deletes();checkBindings();
  });
}

TEST_CASE("APU queued preset engines reject stale replies and retire on the control thread", "[emulation][apu][execution][preset][retirement]") {
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc=score::test::new_document(ctx);REQUIRE(doc);
    auto& plug=doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true,score::test::base_interval(*doc));drain(plug);
    auto& state=*plug.context().execState;const auto& execution=plug.context();
    Settings triangle;selectModel(triangle,Model::Dmg);triangle.sound=SoundRole::TriangleWave;
    auto noise=triangle;noise.sound=SoundRole::Noise;
    std::array<int,4> destroyed{};bool audioThread{},destroyedOnAudio{};
    std::array<std::weak_ptr<Engine>,4> engines;
    const auto prepare=[&](int index,const Settings& settings) {
      auto engine=std::shared_ptr<Engine>{new Engine,[&,index](Engine* retired) {
        destroyedOnAudio|=audioThread;++destroyed[index];delete retired;
      }};
      REQUIRE(engine->prepare(state.sampleRate,settings));engines[index]=engine;return engine;
    };
    auto node=std::make_shared<Executor::Node>(state,triangle);
    auto initial=prepare(0,triangle);node->install(initial,triangle.model,triangle.clock);
    REQUIRE_FALSE(initial);REQUIRE(node->ready());
    engines[0].lock()->midi(std::array<uint8_t,3>{0x90,60,110});
    const auto enqueue=[&](int index,const Settings& settings,uint64_t revision) {
      execution.executionQueue.enqueue(
        [node,prepared=prepare(index,settings),model=settings.model,clock=settings.clock,revision]() mutable {
          node->install(prepared,model,clock,revision);
        });
    };
    execution.executionQueue.enqueue([node,settings=std::make_shared<const Settings>(noise)]{node->update(*settings);node->assetRevision(1);});
    enqueue(1,noise,1);
    // A later preset supersedes a prepared response even when its model and
    // clock match. Keep the old capture alive until control-thread GC.
    execution.executionQueue.enqueue([node,settings=std::make_shared<const Settings>(triangle)]{node->update(*settings);node->assetRevision(2);});
    enqueue(2,noise,1);
    bool staleRejected{};
    execution.executionQueue.enqueue([node,&staleRejected]{staleRejected=!node->ready();});
    enqueue(3,triangle,2);
    {
      Execution::ExecutionCommand command;
      audioThread=true;ossia::set_thread_pinned(ossia::thread_type::Audio,0);
      while(execution.executionQueue.try_dequeue(command)) {
        command();execution.gcQueue.enqueue(Execution::gc(std::move(command)));
      }
      ossia::set_thread_pinned(ossia::thread_type::Ui,0);audioThread=false;
    }
    REQUIRE(node->ready());CHECK(node->error()==nullptr);
    CHECK(staleRejected);
    CHECK(engines[3].lock()->settings()==triangle);
    CHECK(destroyed==std::array<int,4>{});
    for(const auto& engine:engines)CHECK_FALSE(engine.expired());
    {
      Execution::GCCommand command;
      while(execution.gcQueue.try_dequeue(command))command();
    }
    CHECK(destroyed==std::array<int,4>{1,1,1,0});
    CHECK_FALSE(destroyedOnAudio);
    node.reset();CHECK(destroyed==std::array<int,4>{1,1,1,1});
  });
}
