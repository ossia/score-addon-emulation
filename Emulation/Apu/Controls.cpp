// SPDX-License-Identifier: GPL-3.0-or-later
#include "Controls.hpp"
#include <Emulation/Volume.hpp>
#include <Emulation/WaveformInlet.hpp>
namespace Emulation::Apu {
namespace {
Process::ComboBox* routingControl(QObject* parent) {
  return new Process::ComboBox{{{"Mono",0},{"MIDI channel",1}},
    0,"Voice allocation",Id<Process::Port>{Routing+1},parent};
}
Process::ComboBox* soundControl(QObject* parent) {
  return new Process::ComboBox{{{"Pulse 1",0},{"Pulse 2",1},{"Triangle",2},{"Noise",3},{"DMC",4}},
    0,"Sound",Id<Process::Port>{Sound+1},parent};
}
}
QString controlLabel(Model model,int c) {
  if(c==Routing)return QStringLiteral("Voice allocation");
  if(c==Sound)return QStringLiteral("Sound");
  if(c>=RoleBase&&c<RoleEnd) {
    const int field=(c-RoleBase)%RoleFieldCount;
    static constexpr const char* labels[]{"Duty index","Length","Level","Envelope period",
      "Length enable","Envelope up","Sweep period","Sweep shift","Sweep subtract","Rate",
      "Short noise","Left","Right","Linear reload","Envelope enable","Sweep enable"};
    if(field==Length)return gameBoy(model)?QStringLiteral("Length preload"):QStringLiteral("Length index");
    if(field==Rate)return gameBoy(model)?QStringLiteral("Noise period code"):QStringLiteral("Rate index");
    return QString::fromUtf8(labels[field]);
  }
  static constexpr const char* labels[]{"Model","Clock Hz (0=Auto)","Volume","Import DMC",
    "Wave","Address *64","Length *16 +1","DAC level","Loop"};
  return QString::fromUtf8(labels[c]);
}
void updateControlMetadata(Process::ControlInlet& inlet,Model model,int c) {
  const auto rename = [&](const QString& previous, const QString& current) {
    // Only migrate known stock names; preserve authored port names.
    if(inlet.name()==previous)inlet.setName(current);
  };
  if(c==Volume)inlet.setDomain(ossia::make_domain(0.f,MaxVolume));
  if(c==Routing) {
    rename(QStringLiteral("Routing"),QStringLiteral("Voice allocation"));
    if(auto* combo=qobject_cast<Process::ComboBox*>(&inlet))
      combo->setAlternatives({{"Mono",0},{"MIDI channel",1}});
    inlet.setDescription(QStringLiteral("Mono plays the selected Sound from MIDI channel 1. MIDI channel maps channels 1 to 5 (NES: pulse 1, pulse 2, triangle, noise, DMC) or 1 to 4 (Game Boy: pulse 1, pulse 2, wave, noise) to separate monophonic hardware roles. Each uses last-note priority and held-note fallback with the original velocity. Poly is unavailable: these hardware roles are not interchangeable voices. Sustain and pitch bend belong to the input MIDI channel."));
  }
  if(c==Sound) {
    if(auto* combo=qobject_cast<Process::ComboBox*>(&inlet)) {
      std::vector<std::pair<QString,ossia::value>> choices{
        {"Pulse 1",0},{"Pulse 2",1},{gameBoy(model)?"Wave":"Triangle",2},{"Noise",3}};
      if(!gameBoy(model))choices.emplace_back("DMC",4);
      combo->setAlternatives(std::move(choices));
    }
    inlet.setDescription(QStringLiteral("Mono sound on MIDI channel 1; remembered separately for NES and Game Boy. Editing tabs never select the played sound. Voice allocation changes and Mono sound changes stop voices and discard fallback notes, retaining MIDI channel controllers and outstanding note-off lifetimes; play again to restart. Sound does not affect MIDI channel allocation."));
  }
  if(c>=RoleBase&&c<RoleEnd) {
    const int role=(c-RoleBase)/RoleFieldCount,field=(c-RoleBase)%RoleFieldCount;
    const QString roleName=role==0?QStringLiteral("Pulse 1"):role==1?QStringLiteral("Pulse 2"):
      role==2?(gameBoy(model)?QStringLiteral("Wave"):QStringLiteral("Triangle")):
      role==3?QStringLiteral("Noise"):QStringLiteral("DMC");
    const auto name=roleName+QStringLiteral(": ")+controlLabel(model,c);
    for(const auto previousModel : {Model::NesNtsc,Model::Dmg}) {
      const auto previousRole=role==2
          ?(gameBoy(previousModel)?QStringLiteral("Wave"):QStringLiteral("Triangle")):roleName;
      const auto label=controlLabel(previousModel,c);
      rename(previousRole+QStringLiteral(" \u00b7 ")+label,name);
      rename(previousRole+QStringLiteral(": ")+label,name);
    }
    if(field==Length)inlet.setDescription(gameBoy(model)
      ?QStringLiteral("Native length-counter preload; ticks remaining = %1 - preload.").arg(role==2?256:64)
      :QStringLiteral("Native 5-bit NES length-table index, not a duration."));
    if(field==Rate)inlet.setDescription(gameBoy(model)
      ?QStringLiteral("NR43 period code: clock shift * 8 + divisor code; short-noise bit is separate.")
      :QStringLiteral("Native 4-bit rate-table index; timing follows the selected NES clock."));
    if(field==EnvelopePeriod)inlet.setDescription(gameBoy(model)
      ?QStringLiteral("Native 3-bit envelope period in 64 Hz sequencer ticks; 0 disables envelope stepping.")
      :QStringLiteral("Native envelope divider: period + 1 quarter-frame ticks."));
    if(qobject_cast<Process::IntSlider*>(&inlet))
      inlet.setDomain(ossia::make_domain(0,scalarMaximum(model,c)));
  }
  if(c==Clock) {
    rename(QStringLiteral("Clock"),QStringLiteral("Clock override (Hz; 0=Auto)"));
    rename(QStringLiteral("Clock Hz (0=Auto)"),QStringLiteral("Clock override (Hz; 0=Auto)"));
    inlet.setDescription(QStringLiteral("0 = automatic model clock (%1 Hz). Explicit overrides: 100000 to 20000000 Hz.").arg(defaultClock(model)));
  }
  if(c==DmcFile) {
    rename(QStringLiteral("Import DMC"),QStringLiteral("DMC: Import raw data"));
    rename(QStringLiteral("DMC \u00b7 Import raw data"),QStringLiteral("DMC: Import raw data"));
  }
  if(c>=DmcAddress&&c<=DmcLoop) {
    auto previous=controlLabel(model,c);
    previous.replace(QChar('*'),QChar(0x00d7));
    rename(QStringLiteral("DMC \u00b7 ")+previous,QStringLiteral("DMC: ")+controlLabel(model,c));
  }
  if(c==DmcAddress)inlet.setDescription(QStringLiteral("Start address = $C000 + register * 64 bytes."));
  if(c==DmcLength)inlet.setDescription(QStringLiteral("Sample length = register * 16 + 1 bytes."));
  if(c==Wave) {
    rename(QStringLiteral("Wave"),QStringLiteral("Wave: Shape"));
    rename(QStringLiteral("Wave \u00b7 Shape"),QStringLiteral("Wave: Shape"));
  }
}
std::vector<Process::ControlInlet*> makeControls(QObject* parent) {
  std::vector<Process::ControlInlet*> v(ControlCount); const Settings s;
  const auto id=[](int c){return Id<Process::Port>{c+1};};
  const auto integer=[&](int c,int lo,int hi,const char* name){v[c]=new Process::IntSlider{lo,hi,int(controlValue(s,c)),QString::fromUtf8(name),id(c),parent};};
  const auto toggle=[&](int c,const char* name){v[c]=new Process::Toggle{bool(controlValue(s,c)),QString::fromUtf8(name),id(c),parent};};
  v[ChipModel]=new Process::ComboBox{{{"NES NTSC",0},{"NES PAL",1},{"Game Boy DMG",2},{"Game Boy Color",3}},0,"Model",id(ChipModel),parent};
  integer(Clock,0,20000000,"Clock");
  v[Volume]=new Process::FloatSlider{0.f,MaxVolume,float(s.volume),"Volume",id(Volume),parent};
  v[DmcFile]=new Process::FileChooser{QString{},QStringLiteral("Raw DMC (*.dmc *.bin);;All files (*)"),QStringLiteral("Import DMC"),id(DmcFile),parent};
  v[Wave]=makeWaveformInlet(Waveform::triangle(),16,QStringLiteral("Wave: Shape"),id(Wave),parent);
  integer(DmcAddress,0,255,"DMC: Address *64"); integer(DmcLength,0,255,"DMC: Length *16 +1");
  integer(DmcLevel,0,127,"DMC: DAC level");toggle(DmcLoop,"DMC: Loop");
  v[Routing]=routingControl(parent);v[Sound]=soundControl(parent);
  v[DmcFile]->setName(QStringLiteral("DMC: Import raw data"));
  v[DmcAddress]->setDescription(QStringLiteral("Start address = $C000 + register * 64 bytes."));
  v[DmcLength]->setDescription(QStringLiteral("Sample length = register * 16 + 1 bytes."));
  for(int r=0;r<5;++r) {
    const auto c=[r](RoleField f){return roleControl(r,f);};
    integer(c(Duty),0,3,"Duty");integer(c(Length),0,scalarMaximum(s.model,c(Length)),"Length");
    if(r==2)v[c(Level)]=new Process::ComboBox{{{"Mute",0},{"Full",1},{"Half",2},{"Quarter",3}},1,"Wave level",id(c(Level)),parent};
    else integer(c(Level),0,15,"Level");
    integer(c(EnvelopePeriod),0,15,"Envelope period");toggle(c(LengthEnable),"Length enable");toggle(c(EnvelopeUp),"Envelope up");
    integer(c(SweepPeriod),0,7,"Sweep period");integer(c(SweepShift),0,7,"Sweep shift");toggle(c(SweepNegate),"Sweep subtract");
    integer(c(Rate),0,scalarMaximum(s.model,c(Rate)),"Rate");toggle(c(NoiseShort),"Short noise");toggle(c(Left),"Left");toggle(c(Right),"Right");integer(c(Linear),0,127,"Linear reload");
    toggle(c(EnvelopeEnable),"Envelope enable");toggle(c(SweepEnable),"Sweep enable");
    const QString roleName=r==0?QStringLiteral("Pulse 1"):r==1?QStringLiteral("Pulse 2"):
        r==2?QStringLiteral("Triangle"):r==3?QStringLiteral("Noise"):QStringLiteral("DMC");
    for(int f=0;f<RoleFieldCount;++f)
      v[roleControl(r,RoleField(f))]->setName(
          roleName+QStringLiteral(": ")+controlLabel(s.model,roleControl(r,RoleField(f))));
  }
  for(int c=0;c<ControlCount;++c)updateControlMetadata(*v[c],s.model,c);
  return v;
}
}
