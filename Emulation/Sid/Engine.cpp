// SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine.hpp"
#include <SID.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace Emulation::Sid
{
namespace { std::mutex preparationMutex; }
struct Engine::Impl
{
  std::unique_ptr<reSIDfp::SID> chip;
  Patch patch;
  double clock{};
  struct Key { uint64_t order{}, outstanding{}; bool held{}; };
  struct Voice {
    int channel{-1}, note{-1};
    uint64_t order{};
    // gate is lifetime ownership; frequencyValid only masks the hardware gate.
    bool gate{}, muted{true}, frequencyValid{true};
    uint16_t frequency{};
    uint8_t control{};
  };
  std::array<std::array<Key, 128>, 16> keys{};
  std::array<Voice, 3> voices{};
  std::array<bool, 16> sustain{};
  std::array<int, 16> bend{};
  std::array<int16_t, 256> buffer{};
  uint64_t serial{}, cycles{};
  int midiVolume{127};
  bool audible{};
  const char* patchError{};
  const char* midiError{};
  const char* lifetimeError{};

  uint8_t control(int v, bool gate) const noexcept
  {
    const auto& o = patch.oscillators[v];
    return uint8_t((o.waveform << 4) | (o.ring ? 4 : 0) | (o.sync ? 2 : 0) | gate);
  }
  void gate(int v, bool on) noexcept
  {
    voices[v].gate = on;
    voices[v].control = voices[v].muted ? 8 : control(v, on && voices[v].frequencyValid);
    chip->write(v * 7 + 4, voices[v].control);
  }
  bool frequency(int v) noexcept
  {
    auto& voice = voices[v];
    if(voice.note < 0) return true;
    const auto& o = patch.oscillators[v];
    const double semitone = voice.note + o.semitones + o.cents / 100.
        + bend[voice.channel] * (2. / 8192.);
    const double reg = 440. * std::exp2((semitone - 69.) / 12.) * 16777216. / clock;
    if(reg < 1. || reg > 65535.)
    {
      voice.frequencyValid = false;
      gate(v, voice.gate);
      return false;
    }
    voice.frequency = uint16_t(std::lround(reg));
    chip->write(v * 7, uint8_t(voice.frequency));
    chip->write(v * 7 + 1, uint8_t(voice.frequency >> 8));
    if(!voice.frequencyValid)
    {
      voice.frequencyValid = true;
      // Restore only the logical gate: released or stolen notes cannot revive.
      gate(v, voice.gate);
      if(voice.gate && !voice.muted) audible = true;
    }
    return true;
  }
  void registers() noexcept
  {
    int routing{};
    for(int v = 0; v < 3; ++v)
    {
      const auto& o = patch.oscillators[v];
      chip->write(v * 7 + 2, uint8_t(o.pulseWidth));
      chip->write(v * 7 + 3, uint8_t(o.pulseWidth >> 8));
      chip->write(v * 7 + 5, uint8_t((o.attack << 4) | o.decay));
      chip->write(v * 7 + 6, uint8_t((o.sustain << 4) | o.release));
      frequency(v);
      gate(v, voices[v].gate);
      routing |= int(o.filterRoute) << v;
    }
    chip->write(0x15, uint8_t(patch.cutoff & 7));
    chip->write(0x16, uint8_t(patch.cutoff >> 3));
    chip->write(0x17, uint8_t((patch.resonance << 4) | routing));
    chip->write(0x18, uint8_t((patch.filterMode << 4) | (patch.voice3Off ? 128 : 0)
        | ((patch.chipVolume * midiVolume + 63) / 127)));
  }
  void start(int v, int ch, int note, uint64_t order) noexcept
  {
    gate(v, false);
    voices[v].muted = false;
    voices[v].channel = ch;
    voices[v].note = note;
    voices[v].order = order;
    if(frequency(v)) audible = true;
    gate(v, true);
  }
  void choose(int channel) noexcept
  {
    if(channel >= 3) return;
    int selectedChannel{-1}, selectedNote{-1};
    uint64_t newest{};
    const int first = channel < 0 ? 0 : channel;
    const int end = channel < 0 ? 16 : channel + 1;
    for(int ch = first; ch < end; ++ch)
      for(int n = 0; n < 128; ++n)
        if(keys[ch][n].order > newest)
        { newest = keys[ch][n].order; selectedChannel = ch; selectedNote = n; }
    const int vFirst = channel < 0 ? 0 : channel;
    const int vEnd = channel < 0 ? 3 : channel + 1;
    for(int v = vFirst; v < vEnd; ++v)
    {
      if(newest == 0) { gate(v, false); voices[v].order = 0; }
      else if(voices[v].order != newest || !voices[v].gate)
        start(v, selectedChannel, selectedNote, newest);
    }
  }
  void noteOn(int ch, int n) noexcept
  {
    auto& key = keys[ch][n];
    if(serial == std::numeric_limits<uint64_t>::max()
        || key.outstanding == std::numeric_limits<uint64_t>::max())
    {
      lifetimeError = "SID note lifetime counter exhausted; note rejected.";
      return;
    }
    const auto order = ++serial;
    key.order = order;
    ++key.outstanding;
    key.held = true;
    if(patch.allocation == Allocation::Mono) { choose(-1); return; }
    if(patch.allocation == Allocation::Fixed) { choose(ch); return; }
    int chosen{-1};
    for(int v = 0; v < 3; ++v)
      if(voices[v].channel == ch && voices[v].note == n && voices[v].gate) { chosen = v; break; }
    if(chosen < 0)
      for(int v = 0; v < 3; ++v) if(!voices[v].gate) { chosen = v; break; }
    if(chosen < 0)
    {
      chosen = 0;
      for(int v = 1; v < 3; ++v) if(voices[v].order < voices[chosen].order) chosen = v;
    }
    // Stealing never resurrects an older note when the new key is released.
    start(chosen, ch, n, order);
  }
  void release(int ch, int n) noexcept
  {
    auto& key = keys[ch][n];
    // Repeated keys still coalesce into one oscillator, but an older note-off
    // must not release the most recently retriggered lifetime.
    if(key.outstanding == 0 || --key.outstanding != 0) return;
    key.held = false;
    if(key.order == 0) return;
    if(sustain[ch]) return;
    keys[ch][n].order = 0;
    if(patch.allocation == Allocation::Mono) choose(-1);
    else if(patch.allocation == Allocation::Fixed) choose(ch);
    else for(int v = 0; v < 3; ++v)
      if(voices[v].channel == ch && voices[v].note == n) { gate(v, false); voices[v].order = 0; }
  }
  void releasePedal(int ch) noexcept
  {
    sustain[ch] = false;
    for(int n = 0; n < 128; ++n)
      if(!keys[ch][n].held) keys[ch][n].order = 0;
    if(patch.allocation == Allocation::Mono) choose(-1);
    else if(patch.allocation == Allocation::Fixed) choose(ch);
    else for(int v = 0; v < 3; ++v)
      if(voices[v].channel == ch && voices[v].note >= 0 && !keys[ch][voices[v].note].held)
      { gate(v, false); voices[v].order = 0; }
  }
  void stopChannel(int ch, bool immediate) noexcept
  {
    for(int n = 0; n < 128; ++n)
    {
      keys[ch][n].held = false;
      if(immediate || !sustain[ch]) keys[ch][n].order = 0;
    }
    if(patch.allocation == Allocation::Mono) choose(-1);
    else if(patch.allocation == Allocation::Fixed) choose(ch);
    else for(int v = 0; v < 3; ++v)
      if(voices[v].channel == ch && (immediate || !sustain[ch])) { gate(v, false); voices[v].order = 0; }
    if(immediate)
      for(int v = 0; v < 3; ++v)
        if(voices[v].channel == ch && !voices[v].gate)
        {
          // Silence this oscillator without resetting other voices or resampler phase.
          voices[v].muted = true;
          voices[v].control = 8;
          chip->write(v * 7 + 4, 8);
        }
  }
  void stopNotes() noexcept
  {
    // Keep outstanding note-offs and MIDI controllers across allocation changes.
    for(auto& channel : keys)
      for(auto& key : channel) { key.order = 0; key.held = false; }
    for(int v = 0; v < 3; ++v)
    {
      gate(v, false);
      chip->write(v * 7 + 4, 8);
      voices[v] = {};
    }
    audible = false;
  }
};
Engine::Engine(double rate, const Patch& patch) try : m{std::make_unique<Impl>()}
{
  if(const char* error = validate(patch)) throw std::runtime_error(error);
  if(!std::isfinite(rate) || rate < 8000. || rate > 192000.)
    throw std::runtime_error("SID supports output sample rates from 8000 to 192000 Hz.");
  // Global lookup construction and FIR allocation never run in render or event handlers.
  std::lock_guard lock{preparationMutex};
  m->patch = patch;
  m->clock = patch.clock == Clock::Pal ? 985248. : 1022730.;
  m->chip = std::make_unique<reSIDfp::SID>();
  m->chip->setChipModel(patch.model == Model::Mos6581 ? reSIDfp::MOS6581 : reSIDfp::CSG8580);
  m->chip->setCombinedWaveforms(static_cast<reSIDfp::CombinedWaveforms>(patch.combinedStrength));
  if(patch.model == Model::Mos6581) m->chip->setFilter6581Curve(patch.filterCurve / 100.);
  else m->chip->setFilter8580Curve(patch.filterCurve / 100.);
  // Do not call setFilter6581Range / set6581caps: they mutate a shared upstream singleton.
  // Keep upstream's default dither: its zero-noise setter has a negative shift at this pin.
  m->chip->setSamplingParameters(m->clock, reSIDfp::RESAMPLE, rate);
  m->registers();
}
catch(const reSIDfp::SIDError& error) { throw std::runtime_error(error.getMessage()); }
Engine::~Engine() = default;
bool Engine::setPatch(const Patch& p) noexcept
{
  if(const char* e = validate(p)) { m->patchError = e; return false; }
  if(!sameSetup(p, m->patch)) { m->patchError = "SID model, clock, curve and combined-waveform strength require off-audio preparation."; return false; }
  auto previous = m->patch;
  previous.volume = p.volume;
  if(previous == p)
  {
    m->patch.volume = p.volume;
    m->patchError = nullptr;
    return true;
  }
  if(p.allocation != m->patch.allocation) m->stopNotes();
  m->patchError = nullptr;
  m->patch = p;
  m->registers();
  return true;
}
void Engine::panic() noexcept
{
  m->keys = {};
  m->sustain = {};
  m->serial = 0;
  m->lifetimeError = nullptr;
  m->stopNotes();
}
void Engine::midi(std::span<const uint8_t> bytes) noexcept
{
  if(bytes.empty()) return;
  const int status = bytes[0];
  if(status == 0xff) { panic(); m->midiError = nullptr; m->bend = {}; m->midiVolume = 127; m->registers(); return; }
  if(status >= 0xf0)
  {
    if(status < 0xf8) m->midiError = "SID accepts MIDI notes, sustain, bend and reset; SysEx is unsupported.";
    return;
  }
  if(status < 0x80) { m->midiError = "Invalid SID MIDI status."; return; }
  const int type = status & 0xf0;
  const int size = type == 0xc0 || type == 0xd0 ? 2 : 3;
  if(int(bytes.size()) != size || bytes[1] > 127 || (size == 3 && bytes[2] > 127))
  { m->midiError = "Invalid SID MIDI message length or data byte."; return; }
  const int ch = status & 15;
  if(m->patch.allocation == Allocation::Fixed && ch >= 3 && type == 0x90 && bytes[2])
  {
    // Even an unmapped note owns its later FIFO note-off across routing changes.
    auto& key = m->keys[ch][bytes[1]];
    if(key.outstanding != std::numeric_limits<uint64_t>::max()) ++key.outstanding;
    m->midiError = "MIDI-channel SID allocation accepts channels 1, 2 and 3 only.";
    return;
  }
  m->midiError = nullptr;
  const int n = bytes[1];
  if(type == 0x90 && bytes[2]) m->noteOn(ch, n);
  else if(type == 0x80 || type == 0x90) m->release(ch, n);
  else if(type == 0xe0)
  {
    m->bend[ch] = (bytes[1] | (int(bytes[2]) << 7)) - 8192;
    for(int v = 0; v < 3; ++v) if(m->voices[v].channel == ch) m->frequency(v);
  }
  else if(type == 0xb0)
  {
    const int value = bytes[2];
    if(n == 64) { if(value >= 64) m->sustain[ch] = true; else m->releasePedal(ch); }
    else if(n == 7) { m->midiVolume = value; m->registers(); }
    else if(n == 120) m->stopChannel(ch, true);
    else if(n == 123 || n >= 124) m->stopChannel(ch, false);
    else if(n == 121)
    {
      m->bend[ch] = 0;
      m->releasePedal(ch);
      m->midiVolume = 127;
      m->registers();
    }
  }
}
void Engine::render(double* left, double* right, std::size_t frames) noexcept
{
  while(frames)
  {
    const auto n = std::min(frames, m->buffer.size());
    m->cycles += m->chip->clock(m->buffer.data(), int(n));
    const double gain = m->audible ? m->patch.volume / 32768. : 0.;
    for(std::size_t i = 0; i < n; ++i) left[i] = right[i] = m->buffer[i] * gain;
    left += n; right += n; frames -= n;
  }
}
const Patch& Engine::patch() const noexcept { return m->patch; }
const char* Engine::lastError() const noexcept
{
  if(m->patchError) return m->patchError;
  if(m->lifetimeError) return m->lifetimeError;
  for(const auto& voice : m->voices)
    if(voice.gate && !voice.muted && !voice.frequencyValid)
      return "Note, tuning and bend exceed the SID frequency-register range.";
  return m->midiError;
}
int Engine::activeVoices() const noexcept
{ int n{}; for(const auto& v : m->voices) n += v.gate && v.frequencyValid && !v.muted; return n; }
int Engine::voiceNote(int v) const noexcept { return v >= 0 && v < 3 ? m->voices[v].note : -1; }
uint16_t Engine::voiceFrequency(int v) const noexcept { return v >= 0 && v < 3 ? m->voices[v].frequency : 0; }
uint8_t Engine::voiceControl(int v) const noexcept { return v >= 0 && v < 3 ? m->voices[v].control : 0; }
uint64_t Engine::clocks() const noexcept { return m->cycles; }
}
