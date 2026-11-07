// SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine.hpp"
#include <Emulation/Volume.hpp>
#include <Emulation/HeldNotes.hpp>
#include <emu2149.h>
extern "C" {
#include <ympsg.h>
}
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Emulation::Psg
{
const char* modelTag(Model model) noexcept
{
  switch(model) {
    case Model::Ay8910: return "AY-3-8910";
    case Model::Ym2149: return "YM2149";
    case Model::SegaYm7101: return "Sega-YM7101";
  }
  return "unsupported";
}
const char* validate(const Settings& s) noexcept
{
  if(s.model < Model::Ay8910 || s.model > Model::SegaYm7101) return "Unsupported PSG model.";
  if(s.clock < 100000 || s.clock > 4000000) return "PSG clock must be 100000 to 4000000 Hz.";
  if(s.assignment < Assignment::Polyphonic || s.assignment > Assignment::Mono) return "Unsupported PSG voice allocation.";
  if(!std::isfinite(s.volume) || s.volume < 0. || s.volume > MaxVolume) return "PSG Volume must be finite and in [0, 32].";
  for(const auto& c : s.channels) {
    if(c.level < 0 || c.level > 15) return "PSG channel level must be 0 to 15.";
  }
  if(s.noisePeriod < 0 || s.noisePeriod > 31 || s.envelopePeriod < 0 || s.envelopePeriod > 65535
      || s.envelopeShape < 0 || s.envelopeShape > 15) return "Invalid AY shared noise or envelope setting.";
  if(s.segaNoiseRate < 0 || s.segaNoiseRate > 3 || s.segaNoiseLevel < 0 || s.segaNoiseLevel > 15)
    return "Invalid Sega shared noise setting.";
  if(s.attackMs < 0 || s.attackMs > 10000 || s.decayMs < 0 || s.decayMs > 10000
      || s.releaseMs < 0 || s.releaseMs > 10000 || s.sustain < 0 || s.sustain > 15)
    return "Invalid software envelope setting.";
  return nullptr;
}
struct Engine::Impl
{
  struct Voice {
    int note{-1}, channel{}, velocity{};
    uint64_t age{};
    bool held{}, sustained{};
    int stage{}; // attack, decay, sustain, release
    double envelope{1.}, releaseStep{};
  };
  Settings settings;
  double rate;
  PSG* ay{};
  ympsg_t sega{};
  std::array<Voice, 4> voices{};
  HeldNotes notes;
  std::array<int, 16> bend{}, ccVolume{};
  std::array<bool, 16> pedal{};
  std::array<int, 8> desired{}, applied{};
  int writeDelay{}, scan{}, highByte{-1}, highRegister{}, highValue{};
  double clockRemainder{}, clockOutput{};
  const char* configurationError{};
  const char* midiError{};
  bool pitchDirty{true};
  bool isSega() const noexcept { return settings.model == Model::SegaYm7101; }
  Impl(double sampleRate, Settings s) : settings{s}, rate{sampleRate}
  {
    if(auto e = validate(s)) throw std::runtime_error(e);
    if(!std::isfinite(rate) || rate < 8000 || rate > 384000 || std::floor(rate) != rate)
      throw std::runtime_error("PSG requires an integer sample rate from 8000 to 384000 Hz.");
    ccVolume.fill(127);
    applied.fill(-1);
    desired.fill(0);
    for(int i : {1, 3, 5, 7}) desired[i] = 15;
    if(isSega()) {
      YMPSG_Init(&sega);
      // Init deasserts IC without clocking it through the reset pipeline.
      // Finish reset before the first tone-latch byte, or its low nibble is erased.
      for(int i = 0; i < 16; ++i) YMPSG_Clock(&sega);
    }
    else {
      ay = PSG_new(s.clock, static_cast<uint32_t>(rate));
      if(!ay) throw std::bad_alloc{};
      PSG_reset(ay);
      PSG_setVolumeMode(ay, s.model == Model::Ay8910 ? 2 : 1);
      PSG_setQuality(ay, 1);
    }
    updateShared(true);
    updateRegisters();
  }
  ~Impl() { if(ay) PSG_delete(ay); }
  void updateShared(bool retrigger) noexcept
  {
    if(isSega()) desired[6] = settings.segaNoiseRate | (settings.segaWhiteNoise ? 4 : 0);
    else {
      PSG_writeReg(ay, 6, settings.noisePeriod);
      PSG_writeReg(ay, 11, settings.envelopePeriod & 255);
      PSG_writeReg(ay, 12, settings.envelopePeriod >> 8);
      if(retrigger) PSG_writeReg(ay, 13, settings.envelopeShape);
    }
  }
  void stop(Voice& v) noexcept
  {
    v.held = v.sustained = false;
    if(settings.softwareEnvelope && settings.releaseMs > 0 && v.note >= 0) {
      v.stage = 3;
      v.releaseStep = v.envelope / (rate * settings.releaseMs / 1000.);
    } else v = {};
  }
  void start(int slot, HeldNotes::Selection selected) noexcept
  {
    const auto& key = notes.keys[selected.channel][selected.note];
    voices[slot] = {selected.note, selected.channel, key.velocity, key.order,
        key.held, !key.held, 0,
        settings.softwareEnvelope && settings.attackMs > 0 ? 0. : 1., 0.};
    if(settings.softwareEnvelope && settings.attackMs == 0) voices[slot].stage = 1;
    // One envelope generator: each hardware-envelope note-on restarts all AY users.
    if(!isSega() && !settings.softwareEnvelope && settings.channels[slot].hardwareEnvelope)
      PSG_writeReg(ay, 13, settings.envelopeShape);
  }
  void choose(int channel) noexcept
  {
    const bool noise = isSega() && channel == 9;
    if(channel >= 3 && !noise) return;
    const auto selected = notes.latest(channel, isSega() && channel < 0 ? 9 : -1);
    const int first = channel < 0 ? 0 : noise ? 3 : channel;
    const int end = channel < 0 ? 3 : first + 1;
    for(int slot = first; slot < end; ++slot) {
      auto& voice = voices[slot];
      if(!selected.order) { if(voice.held || voice.sustained) stop(voice); }
      else if(voice.age != selected.order || voice.note < 0) start(slot, selected);
      else {
        voice.held = notes.keys[selected.channel][selected.note].held;
        voice.sustained = !voice.held;
      }
    }
  }
  void chooseChannel(int ch) noexcept
  {
    if(isSega() && ch == 9) choose(9);
    else if(settings.assignment == Assignment::Mono) choose(-1);
    else if(settings.assignment == Assignment::Fixed) choose(ch);
  }
  void resetChannel(int ch, bool immediate) noexcept
  {
    notes.stopChannel(ch, !immediate && pedal[ch]);
    for(auto& v : voices) if(v.note >= 0 && v.channel == ch) {
      if(immediate) v = {};
      else if(v.held) { v.held = false; if(pedal[ch]) v.sustained = true; else stop(v); }
    }
    chooseChannel(ch);
  }
  void releasePedal(int ch) noexcept
  {
    pedal[ch] = false;
    notes.releasePedal(ch);
    for(auto& v : voices) if(v.channel == ch && v.sustained) stop(v);
    chooseChannel(ch);
  }
  void noteOn(int ch, int note, int velocity) noexcept
  {
    const bool noise = isSega() && ch == 9;
    const bool accepted = noise ? settings.segaNoise : settings.assignment != Assignment::Fixed || ch < 3;
    if(!notes.press(ch, note, velocity, accepted)) {
      midiError = "PSG note lifetime counter exhausted; note rejected."; return;
    }
    if(!accepted) return;
    if(noise || settings.assignment != Assignment::Polyphonic) { chooseChannel(ch); return; }
    int slot = -1;
    for(int i = 0; i < 3; ++i)
      if(voices[i].note == note && voices[i].channel == ch) { slot = i; break; }
    if(slot < 0) {
      slot = 0;
      for(int i = 0; i < 3; ++i) {
        if(voices[i].note < 0) { slot = i; break; }
        if((voices[i].stage == 3 && voices[slot].stage != 3)
            || ((voices[i].stage == 3) == (voices[slot].stage == 3) && voices[i].age < voices[slot].age)) slot = i;
      }
    }
    start(slot, {ch, note, notes.keys[ch][note].order});
  }
  void noteOff(int ch, int note) noexcept
  {
    if(!notes.release(ch, note, pedal[ch])) return;
    if(settings.assignment != Assignment::Polyphonic || (isSega() && ch == 9)) {
      chooseChannel(ch); return;
    }
    for(auto& v : voices) if(v.note == note && v.channel == ch && v.held) {
      v.held = false;
      if(pedal[ch]) v.sustained = true;
      else stop(v);
    }
  }
  void stopNotes() noexcept
  {
    voices = {};
    notes.stop();
    updateRegisters();
  }
  void panic() noexcept
  {
    notes = {}; pedal = {}; bend = {}; ccVolume.fill(127);
    stopNotes();
  }
  void envelope(Voice& v) noexcept
  {
    if(!settings.softwareEnvelope || v.note < 0) return;
    const double sustain = settings.sustain / 15.;
    switch(v.stage) {
      case 0:
        v.envelope = settings.attackMs == 0 ? 1. : std::min(1., v.envelope + 1000. / (rate * settings.attackMs));
        if(v.envelope >= 1.) v.stage = 1;
        break;
      case 1:
        v.envelope = settings.decayMs == 0 ? sustain : std::max(sustain, v.envelope - (1. - sustain) * 1000. / (rate * settings.decayMs));
        if(v.envelope <= sustain) v.stage = 2;
        break;
      case 2: v.envelope = sustain; break;
      case 3: v.envelope = std::max(0., v.envelope - v.releaseStep); if(v.envelope == 0.) v = {}; break;
    }
  }
  void updateRegisters() noexcept
  {
    int mixer = 0;
    for(int i = 0; i < (isSega() ? 4 : 3); ++i) {
      const auto& v = voices[i];
      const bool active = v.note >= 0;
      const int level = i == 3 ? settings.segaNoiseLevel : settings.channels[i].level;
      const double amplitude = active ? level * (v.velocity / 127.) * (ccVolume[v.channel] / 127.)
          * (settings.softwareEnvelope ? v.envelope : 1.) : 0.;
      const int nativeLevel = std::clamp(int(std::lround(amplitude)), 0, 15);
      if(i < 3 && active && pitchDirty) {
        const double hz = 440. * std::exp2((v.note - 69 + 2. * bend[v.channel] / 8192.) / 12.);
        const double raw = settings.clock / ((isSega() ? 32. : 16.) * hz);
        const int period = std::clamp(int(std::lround(raw)), 1, isSega() ? 1023 : 4095);
        if(isSega()) desired[2 * i] = period;
        else if(PSG_readReg(ay, 2 * i) != (period & 255) || PSG_readReg(ay, 2 * i + 1) != (period >> 8)) {
          PSG_writeReg(ay, 2 * i, period & 255); PSG_writeReg(ay, 2 * i + 1, period >> 8);
        }
      }
      if(isSega()) {
        const bool enabled = i == 3 ? settings.segaNoise : settings.channels[i].tone;
        desired[2 * i + 1] = enabled ? 15 - nativeLevel : 15;
      } else {
        const auto& c = settings.channels[i];
        if(!active || !c.tone) mixer |= 1 << i;
        if(!active || !c.noise) mixer |= 8 << i;
        const int volume = active && !settings.softwareEnvelope && c.hardwareEnvelope ? 16 : nativeLevel;
        if(PSG_readReg(ay, 8 + i) != volume) PSG_writeReg(ay, 8 + i, volume);
      }
    }
    if(!isSega() && PSG_readReg(ay, 7) != mixer) PSG_writeReg(ay, 7, mixer);
    pitchDirty = false;
  }
  void segaClock() noexcept
  {
    // Bounded register coalescing, not upstream's overflow-prone buffered writer.
    // Each bus byte receives eight real clocks; two-byte tone writes stay paired.
    if(writeDelay == 0) {
      if(highByte >= 0) {
        YMPSG_Write(&sega, uint8_t(highByte)); highByte = -1;
        applied[highRegister] = highValue; writeDelay = 8;
      } else {
        for(int n = 0; n < 8; ++n) {
          const int reg = scan; scan = (scan + 1) % 8;
          if(desired[reg] == applied[reg]) continue;
          const int value = desired[reg];
          YMPSG_Write(&sega, uint8_t(0x80 | (reg << 4) | (value & 15)));
          if(!(reg & 1) && reg != 6) {
            highByte = value >> 4; highRegister = reg; highValue = value;
          } else applied[reg] = value;
          writeDelay = 8;
          break;
        }
      }
    }
    YMPSG_Clock(&sega);
    if(writeDelay) --writeDelay;
    clockOutput = YMPSG_GetOutput(&sega) / 4.;
  }
  double sample() noexcept
  {
    if(settings.softwareEnvelope) {
      for(auto& v : voices) envelope(v);
      updateRegisters();
    }
    if(!isSega()) return PSG_calc(ay) / 12240.;
    const double duration = settings.clock / rate;
    double remaining = duration, sum = 0.;
    while(remaining > 0.) {
      if(clockRemainder <= 0.) { segaClock(); clockRemainder = 1.; }
      const double step = std::min(remaining, clockRemainder);
      sum += step * clockOutput; remaining -= step; clockRemainder -= step;
    }
    return sum / duration;
  }
};
Engine::Engine(double rate, Settings s) : m{std::make_unique<Impl>(rate, s)} { }
Engine::~Engine() = default;
bool Engine::configure(const Settings& s) noexcept
{
  if(auto e = validate(s)) { m->configurationError = e; return false; }
  if(s.model != m->settings.model || s.clock != m->settings.clock) {
    m->configurationError = "Model and clock changes require engine preparation."; return false;
  }
  const bool reset = s.assignment != m->settings.assignment;
  const bool retrigger = s.envelopeShape != m->settings.envelopeShape;
  const bool softwareChanged = s.softwareEnvelope != m->settings.softwareEnvelope;
  m->settings = s;
  m->pitchDirty = true;
  if(reset || softwareChanged) m->stopNotes();
  if(!s.segaNoise) { m->voices[3] = {}; if(m->isSega()) m->notes.stopChannel(9, false); }
  m->updateShared(retrigger); m->updateRegisters(); m->configurationError = nullptr;
  return true;
}
void Engine::midi(std::span<const uint8_t> bytes) noexcept
{
  if(bytes.empty()) return;
  const int status = bytes[0], ch = status & 15;
  if(status == 0xff) { m->midiError = nullptr; panic(); return; }
  if(status >= 0xf8) { m->midiError = nullptr; return; }
  if(status < 0x80 || status >= 0xf0) { m->midiError = "PSG accepts channel MIDI, not SysEx or system common messages."; return; }
  const int kind = status & 0xf0;
  const std::size_t length = kind == 0xc0 || kind == 0xd0 ? 2 : 3;
  if(bytes.size() != length || bytes[1] > 127 || (length == 3 && bytes[2] > 127)) {
    m->midiError = "Malformed PSG MIDI message."; return;
  }
  m->midiError = nullptr;
  switch(kind) {
    case 0x80: m->noteOff(ch, bytes[1]); break;
    case 0x90: if(bytes[2]) m->noteOn(ch, bytes[1], bytes[2]); else m->noteOff(ch, bytes[1]); break;
    case 0xe0: m->bend[ch] = (bytes[1] | (bytes[2] << 7)) - 8192; break;
    case 0xb0:
      switch(bytes[1]) {
        case 7: m->ccVolume[ch] = bytes[2]; break;
        case 64:
          if(bytes[2] >= 64) m->pedal[ch] = true;
          else m->releasePedal(ch);
          break;
        case 120: m->resetChannel(ch, true); break;
        case 121:
          m->bend[ch] = 0; m->ccVolume[ch] = 127;
          m->releasePedal(ch);
          break;
        case 123: m->resetChannel(ch, false); break;
        default: if(bytes[1] >= 124) m->resetChannel(ch, false); break;
      }
      break;
    default: break; // No program map or pressure modulation is claimed.
  }
  m->pitchDirty = true;
  m->updateRegisters();
}
void Engine::panic() noexcept { m->panic(); }
void Engine::render(double* left, double* right, std::size_t frames) noexcept
{
  for(std::size_t i = 0; i < frames; ++i) {
    const double sample = m->sample();
    // Zero remains exact while clocks and envelopes continue to advance.
    left[i] = right[i] = m->settings.volume == 0. ? 0. : sample * m->settings.volume;
  }
}
const char* Engine::error() const noexcept
{ return m->configurationError ? m->configurationError : m->midiError; }
int Engine::activeVoices() const noexcept
{ return int(std::count_if(m->voices.begin(), m->voices.end(), [](const auto& v) { return v.note >= 0; })); }
int Engine::voiceNote(int slot) const noexcept { return slot >= 0 && slot < 4 ? m->voices[slot].note : -1; }
int Engine::registerValue(int reg) const noexcept
{
  if(m->isSega()) return reg >= 0 && reg < 8 ? m->desired[reg] : -1;
  return reg >= 0 && reg < 16 ? PSG_readReg(m->ay, reg) : -1;
}
}
