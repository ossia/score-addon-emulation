// SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine.hpp"
#include "VoicePolicy.hpp"
#include <mt32emu/mt32emu.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
namespace Emulation::La
{
namespace
{
constexpr uint32_t systemAddress = 0x40000, patchAddress = 0xc000, timbreAddress = 0x20000;
struct Reports final : MT32Emu::ReportHandler
{
  const char* error{};
  const char* midiError{};
  void printDebug(const char*, va_list) override { }
  void showLCDMessage(const char*) override { }
  bool onMIDIQueueOverflow() override { midiError = "LA MIDI queue overflow; reduce event density."; return false; }
};
struct Image
{
  std::vector<uint8_t> bytes;
  std::unique_ptr<MT32Emu::ArrayFile> file;
  struct Delete { void operator()(const MT32Emu::ROMImage* p) const { if(p) MT32Emu::ROMImage::freeROMImage(p); } };
  std::unique_ptr<const MT32Emu::ROMImage, Delete> image;
  void load(const std::string& path, const std::string& hash, MT32Emu::ROMInfo::Type type, const std::atomic<bool>* cancelled)
  {
    const auto label = type == MT32Emu::ROMInfo::Control ? "control" : "PCM";
    if(path.empty()) throw std::runtime_error(std::string{"Select a user-provided LA "} + label + " ROM file; no ROMs are distributed with score.");
    const std::filesystem::path filePath{
        std::u8string_view{reinterpret_cast<const char8_t*>(path.data()), path.size()}};
    std::ifstream stream{filePath, std::ios::binary | std::ios::ate};
    if(!stream) throw std::runtime_error(std::string{"Cannot open LA "} + label + " ROM: " + path + ". Relink the file in ROM settings.");
    const auto size = stream.tellg();
    if(size <= 0 || size > 2 * 1024 * 1024) throw std::runtime_error(std::string{"Invalid LA "} + label + " ROM size: " + path);
    bytes.resize(std::size_t(size)); stream.seekg(0);
    for(std::size_t at = 0; at < bytes.size();)
    {
      if(cancelled && cancelled->load(std::memory_order_relaxed)) throw std::runtime_error("LA preparation cancelled.");
      const auto n = std::min<std::size_t>(65536, bytes.size() - at);
      if(!stream.read(reinterpret_cast<char*>(bytes.data() + at), n)) throw std::runtime_error("Could not read the complete LA ROM file: " + path);
      at += n;
    }
    file = std::make_unique<MT32Emu::ArrayFile>(bytes.data(), bytes.size());
    if(!hash.empty() && hash != file->getSHA1()) throw std::runtime_error(std::string{"LA "} + label + " ROM SHA-1 differs from the saved reference. Select the intended file again: " + path);
    image.reset(MT32Emu::ROMImage::makeROMImage(file.get()));
    const auto* info = image ? image->getROMInfo() : nullptr;
    if(!info) throw std::runtime_error(std::string{"Unrecognized LA "} + label + " ROM contents (SHA-1 " + file->getSHA1() + "). Use an unmodified ROM supported by munt, not a renamed or padded file.");
    if(info->type != type) throw std::runtime_error(std::string{"Wrong LA ROM type for "} + label + " input: " + info->description);
    if(info->pairType != MT32Emu::ROMInfo::Full) throw std::runtime_error("Partial LA ROM dump selected. Supply the complete, correctly joined ROM image, not an individual chip half.");
  }
};
}
struct Engine::Impl
{
  Image controlROM, pcmROM;
  Reports reports;
  MT32Emu::Synth synth{&reports};
  std::unique_ptr<MT32Emu::SampleRateConverter> converter;
  Patch patch;
  RomReferences resolved;
  std::string machine;
  std::shared_ptr<const ProgramCatalog> catalog;
  std::array<float, 1024> samples{};
  uint64_t frames{};
  VoicePolicy voices;
  std::array<HeldNotes::Selection, 8> sounding{};
  void queuePart(int part, int command, int data1, int data2) noexcept
  {
    const auto timestamp = uint32_t(converter->convertOutputToSynthTimestamp(double(frames)));
    if(!synth.playMsgOnPart(part, command, data1, data2, timestamp))
      reports.midiError = "LA MIDI queue is full.";
  }
  std::array<uint8_t, 9> routes() noexcept
  {
    std::array<uint8_t, 9> result{};
    synth.readMemory(systemAddress + 13, result.size(), result.data());
    return result;
  }
  void note(int channel, int key, int velocity) noexcept
  {
    if(voices.mode == Allocation::Poly)
    {
      const uint32_t message = uint32_t((velocity ? 0x90 : 0x80) | channel)
          | (uint32_t(key) << 8) | (uint32_t(velocity) << 16);
      const auto timestamp = uint32_t(converter->convertOutputToSynthTimestamp(double(frames)));
      const bool retriggerRelease = !velocity && voices.notes.keys[channel][key].held;
      const bool queued = retriggerRelease
          ? synth.releaseNoteOnChannel(channel, key, timestamp) : synth.playMsg(message, timestamp);
      if(!queued) reports.midiError = "LA MIDI queue is full.";
      return;
    }
    const auto receive = routes();
    for(int part = 0; part < 8; ++part)
    {
      if(velocity && receive[part] == channel)
      {
        queuePart(part, 0x9, key, velocity);
        sounding[part] = {channel, key, 0};
      }
      else if(!velocity && sounding[part].channel == channel && sounding[part].note == key)
      {
        queuePart(part, 0x8, key, 0);
        sounding[part] = {};
      }
    }
  }
  void allocation(Allocation mode) noexcept
  {
    // Drain delayed native notes BEFORE stopping, otherwise they can reappear.
    // Part CC120 invokes native decay without resetting channel controllers.
    synth.flushMIDIQueue();
    const auto receive = routes();
    for(int part = 0; part < 9; ++part)
    {
      synth.playMsgOnPart(part, 0xb, 120, 0);
      const bool pedal = (mode == Allocation::Poly || part == 8)
          && receive[part] < 16 && voices.sustain[receive[part]];
      synth.playMsgOnPart(part, 0xb, 64, pedal ? 127 : 0);
    }
    voices.stop();
    sounding = {};
    voices.mode = mode;
  }
  bool cm32{}, oldMT32{}, silenced{};
  struct Memory
  {
    std::array<Timbre, 64> user;
    std::array<Timbre, 8> temporary;
    std::array<uint8_t, 1024> patches;
    std::array<uint8_t, 340> rhythm;
    std::array<uint8_t, 144> parts;
    std::array<uint8_t, 23> system;
  } memory;
  void write(uint32_t address, std::span<const uint8_t> bytes) noexcept
  {
    std::array<uint8_t, 259> data{};
    // Public writeSysex accepts an unpacked address followed by data (no framing/checksum).
    while(!bytes.empty())
    {
      const auto n = std::min<std::size_t>(256, bytes.size());
      data[0] = (address >> 14) & 127; data[1] = (address >> 7) & 127; data[2] = address & 127;
      std::copy_n(bytes.begin(), n, data.begin() + 3);
      synth.writeSysex(0x10, data.data(), uint32_t(n + 3));
      address += n; bytes = bytes.subspan(n);
    }
  }
  void byte(uint32_t address, int v) noexcept { const uint8_t b = uint8_t(v); write(address, {&b, 1}); }
  bool compatible(const Patch& p) noexcept
  {
    if(!cm32)
    {
      for(const auto& t : p.timbres) for(int i = 0; i < 4; ++i)
        if(t[14 + i * 58 + 4] > 1) { reports.error = "PCM bank 2 requires CM-32L-compatible ROMs; MT-32 supports wave / PCM bank values 0 and 1."; return false; }
    }
    if(oldMT32)
      for(int i = 0; i < 8; ++i)
        if(p.values[partControl(i, Bank)] == 4 && p.values[partControl(i, Program)] > 29)
        { reports.error = "Old MT-32 rhythm bank contains timbres 0..29; use new MT-32 or CM-32L-compatible ROMs for the extended rhythm bank."; return false; }
    return true;
  }
  void part(int part, bool select) noexcept
  {
    const auto value = [&](PartField f) { return patch.values[partControl(part, f)]; };
    const auto addr = patchAddress + part * 16;
    if(select)
    {
      if(value(Bank) == 0) synth.playMsgOnPart(uint8_t(part), 0xc, uint8_t(value(Program)), 0);
      else
      {
        constexpr uint8_t groups[] = {0,2,0,1,3};
        const uint8_t selection[] = {groups[value(Bank)], uint8_t(value(Program))};
        write(addr, selection);
      }
    }
    const uint8_t params[] = {uint8_t(value(KeyShift)), uint8_t(value(FineTune)), uint8_t(value(BendRange)),
      uint8_t(value(AssignMode)), uint8_t(value(Reverb)), 0, uint8_t(value(Level)), uint8_t(value(Pan))};
    write(addr + 2, params);
    byte(systemAddress + 13 + part, value(Channel)); byte(systemAddress + 4 + part, value(Reserve));
  }
  void setting(int c) noexcept
  {
    const auto v = patch.values[c];
    if(c >= PartBase && c < EditTimbre)
    {
      const auto index = (c - PartBase) / PartFieldCount;
      const auto field = (c - PartBase) % PartFieldCount;
      constexpr int offset[] = {0,0,0,8,9,2,3,4,5,6,0};
      if(field <= Program) part(index, true);
      else if(field == Channel) byte(systemAddress + 13 + index, v);
      else if(field == Reserve) byte(systemAddress + 4 + index, v);
      else byte(patchAddress + index * 16 + offset[field], v);
      return;
    }
    switch(c)
    {
      case ReverbMode: byte(systemAddress + 1, v); synth.setReverbEnabled(patch.values[ReverbEnabled]); break;
      case ReverbTime: byte(systemAddress + 2, v); synth.setReverbEnabled(patch.values[ReverbEnabled]); break;
      case ReverbLevel: byte(systemAddress + 3, v); synth.setReverbEnabled(patch.values[ReverbEnabled]); break;
      case MasterTune: byte(systemAddress, v); break;
      case MasterLevel: byte(systemAddress + 22, v); break;
      case ReverbEnabled: synth.setReverbEnabled(v); break;
      case ReverseStereo: synth.setReversedStereoEnabled(v); break;
      case DacMode: synth.setDACInputMode(MT32Emu::DACInputMode(v)); break;
      case MidiDelay: synth.setMIDIDelayMode(MT32Emu::MIDIDelayMode(v)); break;
      case NiceRamp: synth.setNiceAmpRampEnabled(v); break;
      case NicePan: synth.setNicePanningEnabled(v); break;
      case NiceMix: synth.setNicePartialMixingEnabled(v); break;
      case RhythmChannel: byte(systemAddress + 21, v); break;
      case RhythmLevel: byte(patchAddress + 8 * 16 + 8, v); break;
      case RhythmPan: byte(patchAddress + 8 * 16 + 9, v); break;
      case RhythmReserve: byte(systemAddress + 12, v); break;
      case RhythmReverb:
        for(int note = 0; note < (cm32 ? 85 : 64); ++note) byte(0xc090 + note * 4 + 3, v);
        break;
      default: break;
    }
  }
  void timbre(int slot, std::size_t offset = 0, std::size_t length = 246) noexcept
  {
    const auto bytes = std::span<const uint8_t>{patch.timbres[slot]}.subspan(offset, length);
    write(timbreAddress + slot * 256 + uint32_t(offset), bytes);
    // Memory timbres and temporary timbres are independent on the hardware.
    // Refresh only parts currently using this edited memory timbre (including MIDI/SysEx selections).
    for(int part = 0; part < 8; ++part)
    {
      uint8_t selection[2]{}; synth.readMemory(patchAddress + part * 16, 2, selection);
      if(selection[0] == 2 && selection[1] == slot) write(0x10000 + part * 246 + uint32_t(offset), bytes);
    }
  }
};
Engine::Engine(double rate, const Patch& p, const std::atomic<bool>* cancelled) : m{std::make_unique<Impl>()}
{
  if(const auto* e = validate(p)) throw std::runtime_error(e);
  if(!std::isfinite(rate) || rate < 8000 || rate > 384000) throw std::runtime_error("LA sample rate must be 8000..384000 Hz.");
  m->controlROM.load(p.roms->controlPath, p.roms->controlHash, MT32Emu::ROMInfo::Control, cancelled);
  m->pcmROM.load(p.roms->pcmPath, p.roms->pcmHash, MT32Emu::ROMInfo::PCM, cancelled);
  for(auto configs = MT32Emu::MachineConfiguration::getAllMachineConfigurations(); *configs; ++configs)
  {
    bool control{}, pcm{};
    for(auto infos = (*configs)->getCompatibleROMInfos(); *infos; ++infos)
    { control |= *infos == m->controlROM.image->getROMInfo(); pcm |= *infos == m->pcmROM.image->getROMInfo(); }
    if(control && pcm) { m->machine = (*configs)->getMachineID(); break; }
  }
  if(m->machine.empty()) throw std::runtime_error("LA control and PCM ROMs are recognized but mismatched. Choose a PCM ROM compatible with " + std::string{m->controlROM.image->getROMInfo()->description} + ".");
  m->cm32 = m->pcmROM.bytes.size() > 512 * 1024;
  m->synth.preallocateReverbMemory(true);
  m->synth.selectRendererType(MT32Emu::RendererType_FLOAT);
  if(cancelled && cancelled->load(std::memory_order_relaxed)) throw std::runtime_error("LA preparation cancelled.");
  if(!m->synth.open(*m->controlROM.image, *m->pcmROM.image, 32, MT32Emu::AnalogOutputMode_ACCURATE))
    throw std::runtime_error("munt could not open the identified LA ROM pair.");
  m->oldMT32 = m->synth.isDefaultReverbMT32Compatible();
  if(!m->compatible(p)) throw std::runtime_error(m->reports.error);
  m->synth.setMIDIEventQueueSize(16384);
  m->synth.configureMIDIEventQueueSysexStorage(262144);
  m->converter = std::make_unique<MT32Emu::SampleRateConverter>(m->synth, rate, MT32Emu::SamplerateConversionQuality_GOOD);
  m->patch = p;
  m->voices.mode = p.allocation;
  m->resolved = *p.roms;
  m->resolved.controlHash = m->controlROM.file->getSHA1(); m->resolved.pcmHash = m->pcmROM.file->getSHA1();
  for(int slot = 0; slot < 64; ++slot) m->timbre(slot);
  for(int c = ReverbMode; c < PartBase; ++c) m->setting(c);
  for(int part = 0; part < 8; ++part) m->part(part, true);
  auto catalog = std::make_shared<ProgramCatalog>();
  catalog->rhythmPrograms = m->oldMT32 ? 30 : 64;
  const auto soundName = [&](int group, int number) {
    char name[11]{};
    if(!m->synth.getSoundName(name, uint8_t(group), uint8_t(number))) return std::string{};
    std::string text{name};
    const auto end = text.find_last_not_of(' ');
    text.resize(end == std::string::npos ? 0 : end + 1);
    return text;
  };
  for(int group = 0; group < 4; ++group)
    for(int number = 0; number < 64; ++number)
      catalog->timbres[group][number] = soundName(group, number);
  for(int program = 0; program < 128; ++program)
  {
    uint8_t selection[2]{};
    m->synth.readMemory(0x14000 + program * 8, 2, selection);
    catalog->factory[program] = soundName(selection[0], selection[1]);
  }
  m->catalog = std::move(catalog);
}
Engine::~Engine() = default;
bool Engine::setPatch(const Patch& p) noexcept
{
  if(const auto* e = validate(p)) { m->reports.error = e; return false; }
  if(!sameSetup(p, m->patch)) { m->reports.error = "LA ROM changes require off-thread engine preparation."; return false; }
  if(!m->compatible(p)) return false;
  m->reports.error = nullptr;
  if(p.allocation != m->patch.allocation) m->allocation(p.allocation);
  m->patch.allocation = p.allocation;
  for(int slot = 0; slot < 64; ++slot)
    if(p.timbres[slot] != m->patch.timbres[slot]) { m->patch.timbres[slot] = p.timbres[slot]; m->timbre(slot); }
  for(int c = ReverbMode; c < PartBase; ++c)
    if(p.values[c] != m->patch.values[c]) { m->patch.values[c] = p.values[c]; m->setting(c); }
  for(int part = 0; part < 8; ++part)
  {
    std::array<bool, PartFieldCount> changed{};
    for(int field = 0; field < PartFieldCount; ++field)
    {
      const auto c = PartBase + part * PartFieldCount + field;
      changed[field] = p.values[c] != m->patch.values[c];
      m->patch.values[c] = p.values[c];
    }
    if(changed[Bank] || changed[Program]) m->part(part, true);
    else for(int field = Channel; field < PartFieldCount; ++field)
      if(changed[field]) m->setting(PartBase + part * PartFieldCount + field);
  }
  m->patch.values[EditTimbre] = p.values[EditTimbre]; m->patch.volume = p.volume;
  m->patch.bankPrograms = p.bankPrograms;
  m->patch.soundROMHashes = p.soundROMHashes;
  return true;
}
bool Engine::control(int c, double value) noexcept
{
  const auto old = controlValue(m->patch, c);
  const int part = c >= PartBase && c < EditTimbre ? (c - PartBase) / PartFieldCount : -1;
  const auto programs = part >= 0 ? m->patch.bankPrograms[part] : std::array<int, 5>{};
  const int bank = part >= 0 ? m->patch.values[partControl(part, Bank)] : 0;
  const int program = part >= 0 ? m->patch.values[partControl(part, Program)] : 0;
  if(!setControl(m->patch, c, value)) { m->reports.error = "Invalid LA automation value."; return false; }
  // Validate only dependent settings: avoid scanning/copying the 64-timbre bank per event.
  bool valid = true;
  if(c >= PartBase && c < EditTimbre)
  {
    const auto part = (c - PartBase) / PartFieldCount;
    valid = !m->patch.values[partControl(part, Bank)] || m->patch.values[partControl(part, Program)] <= 63;
    if(m->oldMT32 && m->patch.values[partControl(part, Bank)] == 4) valid &= m->patch.values[partControl(part, Program)] <= 29;
  }
  if(c >= TimbreBase + 4 && c < VoiceAllocation && (c - TimbreBase - 4) % 58 == 4 && !m->cm32) valid &= value <= 1;
  if(c == RhythmReserve || (c >= PartBase && c < EditTimbre && (c - PartBase) % PartFieldCount == Reserve))
  {
    int total = m->patch.values[RhythmReserve];
    for(int part = 0; part < 8; ++part) total += m->patch.values[partControl(part, Reserve)];
    valid &= total <= 32;
  }
  if(!valid)
  {
    setControl(m->patch, c, old);
    if(part >= 0)
    {
      m->patch.bankPrograms[part] = programs;
      m->patch.values[partControl(part, Bank)] = bank;
      m->patch.values[partControl(part, Program)] = program;
    }
    m->reports.error = "LA automation violates bank compatibility or the 32-partial reserve limit.";
    return false;
  }
  m->reports.error = nullptr;
  if(c == VoiceAllocation)
  {
    if(old != value) m->allocation(m->patch.allocation);
  }
  else if(c >= TimbreBase) m->timbre(m->patch.values[EditTimbre], 10 + c - TimbreBase, 1);
  else if(c != Volume && c != EditTimbre) m->setting(c);
  return true;
}
void Engine::midi(std::span<const uint8_t> bytes) noexcept
{
  if(bytes.empty()) return;
  const auto timestamp = uint32_t(m->converter->convertOutputToSynthTimestamp(double(m->frames)));
  if(bytes[0] == 0xf0)
  {
    if(bytes.size() < 2 || bytes.back() != 0xf7) { m->reports.midiError = "Incomplete LA SysEx message."; return; }
    m->reports.midiError = nullptr;
    if(!m->synth.playSysex(bytes.data(), uint32_t(bytes.size()), timestamp)) m->reports.midiError = "LA SysEx queue is full.";
    return;
  }
  if(bytes[0] == 0xff) { panic(); m->reports.midiError = nullptr; return; }
  if(bytes[0] < 0x80 || bytes[0] >= 0xf0) return;
  const auto kind = bytes[0] & 0xf0;
  const std::size_t size = kind == 0xc0 || kind == 0xd0 ? 2 : 3;
  if(bytes.size() != size || bytes[1] >= 128 || (size == 3 && bytes[2] >= 128)) return;
  m->reports.midiError = nullptr;
  const int channel = bytes[0] & 15;
  const auto emit = [&](int ch, int key, int velocity) { m->note(ch, key, velocity); };
  const bool selected = m->voices.mode != Allocation::Poly;
  const auto receive = m->routes();
  if(kind == 0x80 || kind == 0x90)
  {
    const int velocity = kind == 0x90 ? bytes[2] : 0;
    if(velocity) m->silenced = false;
    if(selected && receive[8] == channel)
      m->queuePart(8, kind >> 4, bytes[1], bytes[2]);
    const bool routed = !selected || std::find(receive.begin(), receive.begin() + 8, channel) != receive.begin() + 8;
    m->voices.note(channel, bytes[1], velocity, routed, emit);
    return;
  }
  if(kind == 0xb0)
  {
    const int controller = bytes[1], value = bytes[2];
    if(controller == 64)
    {
      if(selected && receive[8] == channel)
        m->queuePart(8, 0xb, 64, value);
      m->voices.pedal(channel, value >= 64, emit);
      if(selected) return; // Wrapper sustain cannot hold superseded mono notes.
    }
    else if(controller == 120 || controller == 123 || controller >= 124)
    {
      m->synth.flushMIDIQueue();
      for(int part = 0; part < 9; ++part)
        if(receive[part] == channel)
        {
          if(selected && part < 8 && controller == 123 && m->voices.sustain[channel]) continue;
          if(controller == 120) m->synth.playMsgOnPart(part, 0xb, 120, 0);
          else
          {
            if(controller >= 124) m->synth.playMsgOnPart(part, 0xb, 64, 0);
            m->synth.playMsgOnPart(part, 0xb, 123, 0);
          }
        }
      if(controller >= 124) m->voices.sustain[channel] = false;
      m->voices.stopChannel(channel, controller == 123, emit);
      return;
    }
    else if(controller == 121)
    {
      // Apply reset before fallback so the restored note sees reset controllers.
      m->synth.flushMIDIQueue();
      m->synth.playMsgNow(bytes[0] | (uint32_t(121) << 8));
      m->voices.pedal(channel, false, emit);
      return;
    }
  }
  uint32_t msg = bytes[0] | (uint32_t(bytes[1]) << 8);
  if(size == 3) msg |= uint32_t(bytes[2]) << 16;
  if(!m->synth.playMsg(msg, timestamp)) m->reports.midiError = "LA MIDI queue is full.";
}
void Engine::render(double* left, double* right, std::size_t frames) noexcept
{
  while(frames)
  {
    const auto n = std::min<std::size_t>(frames, m->samples.size() / 2);
    m->converter->getOutputSamples(m->samples.data(), unsigned(n));
    const double volume = m->silenced ? 0. : m->patch.volume;
    for(std::size_t i = 0; i < n; ++i)
    { left[i] = volume == 0. ? 0. : m->samples[2*i] * volume; right[i] = volume == 0. ? 0. : m->samples[2*i+1] * volume; }
    m->frames += n; left += n; right += n; frames -= n;
  }
}
void Engine::panic() noexcept
{
  m->voices.stop();
  m->voices.sustain = {};
  m->sounding = {};
  if(m->silenced) return;
  // CC120 is not implemented by the hardware. Preserve all writable sound
  // memory (including received SysEx), reset voices/controllers, then restore
  // via the supported memory API. Buffers live in the prepared engine.
  // Do not use munt 2.8.3 dumpSysexBank: its second rhythm block repeats the
  // first 21 entries rather than preserving CM-32L entries 64..84.
  m->synth.flushMIDIQueue();
  auto& saved = m->memory;
  for(int i = 0; i < 64; ++i) readMemory(timbreAddress + i * 256, saved.user[i]);
  for(int i = 0; i < 8; ++i) readMemory(0x10000 + i * 246, saved.temporary[i]);
  readMemory(0x14000, saved.patches); readMemory(0xc090, saved.rhythm);
  readMemory(patchAddress, saved.parts); readMemory(systemAddress, saved.system);
  const bool reverb = m->synth.isReverbEnabled();
  const uint8_t reset = 0x7f; m->synth.writeSysex(0x10, &reset, 1);
  m->write(systemAddress, saved.system);
  for(int i = 0; i < 64; ++i) m->write(timbreAddress + i * 256, saved.user[i]);
  m->write(0x14000, saved.patches); m->write(0xc090, saved.rhythm);
  m->write(patchAddress, saved.parts);
  for(int i = 0; i < 8; ++i) m->write(0x10000 + i * 246, saved.temporary[i]);
  m->synth.setReverbEnabled(false);
  if(reverb) m->synth.setReverbEnabled(true); // Mutes the preallocated wet buffer.
  m->silenced = true;
}
const Patch& Engine::patch() const noexcept { return m->patch; }
const RomReferences& Engine::resolvedROMs() const noexcept { return m->resolved; }
const std::string& Engine::machine() const noexcept { return m->machine; }
const std::shared_ptr<const ProgramCatalog>& Engine::programCatalog() const noexcept { return m->catalog; }
const char* Engine::lastError() const noexcept { return m->reports.error ? m->reports.error : m->reports.midiError; }
bool Engine::active() const noexcept { return m->synth.hasActivePartials(); }
uint64_t Engine::frames() const noexcept { return m->frames; }
void Engine::readMemory(uint32_t address, std::span<uint8_t> bytes) const noexcept
{ m->synth.readMemory(address, uint32_t(bytes.size()), bytes.data()); }
uint32_t Engine::playingNotes(int part, std::array<uint8_t, 32>& keys,
    std::array<uint8_t, 32>& velocities) const noexcept
{
  return part >= 0 && part <= 8
      ? m->synth.getPlayingNotes(uint8_t(part), keys.data(), velocities.data()) : 0;
}
}
