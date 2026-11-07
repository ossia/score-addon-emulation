// SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine.hpp"
#include <Emulation/Volume.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
namespace Emulation::La
{
// Byte order and ranges follow munt Structures.h / Roland's native timbre memory.
const std::array<Field, 58> partialFields{{
  {"Coarse pitch",96}, {"Fine pitch",100}, {"Pitch key follow",16}, {"Pitch bend",1},
  {"Wave / PCM bank",3}, {"PCM wave",127}, {"Pulse width",100}, {"Pulse velocity",14},
  {"Pitch envelope depth",10}, {"Pitch envelope velocity",100}, {"Pitch time key follow",4},
  {"Pitch time 1",100}, {"Pitch time 2",100}, {"Pitch time 3",100}, {"Pitch time 4",100},
  {"Pitch level 0",100}, {"Pitch level 1",100}, {"Pitch level 2",100}, {"Pitch sustain",100}, {"Pitch end",100},
  {"LFO rate",100}, {"LFO depth",100}, {"LFO modulation",100},
  {"Cutoff",100}, {"Resonance",30}, {"Filter key follow",14}, {"Filter bias point",127}, {"Filter bias level",14},
  {"Filter envelope depth",100}, {"Filter envelope velocity",100}, {"Filter depth key follow",4}, {"Filter time key follow",4},
  {"Filter time 1",100}, {"Filter time 2",100}, {"Filter time 3",100}, {"Filter time 4",100}, {"Filter time 5",100},
  {"Filter level 1",100}, {"Filter level 2",100}, {"Filter level 3",100}, {"Filter sustain",100},
  {"Amplitude",100}, {"Amplitude velocity",100}, {"Amp bias point 1",127}, {"Amp bias level 1",12},
  {"Amp bias point 2",127}, {"Amp bias level 2",12}, {"Amp time key follow",4}, {"Amp time velocity",4},
  {"Amp time 1",100}, {"Amp time 2",100}, {"Amp time 3",100}, {"Amp time 4",100}, {"Amp time 5",100},
  {"Amp level 1",100}, {"Amp level 2",100}, {"Amp level 3",100}, {"Amp sustain",100}
}};
Field controlField(int c) noexcept
{
  static constexpr Field globals[] = {{"Control ROM",0},{"PCM ROM",0},{"Volume",int(MaxVolume)},
    {"Reverb mode",3},{"Reverb time",7},{"Reverb level",7},{"Master tune",127},{"Master level",100},
    {"Reverb enabled",1},{"Reverse stereo",1},{"DAC",3},{"MIDI delay",2},
    {"Smooth amplitude",1},{"Smooth pan",1},{"In-phase mixing",1},
    {"Rhythm channel",16},{"Rhythm level",100},{"Rhythm pan",14},{"Rhythm reverb",1},{"Rhythm reserve",32}};
  static constexpr Field parts[] = {{"Bank",4},{"Program",127},{"Channel",16},{"Level",100},{"Pan",14},
    {"Transpose",48},{"Fine tune",100},{"Bend range",24},{"Native repeat / priority",3},{"Reverb",1},{"Reserve",32}};
  static constexpr Field common[] = {{"Structure 1 + 2",12},{"Structure 3 + 4",12},{"Enabled partials",15},{"No sustain",1}};
  if(c >= 0 && c < PartBase) return globals[c];
  if(c < EditTimbre && c >= PartBase) return parts[(c - PartBase) % PartFieldCount];
  if(c == EditTimbre) return {"Memory timbre",63};
  if(c >= TimbreBase && c < TimbreBase + 4) return common[c - TimbreBase];
  if(c >= TimbreBase + 4 && c < VoiceAllocation) return partialFields[(c - TimbreBase - 4) % 58];
  if(c == VoiceAllocation) return {"Voice allocation",2};
  return {"Invalid",0};
}
Patch defaultPatch()
{
  Patch p;
  p.values[ReverbTime] = 5; p.values[ReverbLevel] = 3;
  p.values[MasterTune] = 64; p.values[MasterLevel] = 100;
  p.values[ReverbEnabled] = 1; p.values[NiceRamp] = 1;
  p.values[RhythmChannel] = 16; p.values[RhythmLevel] = 100; p.values[RhythmPan] = 7;
  p.values[RhythmReverb] = 1; p.values[RhythmReserve] = 4;
  for(int part = 0; part < 8; ++part)
  {
    p.values[partControl(part, Channel)] = part == 0 ? 0 : 16;
    p.bankPrograms[part][1] = part;
    p.values[partControl(part, Level)] = 100; p.values[partControl(part, Pan)] = 7;
    p.values[partControl(part, KeyShift)] = 24; p.values[partControl(part, FineTune)] = 50;
    p.values[partControl(part, BendRange)] = 12; p.values[partControl(part, Reverb)] = 1;
    p.values[partControl(part, Reserve)] = part < 4 ? 4 : 3;
  }
  Timbre t{};
  constexpr char name[] = "Score Init";
  std::copy_n(name, 10, t.begin()); t[12] = 1;
  for(int partial = 0; partial < 4; ++partial)
  {
    auto* b = t.data() + 14 + partial * 58;
    b[0] = 36; b[1] = 50; b[2] = 11; b[3] = 1; b[4] = 1;
    b[6] = 50; b[7] = 7;
    std::fill(b + 15, b + 20, 50);
    b[23] = 100; b[25] = 11; b[27] = 7;
    b[41] = 80; b[42] = 50; b[53] = 35;
    b[54] = b[55] = b[56] = b[57] = 100;
  }
  p.timbres.fill(t);
  return p;
}
bool Patch::operator==(const Patch& p) const noexcept
{ return volume == p.volume && allocation == p.allocation && values == p.values && bankPrograms == p.bankPrograms
    && *soundROMHashes == *p.soundROMHashes && timbres == p.timbres && *roms == *p.roms; }
bool sameSetup(const Patch& a, const Patch& b) noexcept
{
  if(a.roms == b.roms) return true;
  const auto& x = *a.roms; const auto& y = *b.roms;
  const auto hash = [](const std::string& l, const std::string& r) { return l.empty() || r.empty() || l == r; };
  return x.controlPath == y.controlPath && x.pcmPath == y.pcmPath
      && hash(x.controlHash, y.controlHash) && hash(x.pcmHash, y.pcmHash);
}
double controlValue(const Patch& p, int c) noexcept
{
  if(c == Volume) return p.volume;
  if(c == VoiceAllocation) return int(p.allocation);
  if(c >= TimbreBase && c < VoiceAllocation) return p.timbres[p.values[EditTimbre]][10 + c - TimbreBase];
  return c >= 0 && c < TimbreBase ? p.values[c] : 0.;
}
bool setControl(Patch& p, int c, double v) noexcept
{
  if(c < Volume || c >= ControlCount || !std::isfinite(v) || v < 0 || v > controlField(c).maximum) return false;
  if(c == Volume) { p.volume = v; return true; }
  if(std::trunc(v) != v) return false;
  if(c == VoiceAllocation) { p.allocation = Allocation(int(v)); return true; }
  if(c >= PartBase && c < EditTimbre)
  {
    const int part = (c - PartBase) / PartFieldCount;
    const int field = (c - PartBase) % PartFieldCount;
    const int bank = p.values[partControl(part, Bank)];
    auto& program = p.values[partControl(part, Program)];
    if(field == Bank)
    {
      if(bank == int(v)) return true;
      p.bankPrograms[part][bank] = program;
      p.values[c] = int(v);
      program = p.bankPrograms[part][int(v)];
      return true;
    }
    if(field == Program)
    {
      if(bank && v > 63) return false;
      program = p.bankPrograms[part][bank] = int(v);
      return true;
    }
  }
  if(c >= TimbreBase) p.timbres[p.values[EditTimbre]][10 + c - TimbreBase] = uint8_t(v);
  else p.values[c] = int(v);
  return true;
}
const char* validate(const Patch& p) noexcept
{
  if(!p.roms) return "Missing LA ROM reference object.";
  const auto validHash = [](const std::string& s) {
    return s.empty() || (s.size() == 40 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
  };
  if(!validHash(p.roms->controlHash) || !validHash(p.roms->pcmHash)) return "Invalid LA ROM SHA-1 reference.";
  if(!p.soundROMHashes) return "Missing LA sound ROM identity.";
  for(const auto& hash : *p.soundROMHashes)
    if(!validHash(hash)) return "Invalid LA sound ROM SHA-1 identity.";
  for(const auto& banks : p.bankPrograms)
    for(int bank = 0; bank < 5; ++bank)
      if(banks[bank] < 0 || banks[bank] > (bank == 0 ? 127 : 63))
        return "Invalid LA remembered bank program.";
  if(p.roms->controlPath.size() > 32768 || p.roms->pcmPath.size() > 32768) return "LA ROM path is too long.";
  if(!std::isfinite(p.volume) || p.volume < 0 || p.volume > MaxVolume) return "LA Volume must be finite and within 0..32.";
  if(int(p.allocation) < 0 || int(p.allocation) > 2) return "Invalid LA voice allocation.";
  for(int c = ReverbMode; c < TimbreBase; ++c)
    if(p.values[c] < 0 || p.values[c] > controlField(c).maximum) return "LA setting outside its native range.";
  for(int part = 0; part < 8; ++part)
    if(p.values[partControl(part, Bank)] && p.values[partControl(part, Program)] > 63)
      return "Direct LA timbre banks have programs 0..63; factory patch programs have 0..127.";
  int reserve = p.values[RhythmReserve];
  for(int part = 0; part < 8; ++part) reserve += p.values[partControl(part, Reserve)];
  if(reserve > 32) return "LA partial reserves must total at most 32.";
  for(const auto& t : p.timbres)
  {
    for(int i = 0; i < 10; ++i) if(t[i] < 32 || t[i] > 126) return "LA timbre names must use printable ASCII.";
    for(int c = TimbreBase; c < VoiceAllocation; ++c)
      if(t[10 + c - TimbreBase] > controlField(c).maximum) return "LA timbre field outside its native range.";
  }
  return nullptr;
}
std::vector<uint8_t> encodePatch(const Patch& p)
{
  if(const auto* e = validate(p)) throw std::runtime_error(e);
  std::vector<uint8_t> out;
  out.reserve(17000);
  const auto u32 = [&](uint32_t v) { for(int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i))); };
  const auto text = [&](const std::string& s) { u32(uint32_t(s.size())); out.insert(out.end(), s.begin(), s.end()); };
  u32(0x33414c53); // SLA3; SLA1/SLA2 retain native Poly allocation on read.
  text(p.roms->controlPath); text(p.roms->pcmPath); text(p.roms->controlHash); text(p.roms->pcmHash);
  const auto gain = std::bit_cast<uint64_t>(p.volume); u32(uint32_t(gain)); u32(uint32_t(gain >> 32));
  for(auto v : p.values) u32(uint32_t(v));
  for(const auto& banks : p.bankPrograms) for(auto program : banks) u32(uint32_t(program));
  for(const auto& hash : *p.soundROMHashes) text(hash);
  for(const auto& t : p.timbres) out.insert(out.end(), t.begin(), t.end());
  u32(uint32_t(p.allocation));
  return out;
}
Patch decodePatch(std::span<const uint8_t> in)
{
  std::size_t at{};
  const auto u32 = [&]() {
    if(in.size() - at < 4) throw std::runtime_error("Truncated LA state.");
    uint32_t v{}; for(int i = 0; i < 4; ++i) v |= uint32_t(in[at++]) << (8 * i); return v;
  };
  const auto text = [&]() {
    const auto size = u32();
    if(size > 32768 || size > in.size() - at) throw std::runtime_error("Invalid LA ROM reference length.");
    std::string s{reinterpret_cast<const char*>(in.data() + at), size}; at += size; return s;
  };
  const auto schema = u32();
  if(schema != 0x31414c53 && schema != 0x32414c53 && schema != 0x33414c53) throw std::runtime_error("Unsupported LA state schema.");
  Patch p;
  auto roms = std::make_shared<RomReferences>();
  roms->controlPath = text(); roms->pcmPath = text(); roms->controlHash = text(); roms->pcmHash = text(); p.roms = roms;
  uint64_t gain = u32(); gain |= uint64_t(u32()) << 32; p.volume = std::bit_cast<double>(gain);
  for(auto& v : p.values) { const auto n = u32(); if(n > 127) throw std::runtime_error("Invalid LA control value."); v = int(n); }
  if(schema != 0x31414c53)
  {
    for(auto& banks : p.bankPrograms) for(auto& program : banks)
    {
      const auto n = u32();
      if(n > 127) throw std::runtime_error("Invalid LA remembered program.");
      program = int(n);
    }
    auto hashes = std::make_shared<RomHashes>();
    for(auto& hash : *hashes) hash = text();
    p.soundROMHashes = std::move(hashes);
  }
  else
  {
    for(int part = 0; part < 8; ++part)
    {
      const int bank = p.values[partControl(part, Bank)];
      if(bank > 4) throw std::runtime_error("Invalid LA bank.");
      p.bankPrograms[part][bank] = p.values[partControl(part, Program)];
    }
  }
  if(in.size() - at != 64 * 246 + (schema == 0x33414c53 ? 4 : 0)) throw std::runtime_error("Invalid LA timbre bank length.");
  for(auto& t : p.timbres) { std::copy_n(in.begin() + at, t.size(), t.begin()); at += t.size(); }
  if(schema == 0x33414c53)
  {
    const auto allocation = u32();
    if(allocation > 2) throw std::runtime_error("Invalid LA voice allocation.");
    p.allocation = Allocation(allocation);
  }
  if(const auto* e = validate(p)) throw std::runtime_error(e);
  return p;
}
}
