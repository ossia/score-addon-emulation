// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Engine.hpp"
#include <Emulation/Volume.hpp>
#include <Emulation/Waveform.hpp>
#include <ossia/network/value/value.hpp>
#include <ossia/network/value/value_conversion.hpp>
#include <algorithm>
#include <cmath>
namespace Emulation::Apu {
// Stable port IDs: MIDI=0, controls=enumerator+1. Never reorder.
enum Control : int { ChipModel, Clock, Volume, DmcFile, Wave, DmcAddress, DmcLength,
  DmcLevel, DmcLoop, RoleBase, RoleEnd = RoleBase + 5 * 16,
  Routing = RoleEnd, Sound, ControlCount };
enum RoleField : int { Duty, Length, Level, EnvelopePeriod, LengthEnable, EnvelopeUp,
  SweepPeriod, SweepShift, SweepNegate, Rate, NoiseShort, Left, Right, Linear, EnvelopeEnable, SweepEnable, RoleFieldCount };
constexpr int roleControl(int role, RoleField field) { return RoleBase + role * RoleFieldCount + field; }
inline bool gameBoy(Model m) noexcept { return m == Model::Dmg || m == Model::Cgb; }
inline void selectModel(Settings& s, Model m) noexcept {
  if(gameBoy(s.model)!=gameBoy(m)) {
    s.roles.swap(s.inactiveRoles);
    std::swap(s.sound,s.inactiveSound);
  }
  s.model=m;
}
inline int scalarMaximum(Model model,int c) noexcept {
  if(c>=RoleBase && c<RoleEnd) {
    const int role=(c-RoleBase)/RoleFieldCount,field=(c-RoleBase)%RoleFieldCount;
    const bool gb=gameBoy(model);
    switch(field) {
      case Duty:return 3;
      case Length:return role==4?255:!gb?31:role==2?255:63;
      case Level:return gb&&role==2?3:15;
      case EnvelopePeriod:return gb&&role<4&&role!=2?7:15;
      case SweepPeriod:case SweepShift:return 7;
      case Rate:return !gb&&role>=3?15:127;
      case Linear:return 127;
      default:return 1;
    }
  }
  switch(c) {
    case ChipModel:return 3;case Clock:return 20000000;case Volume:return int(MaxVolume);
    case Routing:return 1;case Sound:return gameBoy(model)?3:4;
    case DmcAddress:case DmcLength:return 255;case DmcLevel:return 127;
    case DmcLoop:return 1;default:return 0;
  }
}
inline double controlValue(const Settings& s, int c) noexcept {
  if(c>=RoleBase && c<RoleEnd) {
    const auto& r=s.roles[(c-RoleBase)/RoleFieldCount];
    switch((c-RoleBase)%RoleFieldCount) {
      case Duty:return r.duty; case Length:return r.length; case Level:return r.level;
      case EnvelopePeriod:return r.envelopePeriod; case LengthEnable:return r.lengthEnable;
      case EnvelopeUp:return r.envelopeUp; case SweepPeriod:return r.sweepPeriod;
      case SweepShift:return r.sweepShift; case SweepNegate:return r.sweepNegate;
      case Rate:return r.rate; case NoiseShort:return r.noiseShort; case Left:return r.left;
      case Right:return r.right; case Linear:return r.linear;
      case EnvelopeEnable:return r.envelopeEnable; case SweepEnable:return r.sweepEnable;
    }
  }
  switch(c) { case ChipModel:return int(s.model); case Clock:return s.clock; case Volume:return s.volume;
    case Routing:return int(s.routing);case Sound:return int(s.sound);
    case DmcAddress:return s.dmcAddress; case DmcLength:return s.dmcLength;
    case DmcLevel:return s.dmcLevel; case DmcLoop:return s.dmcLoop; default:return 0.; }
}
inline bool setScalar(Settings& s,int c,double number) noexcept {
  if(!std::isfinite(number) || number < -2147483647. || number > 2147483647.
      || (c!=Volume && std::trunc(number)!=number)) return false;
  if(number<0. || number>scalarMaximum(s.model,c)
      || (c==Clock && number!=0. && number<100000.))return false;
  int v=int(number);
  if(c>=RoleBase && c<RoleEnd) {
    auto& r=s.roles[(c-RoleBase)/RoleFieldCount];
    switch((c-RoleBase)%RoleFieldCount) {
      case Duty:r.duty=v;break; case Length:r.length=v;break; case Level:r.level=v;break;
      case EnvelopePeriod:r.envelopePeriod=v;break; case LengthEnable:if(v<0||v>1)return false;r.lengthEnable=v;break;
      case EnvelopeUp:if(v<0||v>1)return false;r.envelopeUp=v;break;
      case SweepPeriod:r.sweepPeriod=v;break; case SweepShift:r.sweepShift=v;break;
      case SweepNegate:if(v<0||v>1)return false;r.sweepNegate=v;break;
      case Rate:r.rate=v;break; case NoiseShort:if(v<0||v>1)return false;r.noiseShort=v;break;
      case Left:if(v<0||v>1)return false;r.left=v;break; case Right:if(v<0||v>1)return false;r.right=v;break;
      case Linear:r.linear=v;break;
      case EnvelopeEnable:if(v<0||v>1)return false;r.envelopeEnable=v;break;
      case SweepEnable:if(v<0||v>1)return false;r.sweepEnable=v;break;
    } return true;
  }
  switch(c) { case ChipModel:if(v<0||v>3)return false;selectModel(s,Model(v));break;
    case Clock:s.clock=v;break; case Volume:s.volume=number;break;
    case Routing:s.routing=RoutingMode(v);break;case Sound:s.sound=SoundRole(v);break;
    case DmcAddress:s.dmcAddress=v;break; case DmcLength:s.dmcLength=v;break;
    case DmcLevel:s.dmcLevel=v;break; case DmcLoop:if(v<0||v>1)return false;s.dmcLoop=v;break;
    default:return false; } return true;
}
inline bool setControl(Settings& s,int c,const ossia::value& value) noexcept {
  // Four-bit wave RAM, resampled from the value in place.
  if(c==Wave)return Waveform::sample(value,s.wave,0,15);
  if(value.get_type()!=ossia::val_type::INT && value.get_type()!=ossia::val_type::FLOAT
      && value.get_type()!=ossia::val_type::BOOL) return false;
  return setScalar(s,c,ossia::convert<double>(value));
}
inline bool available(Model m,int c) noexcept {
  const bool gb=gameBoy(m);
  if(c==Wave)return gb;
  if(c>=DmcFile && c<=DmcLoop)return !gb;
  if(c<RoleBase||c>=RoleEnd)return true;
  const int role=(c-RoleBase)/RoleFieldCount,field=(c-RoleBase)%RoleFieldCount;
  if(role==4)return !gb && field==Rate;
  switch(field) {
    case Duty:return role<2; case Level:return gb || role!=2;
    case EnvelopePeriod:return role!=2; case EnvelopeUp:return gb && role!=2;
    case EnvelopeEnable:return !gb && role!=2;
    case SweepEnable:return !gb && role<2;
    case SweepPeriod:case SweepShift:case SweepNegate:return role==0 || (!gb&&role==1);
    case Rate:case NoiseShort:return role==3;
    case Left:case Right:return gb;
    case Linear:return !gb&&role==2;
    default:return true;
  }
}
} // namespace Emulation::Apu
