// SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine.hpp"
#include <Emulation/Volume.hpp>
#include <Nes_Apu.h>
#include <Blip_Buffer.h>
#include <3rdparty/apu/gb_apu/gb_apu.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
namespace Emulation::Apu {
namespace {
bool nes(Model m) noexcept { return m==Model::NesNtsc || m==Model::NesPal; }
bool in(int n,int lo,int hi) noexcept { return n>=lo && n<=hi; }
}
int defaultClock(Model m) noexcept { return m==Model::NesNtsc?1789773:m==Model::NesPal?1662607:4194304; }
int effectiveClock(const Settings& s) noexcept { return s.clock?s.clock:defaultClock(s.model); }
DmcData defaultDmc() noexcept {
  // Authored delta-bit triangle bursts, not an extracted sample or ROM.
  DmcData d; d.size=257;
  for(int i=0;i<d.size;++i) d.bytes[i]=(i%16<8)?0xff:0x00;
  return d;
}
const char* validate(const DmcData& d) noexcept {
  return in(d.size,1,DmcData::capacity)?nullptr:"DMC data must contain 1 through 16384 raw bytes.";
}
const char* validate(const Settings& s) noexcept {
  if(!in(int(s.model),0,3)) return "Unknown Console APU model.";
  if(!in(int(s.routing),0,1)) return "Unknown APU MIDI routing mode.";
  if(!in(int(s.sound),0,nes(s.model)?4:3)
      ||!in(int(s.inactiveSound),0,nes(s.model)?3:4))
    return "APU sound must name an available role in its architecture.";
  if(s.clock!=0&&!in(s.clock,100000,20000000)) return "APU clock must be 0 (automatic) or 100000 through 20000000 Hz.";
  if(!std::isfinite(s.volume)||s.volume<0.||s.volume>MaxVolume) return "APU global volume must be finite and within 0 through 32.";
  if(!in(s.dmcAddress,0,255)||!in(s.dmcLength,0,255)||!in(s.dmcLevel,0,127)) return "Invalid native DMC address, length or DAC level.";
  for(auto v:s.wave) if(v>15) return "Game Boy wave RAM needs exactly 32 four-bit samples.";
  const auto rolesValid=[](const std::array<Role,5>& roles,bool isNes)->const char* {
    for(int i=0;i<5;++i) {
      const auto& r=roles[i];
      if(!in(r.duty,0,3)||!in(r.length,0,255)||!in(r.level,0,15)||!in(r.envelopePeriod,0,15)
         ||!in(r.sweepPeriod,0,7)||!in(r.sweepShift,0,7)||!in(r.rate,0,127)||!in(r.linear,0,127))
        return "Invalid APU role register value.";
      if(isNes) {
        if(i<4 && r.length>31) return "NES length is a native 5-bit table index.";
        if(i>=3 && r.rate>15) return "NES noise and DMC rates are 4-bit table indices.";
      } else if(i<4) {
        if(i!=2 && r.length>63) return "Game Boy pulse/noise length is a 6-bit preload.";
        if(i!=2 && r.envelopePeriod>7) return "Game Boy envelope period is 3-bit.";
        if(i==2 && r.level>3) return "Game Boy wave level is 0=mute, 1=full, 2=half, 3=quarter.";
      }
    }
    return nullptr;
  };
  if(auto e=rolesValid(s.roles,nes(s.model)))return e;
  if(auto e=rolesValid(s.inactiveRoles,!nes(s.model)))return e;
  return nullptr;
}
struct Engine::Impl {
  Settings state{}; DmcData data{};
  std::unique_ptr<Nes_Apu> na; std::unique_ptr<Blip_Buffer> nb;
  GbApu* gb{};
  const char* failure{}; const char* midiFailure{}; bool prepared{};
  int frameCountdown{8192}; int enables{};
  struct Owner {
    int note{-1}, velocity{127}, channel{};
  };
  struct Note {
    uint64_t order{};
    int velocity{};
    bool released{};
  };
  struct Channel {
    std::array<Note,128> notes{};
    uint64_t order{};
    int bend{};
    bool pedal{};
  };
  std::array<Owner,5> owners{};
  std::array<Channel,5> channels{};
  // Input-channel FIFO ownership outlives a sound/routing transition. A new
  // same-pitch note owes the old note-offs before its own release can stop it.
  std::array<std::array<uint16_t,128>,5> held{};
  std::array<short,1024> pcm{};
  ~Impl() { if(gb) apu_quit(gb); }
  static int readDmc(void* p,nes_addr_t address) {
    const auto& d=static_cast<Impl*>(p)->data;
    const int offset=int(address)-0xc000;
    return offset>=0 && offset<d.size?d.bytes[offset]:0;
  }
  void nw(int address,int value) noexcept { na->write_register(0,address,value); }
  void gw(int address,int value) noexcept { apu_write_io(gb,address,value,0); }
  int timer(int role) const noexcept {
    const auto& o=owners[role];
    const double hz=440.*std::exp2((o.note-69.+double(channels[o.channel].bend)/8192.*2.)/12.);
    const double divider=nes(state.model)?(role==2?32.:16.):(role==2?64.:32.);
    const double period=double(effectiveClock(state))/(divider*hz);
    return std::clamp(int(std::lround(nes(state.model)?period-1.:2048.-period)),0,2047);
  }
  int level(int role) const noexcept {
    return (state.roles[role].level*owners[role].velocity+63)/127;
  }
  void setNesEnable(int role,bool on) noexcept {
    // Writing $4015 with stale DMC-enable would restart an exhausted sample.
    if(role!=4) enables=(enables&15)|(na->active_channels()&16);
    if(on) enables|=1<<role; else enables&=~(1<<role);
    nw(0x4015,enables);
  }
  void routing() noexcept {
    int route{};
    for(int i=0;i<4;++i) route|=(int(state.roles[i].left)<<(i+4))|(int(state.roles[i].right)<<i);
    gw(0x24,0x77); gw(0x25,route);
  }
  void wave() noexcept {
    gw(0x1a,0);
    for(int i=0;i<16;++i) gw(0x30+i,(state.wave[2*i]<<4)|state.wave[2*i+1]);
  }
  void nesParameters(int i,const Role* old=nullptr) noexcept {
    const auto& r=state.roles[i];
    if(i==4) {
      nw(0x4010,(state.dmcLoop?0x40:0)|r.rate);
      nw(0x4012,state.dmcAddress); nw(0x4013,state.dmcLength); return;
    }
    const int base=0x4000+4*i;
    if(i==2) {
      if(!old||r.linear!=old->linear||r.lengthEnable!=old->lengthEnable)
        nw(base,(!r.lengthEnable?0x80:0)|r.linear);
    } else {
      if(!old||r.duty!=old->duty||r.lengthEnable!=old->lengthEnable||r.envelopePeriod!=old->envelopePeriod||r.envelopeEnable!=old->envelopeEnable||r.level!=old->level)
        nw(base,(i<2?r.duty<<6:0)|(!r.lengthEnable?0x20:0)|(r.envelopeEnable?r.envelopePeriod:0x10|level(i)));
      if(i<2 && (!old||r.sweepPeriod!=old->sweepPeriod||r.sweepNegate!=old->sweepNegate||r.sweepShift!=old->sweepShift||r.sweepEnable!=old->sweepEnable))
        nw(base+1,(r.sweepEnable?0x80:0)|(r.sweepPeriod<<4)|(r.sweepNegate?8:0)|r.sweepShift);
      if(i==3 && (!old||r.rate!=old->rate||r.noiseShort!=old->noiseShort)) nw(base+2,(r.noiseShort?0x80:0)|r.rate);
    }
    if(old && r.length!=old->length && owners[i].note>=0)
      nw(base+3,(r.length<<3)|(i<3?(timer(i)>>8):0));
  }
  void gbParameters(int i,const Role* old=nullptr) noexcept {
    const auto& r=state.roles[i];
    static constexpr int base[]{0x10,0x15,0x1a,0x1f};
    const int b=base[i];
    if(i==0 && (!old||r.sweepPeriod!=old->sweepPeriod||r.sweepNegate!=old->sweepNegate||r.sweepShift!=old->sweepShift))
      gw(b,(r.sweepPeriod<<4)|(r.sweepNegate?8:0)|r.sweepShift);
    if(!old||r.duty!=old->duty||r.length!=old->length)
      gw(b+1,(i<2?r.duty<<6:0)|r.length);
    if(i==2) {
      if(!old||r.level!=old->level) gw(b+2,r.level<<5);
    } else if(!old||r.level!=old->level||r.envelopeUp!=old->envelopeUp||r.envelopePeriod!=old->envelopePeriod) {
      // A released role must not re-enable its DAC through parameter automation.
      gw(b+2,owners[i].note<0?0:(level(i)<<4)|(r.envelopeUp?8:0)|r.envelopePeriod);
    }
    if(i==3 && (!old||r.rate!=old->rate||r.noiseShort!=old->noiseShort))
      gw(0x22,((r.rate/8)<<4)|(r.rate%8)|(r.noiseShort?8:0));
    if(old && r.lengthEnable!=old->lengthEnable)
      gw(b+4,(r.lengthEnable?0x40:0)|(i<3&&owners[i].note>=0?(timer(i)>>8):0));
  }
  void pitch(int i,bool trigger) noexcept {
    if(owners[i].note<0) return;
    const auto& r=state.roles[i];
    if(nes(state.model)) {
      if(i==4) {
        if(trigger) { nw(0x4011,state.dmcLevel); setNesEnable(4,false); setNesEnable(4,true); }
        return;
      }
      const int b=0x4000+4*i;
      if(i<3) { const int t=timer(i); nw(b+2,t&255); nw(b+3,(r.length<<3)|(t>>8)); }
      else if(trigger) nw(b+3,r.length<<3);
    } else {
      static constexpr int b[]{0x10,0x15,0x1a,0x1f};
      const int t=i<3?timer(i):0;
      if(i<3) gw(b[i]+3,t&255);
      if(i==2 && trigger) gw(0x1a,0x80);
      gw(b[i]+4,(trigger?0x80:0)|(r.lengthEnable?0x40:0)|(t>>8));
    }
  }
  void play(int i,int channel,int note) noexcept {
    auto& o=owners[i];
    o.note=note; o.channel=channel; o.velocity=channels[channel].notes[note].velocity;
    if(nes(state.model)) { if(i!=4) setNesEnable(i,true); nesParameters(i); }
    else gbParameters(i);
    pitch(i,true);
  }
  void start(int i,int channel,int note,int velocity) noexcept {
    auto& count=held[channel][note];
    auto& c=channels[channel];
    if(count==std::numeric_limits<uint16_t>::max()
        ||c.order==std::numeric_limits<uint64_t>::max()) {
      reset(); midiFailure="APU note ownership overflow; all roles stopped."; return;
    }
    ++count;
    c.notes[note]={++c.order,velocity,false};
    play(i,channel,note);
  }
  void stop(int i) noexcept {
    owners[i].note=-1;
    if(nes(state.model)) setNesEnable(i,false);
    else { static constexpr int dac[]{0x12,0x17,0x1a,0x21}; gw(dac[i],0); }
  }
  void resume(int i,int channel) noexcept {
    const auto& notes=channels[channel].notes;
    int latest=-1;
    for(int note=0;note<128;++note)
      if(notes[note].order&&(latest<0||notes[note].order>notes[latest].order))latest=note;
    if(latest==owners[i].note)return;
    if(latest<0)stop(i);else play(i,channel,latest);
  }
  void release(int i,int channel,int note) noexcept {
    auto& c=channels[channel];
    auto& n=c.notes[note];
    if(held[channel][note]||!n.order)return;
    if(c.pedal)n.released=true;
    else { n={}; if(owners[i].note==note)resume(i,channel); }
  }
  void controller(int i,int channel,int number,int value) noexcept {
    auto& c=channels[channel];
    if(number==64||number==121) {
      const int previous=i>=0?owners[i].note:-1;
      c.pedal=number==64&&value>=64;
      if(number==121)c.bend=0;
      if(!c.pedal) {
        for(auto& n:c.notes)if(n.released)n={};
        if(previous>=0&&!c.notes[previous].order)resume(i,channel);
      }
      if(number==121&&i>=0&&i<3&&owners[i].note==previous)pitch(i,false);
    } else if(number==120||number==123) {
      // Cancel musical ownership, not the FIFO lifetimes still owed by the
      // input device. A subsequent retrigger must survive those old note-offs.
      if(number==123&&c.pedal) {
        for(auto& n:c.notes)if(n.order)n.released=true;
      } else {
        c.notes={};
        if(i>=0)stop(i);
      }
    }
  }
  void reset(bool clearHeld=true) noexcept {
    owners={}; enables=0; frameCountdown=8192;
    if(clearHeld) { held={}; channels={}; }
    else for(auto& c:channels) { c.notes={}; c.order=0; }
    if(na) { na->reset(state.model==Model::NesPal,0); nb->clear(); nw(0x4017,0x40); }
    if(gb) {
      apu_reset(gb,state.model==Model::Cgb?GbApuType_CGB:GbApuType_DMG);
      gw(0x26,0x80); routing(); wave();
    }
  }
};
Engine::Engine():impl{std::make_unique<Impl>()} {}
Engine::~Engine()=default;
bool Engine::prepare(double sampleRate,const Settings& s,const DmcData& d) {
  if(auto e=validate(s)) { impl->failure=e; return false; }
  if(auto e=validate(d)) { impl->failure=e; return false; }
  if(!std::isfinite(sampleRate)||sampleRate<8000.||sampleRate>384000.) {
    impl->failure="APU host sample rate must be 8000 through 384000 Hz."; return false;
  }
  if(nes(s.model)&&std::floor(sampleRate)!=sampleRate) {
    impl->failure="The NES Blip_Buffer core requires an integer host sample rate."; return false;
  }
  auto next=std::make_unique<Impl>(); next->state=s; next->data=d;
  if(nes(s.model)) {
    next->nb=std::make_unique<Blip_Buffer>();
    if(auto e=next->nb->set_sample_rate(long(std::lround(sampleRate)),100)) { impl->failure=e; return false; }
    next->nb->clock_rate(effectiveClock(s));
    next->na=std::make_unique<Nes_Apu>(); next->na->output(next->nb.get());
    next->na->dmc_reader(&Impl::readDmc,next.get());
  } else {
    next->gb=apu_init(effectiveClock(s),sampleRate);
    if(!next->gb) { impl->failure="Unable to allocate Game Boy APU buffers."; return false; }
    apu_set_highpass_filter(next->gb,s.model==Model::Cgb?GbApuFilter_CGB:GbApuFilter_DMG,effectiveClock(s),sampleRate);
  }
  next->prepared=true; next->reset(); impl.swap(next); return true;
}
bool Engine::configure(const Settings& s) noexcept {
  if(auto e=validate(s)) { impl->failure=e; return false; }
  auto& p=*impl;
  if(!p.prepared || s.model!=p.state.model || effectiveClock(s)!=effectiveClock(p.state)) {
    p.failure="APU model or clock changes require off-thread preparation."; return false;
  }
  const auto oldRoles=p.state.roles;
  if(s.routing!=p.state.routing
      ||(s.routing==RoutingMode::Instrument&&s.sound!=p.state.sound)) {
    // Stop the old musical ownership without losing source-channel controllers
    // or FIFO note-off debt. Old held notes must not become fallback candidates.
    p.reset(false);
  }
  if(nes(s.model)) {
    const bool dmcChanged=s.roles[4]!=oldRoles[4]||s.dmcLoop!=p.state.dmcLoop
      ||s.dmcAddress!=p.state.dmcAddress||s.dmcLength!=p.state.dmcLength;
    const bool levelChanged=s.dmcLevel!=p.state.dmcLevel;
    p.state=s;
    for(int i=0;i<4;++i) if(s.roles[i]!=oldRoles[i]) p.nesParameters(i,&oldRoles[i]);
    if(dmcChanged)p.nesParameters(4,&oldRoles[4]);
    if(levelChanged)p.nw(0x4011,s.dmcLevel);
  } else {
    const bool waveChanged=s.wave!=p.state.wave;
    p.state=s;
    for(int i=0;i<4;++i) if(s.roles[i]!=oldRoles[i]) p.gbParameters(i,&oldRoles[i]);
    bool route{};
    for(int i=0;i<4;++i) route|=s.roles[i].left!=oldRoles[i].left||s.roles[i].right!=oldRoles[i].right;
    if(route) p.routing();
    if(waveChanged) { p.wave(); if(p.owners[2].note>=0) p.pitch(2,true); }
  }
  p.failure=nullptr; return true;
}
void Engine::midi(std::span<const uint8_t> bytes) noexcept {
  auto& p=*impl; if(bytes.empty()||!p.prepared) return;
  const int status=bytes[0],kind=status&0xf0,ch=status&15;
  if(status==0xff) { panic(); return; }
  if(status<0x80||status>=0xf0) { p.midiFailure="APU accepts channel MIDI, not SysEx or system messages."; return; }
  if(ch>=int(p.held.size())) { p.midiFailure="APU MIDI channel allocation uses channels 1-5 (NES) or 1-4 (Game Boy); Mono uses channel 1."; return; }
  const int length=(kind==0xc0||kind==0xd0)?2:3;
  if(int(bytes.size())<length||bytes[1]>127||(length==3&&bytes[2]>127)) { p.midiFailure="Malformed APU MIDI message."; return; }
  const int a=bytes[1],b=length==3?bytes[2]:0;
  const bool noteOff=kind==0x80||(kind==0x90&&!b);
  const bool owed=noteOff&&p.held[ch][a];
  if(owed)--p.held[ch][a];
  const bool instrument=p.state.routing==RoutingMode::Instrument;
  const int role=instrument?int(p.state.sound):ch;
  if((instrument&&ch!=0)||role>=(nes(p.state.model)?5:4)) {
    // Cleanup from a former native route must drain its debt even when that
    // input channel is no longer a musical route.
    if(kind==0xb0)p.controller(-1,ch,a,b);
    else if(kind==0xe0)p.channels[ch].bend=(a|(b<<7))-8192;
    if(owed||kind==0xb0||kind==0xe0)p.midiFailure=nullptr;
    else p.midiFailure="APU Mono accepts channel 1; MIDI channel allocation addresses the model's physical roles.";
    return;
  }
  p.midiFailure=nullptr;
  if(kind==0x90&&b) p.start(role,ch,a,b);
  else if(noteOff&&owed) p.release(role,ch,a);
  else if(kind==0xe0) { p.channels[ch].bend=(a|(b<<7))-8192; if(role<3) p.pitch(role,false); }
  else if(kind==0xb0) p.controller(role,ch,a,b);
}
void Engine::render(double* left,double* right,int frames) noexcept {
  auto& p=*impl;
  if(frames<=0) return;
  std::fill_n(left,frames,0.); std::fill_n(right,frames,0.);
  if(!p.prepared) return;
  for(int offset=0;offset<frames;) {
    const int count=std::min(512,frames-offset); int got{};
    if(p.na) {
      const int cycles=p.nb->count_clocks(count);
      p.na->end_frame(cycles); p.nb->end_frame(cycles);
      got=int(p.nb->read_samples(p.pcm.data(),count));
      if(got!=count) { p.failure="NES resampler did not produce the requested host frames."; p.reset(); return; }
      for(int i=0;i<count;++i) left[offset+i]=right[offset+i]=p.pcm[i]*(p.state.volume/32768.);
    } else {
      const int cycles=apu_clocks_needed(p.gb,count*2);
      for(int t=p.frameCountdown;t<=cycles;t+=8192) apu_frame_sequencer_clock(p.gb,t);
      p.frameCountdown-=cycles;
      while(p.frameCountdown<=0) p.frameCountdown+=8192;
      apu_end_frame(p.gb,cycles); apu_update_timestamp(p.gb,-cycles);
      got=apu_read_samples(p.gb,p.pcm.data(),count*2);
      if(got!=count*2) { p.failure="Game Boy resampler did not produce the requested host frames."; p.reset(); return; }
      for(int i=0;i<count;++i) { left[offset+i]=p.pcm[2*i]*(p.state.volume/32768.); right[offset+i]=p.pcm[2*i+1]*(p.state.volume/32768.); }
    }
    offset+=count;
  }
}
void Engine::panic() noexcept { if(impl->prepared) impl->reset(); impl->failure=nullptr; impl->midiFailure=nullptr; }
const char* Engine::error() const noexcept { return impl->failure?impl->failure:impl->midiFailure; }
const char* Engine::midiError() const noexcept { return impl->midiFailure; }
const Settings& Engine::settings() const noexcept { return impl->state; }
int Engine::activeVoices() const noexcept {
  if(!impl->prepared) return 0;
  unsigned status=impl->na?impl->na->active_channels()&15:apu_read_io(impl->gb,0x26,0)&15;
  if(impl->na && impl->owners[4].note>=0 && impl->na->dmc_playing()) status|=16;
  return std::popcount(status);
}
}
