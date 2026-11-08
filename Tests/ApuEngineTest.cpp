// SPDX-License-Identifier: GPL-3.0-or-later
#include <Emulation/Apu/Engine.hpp>
#include <Emulation/Volume.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <vector>
namespace {
using namespace Emulation::Apu;
Settings settings(Model m) { Settings s; s.model=m; s.clock=defaultClock(m); s.routing=RoutingMode::NativeMidi; return s; }
void send(Engine& e,std::initializer_list<uint8_t> b) {
  e.midi({b.begin(),b.size()}); REQUIRE(e.error()==nullptr);
}
struct Audio { std::vector<double> l,r; bool operator==(const Audio&) const=default; };
Audio render(Engine& e,int count,int block=257) {
  Audio a{std::vector<double>(count),std::vector<double>(count)};
  for(int at=0;at<count;at+=block) e.render(a.l.data()+at,a.r.data()+at,std::min(block,count-at));
  REQUIRE(e.error()==nullptr);
  REQUIRE(std::all_of(a.l.begin(),a.l.end(),[](double v){return std::isfinite(v);}));
  REQUIRE(std::all_of(a.r.begin(),a.r.end(),[](double v){return std::isfinite(v);}));
  return a;
}
bool audible(const std::vector<double>& v) {
  const auto [a,b]=std::minmax_element(v.begin(),v.end()); return *b-*a>0.0001;
}
double frequency(const std::vector<double>& a,double rate) {
  const auto begin=a.begin()+int(rate/10);
  const auto [lo,hi]=std::minmax_element(begin,a.end());
  const double threshold=(*hi-*lo)*.15; bool armed{}; int crossings{};
  for(auto it=begin;it!=a.end();++it) {
    if(*it < -threshold) armed=true;
    if(armed&&*it>threshold) { ++crossings; armed=false; }
  }
  return crossings*rate/double(a.end()-begin);
}
constexpr std::array models{Model::NesNtsc,Model::NesPal,Model::Dmg,Model::Cgb};
}
TEST_CASE("APU default roles keep every Role default", "[emulation][apu]") {
  // Runtime checks: the miscompiled initialiser still satisfies static_assert.
  const Settings s;
  for(const auto* bank:{&s.roles,&s.inactiveRoles}) for(int i=0;i<5;++i) {
    CAPTURE(i);
    Role expected; if(i==2) expected.level=1;
    CHECK((*bank)[i]==expected);
    CHECK((*bank)[i].duty==2); CHECK((*bank)[i].rate==4); CHECK((*bank)[i].linear==127);
    CHECK((*bank)[i].left); CHECK((*bank)[i].right);
  }
}
TEST_CASE("APU native pulse and triangle/wave timers track host rate", "[emulation][apu]") {
  for(auto m:models) for(double rate:{44100.,48000.,96000.}) for(int role:{0,1,2}) {
    Engine e; REQUIRE(e.prepare(rate,settings(m)));
    send(e,{uint8_t(0x90+role),69,127});
    const auto a=render(e,int(rate/2)); REQUIRE(audible(a.l));
    CHECK(std::abs(frequency(a.l,rate)-440.)<8.);
  }
}
TEST_CASE("APU clocks retain sample phase across arbitrary render partitions", "[emulation][apu]") {
  for(auto m:models) for(double rate:{8000.,44100.,48000.,96000.,192000.}) {
    CAPTURE(int(m),rate);
    auto s=settings(m); s.roles[0].envelopePeriod=3; s.roles[0].envelopeEnable=true;
    s.roles[0].sweepPeriod=3; s.roles[0].sweepShift=2; s.roles[0].sweepNegate=true; s.roles[0].sweepEnable=true;
    Engine a,b,isolated; REQUIRE(a.prepare(rate,s)); REQUIRE(b.prepare(rate,s)); REQUIRE(isolated.prepare(rate,s));
    send(a,{0x90,64,127}); send(b,{0x90,64,127});
    send(a,{0x93,48,127}); send(b,{0x93,48,127});
    CHECK(render(a,12000,12000)==render(b,12000,37));
    CHECK_FALSE(audible(render(isolated,1000).l));
  }
}
TEST_CASE("APU roles enforce physical resources and FIFO stolen-note lifetimes", "[emulation][apu]") {
  for(auto m:models) {
    Engine e; REQUIRE(e.prepare(48000,settings(m)));
    send(e,{0x90,60,127}); send(e,{0x90,60,90}); send(e,{0x90,64,127}); send(e,{0x90,60,127});
    REQUIRE(e.activeVoices()==1);
    send(e,{0x80,60,0}); CHECK(e.activeVoices()==1);
    send(e,{0x80,64,0}); CHECK(e.activeVoices()==1);
    send(e,{0x80,60,0}); CHECK(e.activeVoices()==1);
    send(e,{0xb0,64,127}); send(e,{0x80,60,0}); CHECK(e.activeVoices()==1);
    send(e,{0xb0,121,0}); CHECK(e.activeVoices()==0);
    send(e,{0x91,69,127}); send(e,{0xb1,64,127}); send(e,{0xb1,123,0}); CHECK(e.activeVoices()==1);
    send(e,{0xb1,64,0}); CHECK(e.activeVoices()==0);
    send(e,{0x92,60,127}); send(e,{0xb2,120,0}); CHECK(e.activeVoices()==0);
    for(int i=0;i<(int(m)<2?5:4);++i) send(e,{uint8_t(0x90+i),60,127});
    CHECK(e.activeVoices()==(int(m)<2?5:4));
    e.midi(std::array<uint8_t,3>{0x9f,60,127}); REQUIRE(e.error()!=nullptr);
    send(e,{0xff}); CHECK(e.activeVoices()==0); CHECK_FALSE(audible(render(e,1000).l));
  }
}
TEST_CASE("APU hardware length clocks expire and note retriggers reload them", "[emulation][apu]") {
  for(auto m:models) for(int role:{0,1,2,3}) {
    auto s=settings(m); s.roles[role].lengthEnable=true;
    s.roles[role].length=int(m)<2?3:role==2?255:63;
    Engine e; REQUIRE(e.prepare(48000,s)); send(e,{uint8_t(0x90+role),69,127});
    REQUIRE(e.activeVoices()==1); render(e,2400); CHECK(e.activeVoices()==0);
    send(e,{uint8_t(0x90+role),69,127}); CHECK(e.activeVoices()==1);
    render(e,2400); CHECK(e.activeVoices()==0);
  }
}
TEST_CASE("APU native sweep changes pulse timbre without fictional sweep on pulse two GB", "[emulation][apu]") {
  for(auto m:models) {
    auto s=settings(m); auto swept=s;
    swept.roles[0].sweepPeriod=2; swept.roles[0].sweepShift=2; swept.roles[0].sweepNegate=true; swept.roles[0].sweepEnable=true;
    Engine a,b; REQUIRE(a.prepare(48000,s)); REQUIRE(b.prepare(48000,swept));
    send(a,{0x90,60,127}); send(b,{0x90,60,127}); CHECK(render(a,12000)!=render(b,12000));
  }
  for(auto m:{Model::Dmg,Model::Cgb}) {
    auto s=settings(m); auto swept=s; swept.roles[1].sweepPeriod=2; swept.roles[1].sweepShift=2;
    Engine a,b; REQUIRE(a.prepare(48000,s)); REQUIRE(b.prepare(48000,swept));
    send(a,{0x91,60,127}); send(b,{0x91,60,127}); CHECK(render(a,12000)==render(b,12000));
  }
}
TEST_CASE("APU native noise modes and editable wave RAM produce distinct signals", "[emulation][apu]") {
  for(auto m:models) {
    auto s=settings(m),shortNoise=s; shortNoise.roles[3].noiseShort=true;
    Engine a,b; REQUIRE(a.prepare(48000,s)); REQUIRE(b.prepare(48000,shortNoise));
    send(a,{0x93,60,127}); send(b,{0x93,60,127});
    const auto x=render(a,6000),y=render(b,6000); CHECK(audible(x.l)); CHECK(audible(y.l)); CHECK(x!=y);
  }
  for(auto m:{Model::Dmg,Model::Cgb}) {
    auto s=settings(m); Engine a; REQUIRE(a.prepare(48000,s)); send(a,{0x92,69,127});
    REQUIRE(audible(render(a,6000).l)); s.wave.fill(0); REQUIRE(a.configure(s));
    render(a,24000); CHECK_FALSE(audible(render(a,24000).l));
    s.wave=settings(m).wave; REQUIRE(a.configure(s)); CHECK(audible(render(a,6000).l));
  }
}
TEST_CASE("APU fixed sample RAM preserves all sixteen levels on initial and live updates", "[emulation][apu][waveform]") {
  std::array<uint8_t,32> samples{};
  for(int i=0;i<32;++i)samples[i]=(i*7)%16;
  for(auto model:{Model::Dmg,Model::Cgb}) {
    auto edited=settings(model);edited.wave=samples;
    Engine initial,updated,triangle;
    REQUIRE(initial.prepare(48000,edited));
    REQUIRE(updated.prepare(48000,settings(model)));
    REQUIRE(triangle.prepare(48000,settings(model)));
    REQUIRE(updated.configure(edited));CHECK(updated.settings().wave==samples);
    send(initial,{0x92,69,127});send(updated,{0x92,69,127});send(triangle,{0x92,69,127});
    const auto actual=render(initial,6000);
    CHECK(actual==render(updated,6000));CHECK(actual!=render(triangle,6000));CHECK(audible(actual.l));
    edited.wave.fill(0);REQUIRE(initial.configure(edited));REQUIRE(updated.configure(edited));
    CHECK(render(initial,6000)==render(updated,6000));
    edited.wave=samples;REQUIRE(initial.configure(edited));REQUIRE(updated.configure(edited));
    const auto resumed=render(initial,6000);CHECK(resumed==render(updated,6000));CHECK(audible(resumed.l));
  }
}
TEST_CASE("APU GB stereo routing and model-specific capacitor response are real", "[emulation][apu]") {
  for(auto m:{Model::Dmg,Model::Cgb}) {
    auto s=settings(m); s.roles[0].right=false;
    Engine e; REQUIRE(e.prepare(48000,s)); send(e,{0x90,69,127}); const auto a=render(e,6000);
    CHECK(audible(a.l)); CHECK_FALSE(audible(a.r));
    s.roles[0].right=true; s.roles[0].left=false; REQUIRE(e.configure(s));
    render(e,24000); const auto b=render(e,6000); CHECK_FALSE(audible(b.l)); CHECK(audible(b.r));
  }
  Engine dmg,cgb; REQUIRE(dmg.prepare(48000,settings(Model::Dmg))); REQUIRE(cgb.prepare(48000,settings(Model::Cgb)));
  send(dmg,{0x90,69,127}); send(cgb,{0x90,69,127}); CHECK(render(dmg,6000)!=render(cgb,6000));
}
TEST_CASE("APU DMC reads authored bytes through native address length loop and rate", "[emulation][apu]") {
  auto s=settings(Model::NesNtsc); s.dmcLength=16; s.roles[4].rate=15;
  Engine e; REQUIRE(e.prepare(48000,s)); send(e,{0x94,60,127});
  CHECK(audible(render(e,12000).l)); CHECK(e.activeVoices()==0);
  s.dmcLoop=true; REQUIRE(e.configure(s)); send(e,{0x94,60,127}); render(e,48000); CHECK(e.activeVoices()==1);
  send(e,{0xb4,120,0}); CHECK(e.activeVoices()==0);
  auto data=defaultDmc(); auto inverse=data;
  for(int i=0;i<inverse.size;++i) inverse.bytes[i]^=255;
  Engine a,b; REQUIRE(a.prepare(48000,s,data)); REQUIRE(b.prepare(48000,s,inverse));
  send(a,{0x94,60,127}); send(b,{0x94,60,127}); CHECK(render(a,6000)!=render(b,6000));
  auto offset=s; offset.dmcAddress=1;
  Engine c,d; REQUIRE(c.prepare(48000,s,data)); REQUIRE(d.prepare(48000,offset,data));
  send(c,{0x94,60,127}); send(d,{0x94,60,127}); CHECK(render(c,12000)!=render(d,12000));
  auto slower=s; slower.roles[4].rate=0;
  Engine fast,slow; REQUIRE(fast.prepare(48000,s)); REQUIRE(slow.prepare(48000,slower));
  send(fast,{0x94,60,127}); send(slow,{0x94,60,127}); CHECK(render(fast,12000)!=render(slow,12000));
}
TEST_CASE("APU settings reject atomically and Volume changes need no MIDI note", "[emulation][apu]") {
  for(auto m:models) {
    auto s=settings(m); Engine e; REQUIRE(e.prepare(48000,s));
    s.volume=0.; REQUIRE(e.configure(s)); CHECK(e.settings()==s);
    auto bad=s; bad.wave[7]=16; CHECK_FALSE(e.configure(bad)); CHECK(e.settings()==s);
    for(double invalid : {-1., double(Emulation::MaxVolume) + .01,
        std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
      bad=s; bad.volume=invalid;
      CHECK(validate(bad)); CHECK_FALSE(e.configure(bad)); CHECK(e.settings()==s);
    }
    DmcData empty; CHECK_FALSE(e.prepare(48000,s,empty)); CHECK(e.settings()==s);
    CHECK_FALSE(e.prepare(0,s)); CHECK(e.settings()==s);
    s.volume=.7; REQUIRE(e.configure(s)); send(e,{0x90,69,127}); CHECK(audible(render(e,2000).l));
    s.volume=0.; REQUIRE(e.configure(s)); const auto zero=render(e,2000); CHECK(std::all_of(zero.l.begin(),zero.l.end(),[](double x){return x==0.;}));
    s.volume=.7; REQUIRE(e.configure(s)); CHECK(audible(render(e,2000).l));
  }
}
TEST_CASE("APU boosted Volume scales native audio without restarting hardware envelopes", "[emulation][apu]") {
  for(auto model:models) {
    auto s=settings(model); s.volume=1.;
    s.roles[0].envelopeEnable=true; s.roles[0].envelopePeriod=3;
    Engine reference,adjusted;
    REQUIRE(reference.prepare(48000,s)); REQUIRE(adjusted.prepare(48000,s));
    send(reference,{0x90,69,127}); send(adjusted,{0x90,69,127});
    for(double gain:{.25,4.5,double(Emulation::MaxVolume),0.,1.}) {
      s.volume=gain; REQUIRE(adjusted.configure(s));
      const auto a=render(reference,4096), b=render(adjusted,4096,31);
      REQUIRE(audible(a.l));
      for(std::size_t i=0;i<a.l.size();++i) {
        CHECK(b.l[i]==a.l[i]*gain); CHECK(b.r[i]==a.r[i]*gain);
      }
    }
  }
}
TEST_CASE("APU automatic clocks follow the selected console without replacing explicit overrides", "[emulation][apu]") {
  for(auto model:models) {
    Settings automatic; automatic.model=model;
    auto explicitClock=automatic; explicitClock.clock=defaultClock(model);
    Engine a,b; REQUIRE(a.prepare(48000,automatic)); REQUIRE(b.prepare(48000,explicitClock));
    send(a,{0x90,69,127}); send(b,{0x90,69,127});
    CHECK(render(a,6000)==render(b,6000));
    auto invalid=automatic; invalid.clock=1;
    CHECK_FALSE(a.configure(invalid)); CHECK(a.settings()==automatic);
  }
  Settings override; override.clock=2000000; override.model=Model::Cgb;
  CHECK(effectiveClock(override)==2000000);
  override.model=Model::NesPal; CHECK(effectiveClock(override)==2000000);
}
TEST_CASE("APU exhausted DMC does not restart when another role changes enable state", "[emulation][apu]") {
  auto s=settings(Model::NesNtsc); s.roles[4].rate=15; s.dmcLength=0;
  Engine e; REQUIRE(e.prepare(48000,s));
  send(e,{0x94,60,127}); render(e,2000); REQUIRE(e.activeVoices()==0);
  send(e,{0x90,69,127}); CHECK(e.activeVoices()==1);
  send(e,{0x80,69,0}); CHECK(e.activeVoices()==0);
  send(e,{0x94,60,127}); CHECK(e.activeVoices()==1);
}
TEST_CASE("APU NES envelope rate zero is distinct from constant volume", "[emulation][apu]") {
  Settings constant; constant.roles[0].level=0;
  auto envelope=constant; envelope.roles[0].envelopeEnable=true;
  Engine a,b; REQUIRE(a.prepare(48000,constant)); REQUIRE(b.prepare(48000,envelope));
  send(a,{0x90,69,127}); send(b,{0x90,69,127});
  CHECK_FALSE(audible(render(a,3000).l)); CHECK(audible(render(b,3000).l));
}
TEST_CASE("APU NES status inspection does not advance chip time or disrupt same-sample writes", "[emulation][apu]") {
  for(auto model:{Model::NesNtsc,Model::NesPal}) {
    Engine observed,reference;
    REQUIRE(observed.prepare(48000,settings(model))); REQUIRE(reference.prepare(48000,settings(model)));
    CHECK(observed.activeVoices()==0); CHECK(observed.activeVoices()==0);
    send(observed,{0x90,69,127}); send(reference,{0x90,69,127});
    CHECK(observed.activeVoices()==1); CHECK(observed.activeVoices()==1);
    send(observed,{0x91,64,127}); send(reference,{0x91,64,127});
    CHECK(render(observed,1000)==render(reference,1000));
    CHECK(observed.activeVoices()==2); CHECK(observed.activeVoices()==2);
    send(observed,{0x80,69,0}); send(reference,{0x80,69,0});
    CHECK(render(observed,1000)==render(reference,1000));
  }
}
TEST_CASE("APU Game Boy wave right-only routing remains audible after initial transitions", "[emulation][apu]") {
  for(auto model:{Model::Dmg,Model::Cgb}) {
    CAPTURE(int(model));
    auto s=settings(model); s.roles[2].left=false; s.roles[2].right=true;
    Engine e; REQUIRE(e.prepare(48000,s)); send(e,{0x92,69,127});
    render(e,6000); const auto a=render(e,6000);
    double leftEnergy{},rightEnergy{};
    for(int i=0;i<6000;++i) { leftEnergy+=a.l[i]*a.l[i]; rightEnergy+=a.r[i]*a.r[i]; }
    CHECK(leftEnergy==0.); CHECK(std::sqrt(rightEnergy/6000.)>.001);
  }
}

TEST_CASE("APU invalid hardware register settings never replace the accepted engine patch", "[emulation][apu]") {
  for(auto model:models) {
    auto accepted=settings(model);
    accepted.roles[0].length=int(model)<2?31:63;
    accepted.roles[2].length=int(model)<2?31:255;
    accepted.roles[0].envelopePeriod=int(model)<2?15:7;
    accepted.roles[3].rate=int(model)<2?15:127;
    accepted.roles[4].rate=15;
    Engine engine;REQUIRE(engine.prepare(48000,accepted));
    for(int field=0;field<5;++field) {
      auto invalid=accepted;
      switch(field) {
        case 0:++invalid.roles[0].length;break;
        case 1:++invalid.roles[2].length;break;
        case 2:++invalid.roles[0].envelopePeriod;break;
        case 3:++invalid.roles[3].rate;break;
        case 4:if(int(model)<2)++invalid.roles[4].rate;else invalid.roles[2].level=4;break;
      }
      CHECK_FALSE(engine.configure(invalid));CHECK(engine.settings()==accepted);
      engine.midi(std::array<uint8_t,3>{0x90,69,127});
      CHECK(engine.error()!=nullptr);
      REQUIRE(engine.configure(accepted));CHECK(engine.error()==nullptr);
    }
    engine.midi(std::array<uint8_t,3>{0x9f,69,127});REQUIRE(engine.error());
    send(engine,{0x90,69,127});CHECK(audible(render(engine,2000).l));
  }
}

TEST_CASE("APU Mono sound choice plays every physical role from channel one", "[emulation][apu][routing]") {
  for(auto model:models) for(int role=0;role<(int(model)<2?5:4);++role) {
    CAPTURE(int(model),role);
    Settings instrument;instrument.model=model;instrument.sound=SoundRole(role);instrument.dmcLoop=true;
    auto native=instrument;native.routing=RoutingMode::NativeMidi;
    Engine played,reference;REQUIRE(played.prepare(48000,instrument));REQUIRE(reference.prepare(48000,native));
    send(played,{0x90,69,127});send(reference,{uint8_t(0x90+role),69,127});
    const auto actual=render(played,4096);CHECK(audible(actual.l));CHECK(actual==render(reference,4096));
    send(played,{0xe0,0,80});send(reference,{uint8_t(0xe0+role),0,80});
    CHECK(render(played,1024)==render(reference,1024));
    send(played,{0xb0,64,127});send(played,{0x80,69,0});CHECK(played.activeVoices()==1);
    send(played,{0xb0,64,0});CHECK(played.activeVoices()==0);
    send(played,{0x90,69,127});send(played,{0xb0,123,0});CHECK(played.activeVoices()==0);
    played.midi(std::array<uint8_t,3>{0x91,69,127});CHECK(played.midiError()!=nullptr);
    send(played,{0x90,69,127});CHECK(played.activeVoices()==1);
    send(played,{0xb0,120,0});CHECK(played.activeVoices()==0);
  }
}

TEST_CASE("APU routing transitions stop voices and preserve source-channel note-off debt", "[emulation][apu][routing]") {
  for(auto model:models) {
    Settings s;s.model=model;
    Engine e;REQUIRE(e.prepare(48000,s));
    send(e,{0x90,60,127});send(e,{0x90,60,100});send(e,{0xb0,64,127});
    s.sound=SoundRole::TriangleWave;REQUIRE(e.configure(s));CHECK(e.activeVoices()==0);
    send(e,{0x90,60,127});
    send(e,{0x80,60,0});CHECK(e.activeVoices()==1);
    send(e,{0x90,60,0});CHECK(e.activeVoices()==1);
    send(e,{0x80,60,0});CHECK(e.activeVoices()==1); // Sustain follows the source channel.
    send(e,{0xb0,64,0});CHECK(e.activeVoices()==0);
    s.sound=SoundRole::Pulse1;REQUIRE(e.configure(s));CHECK(e.activeVoices()==0);
    send(e,{0x90,60,127});
    s.routing=RoutingMode::NativeMidi;REQUIRE(e.configure(s));CHECK(e.activeVoices()==0);
    send(e,{0x90,60,127});send(e,{0x80,60,0});CHECK(e.activeVoices()==1);
    send(e,{0x80,60,0});CHECK(e.activeVoices()==0);
    send(e,{0x91,64,127});
    s.sound=SoundRole::Noise;REQUIRE(e.configure(s));CHECK(e.activeVoices()==1); // Not musical in Native MIDI.
    s.routing=RoutingMode::Instrument;REQUIRE(e.configure(s));CHECK(e.activeVoices()==0);
    send(e,{0x81,64,0}); // Former native channel is cleanup only.
    send(e,{0x90,67,127});send(e,{0xb0,64,127});send(e,{0x80,67,0});
    REQUIRE(e.activeVoices()==1);
    s.sound=SoundRole::Pulse2;REQUIRE(e.configure(s));CHECK(e.activeVoices()==0);
    send(e,{0xb0,64,0});CHECK(e.activeVoices()==0);
    s.sound=SoundRole::Noise;REQUIRE(e.configure(s));CHECK(e.activeVoices()==0);
    s.routing=RoutingMode::NativeMidi;REQUIRE(e.configure(s));
    send(e,{0x91,64,127});send(e,{0x81,64,0});CHECK(e.activeVoices()==0);
    e.panic();CHECK_FALSE(audible(render(e,1000).l));
  }
}

TEST_CASE("APU routing and architecture sound validation is atomic", "[emulation][apu][routing]") {
  for(auto model:models) {
    Settings accepted;accepted.model=model;
    Engine e;REQUIRE(e.prepare(48000,accepted));send(e,{0x90,60,127});
    for(int field=0;field<3;++field) {
      auto rejected=accepted;
      if(field==0)rejected.routing=RoutingMode(2);
      if(field==1)rejected.sound=SoundRole(int(model)<2?5:4);
      if(field==2)rejected.inactiveSound=SoundRole(int(model)<2?4:5);
      CHECK_FALSE(e.configure(rejected));CHECK(e.settings()==accepted);CHECK(e.activeVoices()==1);
      REQUIRE(e.configure(accepted));
    }
    send(e,{0x80,60,0});CHECK(e.activeVoices()==0);
  }
}

TEST_CASE("APU last-note fallback restores pitch and velocity on every monophonic role", "[emulation][apu][allocation]") {
  for(auto model:models)
    for(auto allocation:{RoutingMode::Instrument,RoutingMode::NativeMidi})
      for(int role=0;role<(int(model)<2?5:4);++role) {
        CAPTURE(int(model),int(allocation),role);
        auto s=settings(model);s.routing=allocation;s.sound=SoundRole(role);s.dmcLoop=true;
        Engine played,reference;REQUIRE(played.prepare(48000,s));REQUIRE(reference.prepare(48000,s));
        const auto ch=uint8_t(allocation==RoutingMode::Instrument?0:role);
        const auto on=uint8_t(0x90+ch),off=uint8_t(0x80+ch);
        for(auto* e:{&played,&reference}) {
          send(*e,{on,60,37});send(*e,{on,64,105});send(*e,{on,67,90});send(*e,{on,64,68});
        }
        CHECK(render(played,1024)==render(reference,1024));
        send(played,{off,64,0}); // First lifetime cannot stop the retrigger.
        CHECK(render(played,1024)==render(reference,1024));
        send(played,{off,64,0});send(reference,{on,67,90});
        CHECK(played.activeVoices()==1);
        CHECK(render(played,1024)==render(reference,1024));
        send(played,{off,67,0});send(reference,{on,60,37});
        CHECK(played.activeVoices()==1);
        CHECK(render(played,1024)==render(reference,1024));
        send(played,{off,60,0});CHECK(played.activeVoices()==0);
      }
}

TEST_CASE("APU released background notes cannot resurrect and sustain remains channel scoped", "[emulation][apu][allocation]") {
  for(auto model:models) {
    auto s=settings(model);
    Engine played,reference;REQUIRE(played.prepare(48000,s));REQUIRE(reference.prepare(48000,s));
    for(auto* e:{&played,&reference}) {
      send(*e,{0x90,60,41});send(*e,{0x90,64,110});send(*e,{0x90,67,70});
      send(*e,{0x91,69,127});send(*e,{0xb1,64,127});send(*e,{0x81,69,0});
      send(*e,{0xe0,0,80});send(*e,{0xe1,0,48});
    }
    send(played,{0x80,64,0}); // A stolen, released note is no longer a candidate.
    send(played,{0x80,67,0});send(reference,{0x90,60,41});
    CHECK(played.activeVoices()==2);
    CHECK(render(played,4096)==render(reference,4096));
    send(played,{0x80,60,0});CHECK(played.activeVoices()==1);
    send(played,{0xb0,121,0});CHECK(played.activeVoices()==1);
    send(played,{0xb1,64,0});CHECK(played.activeVoices()==0);
    send(played,{0xb0,64,127});send(played,{0x90,60,41});send(played,{0x90,64,110});
    send(played,{0x80,64,0});CHECK(played.activeVoices()==1);
    send(played,{0xb0,64,0});CHECK(played.activeVoices()==1); // Falls back to held 60.
    send(played,{0x80,60,0});CHECK(played.activeVoices()==0);
  }
}

TEST_CASE("APU controller stops retain old FIFO debt without restoring cancelled fallback notes", "[emulation][apu][allocation]") {
  for(auto model:models)
    for(auto allocation:{RoutingMode::Instrument,RoutingMode::NativeMidi})
      for(uint8_t cc:{120,123})for(bool pedal:{false,true}) {
        CAPTURE(int(model),int(allocation),int(cc),pedal);
        auto s=settings(model);s.routing=allocation;
        Engine e;REQUIRE(e.prepare(48000,s));
        send(e,{0x90,60,45});send(e,{0x90,62,90});
        if(pedal)send(e,{0xb0,64,127});
        send(e,{0xb0,cc,0});
        CHECK(e.activeVoices()==(cc==123&&pedal?1:0));
        send(e,{0x90,60,100});send(e,{0x90,60,80});
        send(e,{0x80,60,0});CHECK(e.activeVoices()==1);
        send(e,{0x80,60,0});CHECK(e.activeVoices()==1);
        send(e,{0xb0,64,0});CHECK(e.activeVoices()==1);
        send(e,{0x80,60,0});CHECK(e.activeVoices()==0);
        send(e,{0x80,62,0});CHECK(e.activeVoices()==0);
      }
}

TEST_CASE("APU allocation changes keep channel bend and sustain but discard old fallback notes", "[emulation][apu][allocation]") {
  for(auto model:models) {
    auto s=settings(model);
    Engine played,reference;REQUIRE(played.prepare(48000,s));
    send(played,{0xe0,0,80});send(played,{0xb0,64,127});
    send(played,{0xe1,0,48});send(played,{0xb1,64,127});
    send(played,{0x90,60,30});send(played,{0x90,64,100});send(played,{0x91,67,60});
    s.routing=RoutingMode::Instrument;s.sound=SoundRole::Pulse2;
    REQUIRE(played.configure(s));CHECK(played.activeVoices()==0);
    REQUIRE(reference.prepare(48000,s));
    send(reference,{0xe0,0,80});send(reference,{0xb0,64,127});
    send(played,{0x90,69,50});send(reference,{0x90,69,50});
    CHECK(render(played,4096)==render(reference,4096));
    send(played,{0x80,69,0});CHECK(played.activeVoices()==1);
    send(played,{0xb0,64,0});CHECK(played.activeVoices()==0);
    // Cleanup on an inactive native channel must not erase its old note debt.
    send(played,{0xb1,120,0});send(played,{0xb1,64,0});
    s.routing=RoutingMode::NativeMidi;REQUIRE(played.configure(s));
    send(played,{0x91,67,80});send(played,{0x81,67,0});CHECK(played.activeVoices()==1);
    send(played,{0x81,67,0});CHECK(played.activeVoices()==0);
    send(played,{0x80,64,0});send(played,{0x80,60,0});CHECK(played.activeVoices()==0);
  }
}
