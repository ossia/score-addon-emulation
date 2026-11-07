// SPDX-License-Identifier: GPL-3.0-only
// Signal-chain adaptation of reales/retromulator openWurliLib/device.cpp,
// bb7ffd871c7c227aed0671ba39df8188a7a4697d (GPL v3). DSP headers remain unmodified.
#include "Engine.hpp"
#include <Emulation/Volume.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <owVoice.h>
#include <owDkPreamp.h>
#include <owTremolo.h>
#include <owOversampler.h>
#include <owPowerAmp.h>
#include <owSpeaker.h>
namespace Emulation::Wurli
{
const char* validate(const Settings& s) noexcept
{
  if(!std::isfinite(s.volume) || s.volume < 0.f || s.volume > MaxVolume) return "Wurlitzer Volume must be finite and within 0..32.";
  for(float v : {s.amplifier, s.tremolo, s.speaker})
    if(!std::isfinite(v) || v < 0.f || v > 1.f) return "Wurlitzer controls must be finite and within 0..1.";
  if(s.velocityCurve < 0 || s.velocityCurve > 3) return "Invalid Wurlitzer velocity curve.";
  if(s.voiceAllocation < Allocation::Mono || s.voiceAllocation > Allocation::MidiChannel) return "Invalid Wurlitzer voice allocation.";
  return nullptr;
}
namespace
{
struct Smoother {
  double value{}, target{}, step{};
  unsigned remaining{}, ramp{};
  void init(double v, double rate) noexcept { value = target = v; step = 0.; remaining = 0; ramp = std::max(1u, unsigned(rate * .005)); }
  void set(double v) noexcept {
    if(v == target) return;
    target = v; remaining = ramp; step = (v - value) / remaining;
  }
  double next() noexcept { if(remaining && !--remaining) value = target; else if(remaining) value += step; return value; }
};
enum class State { Free, Releasing, Sustained, Held };
struct Slot {
  openWurli::Voice voice, stolen;
  State state{State::Free};
  uint8_t note{}, channel{}, stolenChannel{};
  uint64_t age{}, noteId{};
  unsigned fade{};
};
struct Channel { bool sustain{}; double volume{1.}, expression{1.}; };
// Per-key monotonically numbered lifetimes include stolen or naturally silent
// held notes. A Note Off consumes one lifetime, even when its voice no longer exists.
struct KeyLifetimes {
  uint64_t issued{}, released{}, order{};
  uint8_t velocity{};
  bool held{}, sustained{};
};
struct Chain {
  openWurli::DkPreamp preamp;
  openWurli::Tremolo tremolo;
  openWurli::Oversampler oversampler;
  openWurli::PowerAmp power;
  openWurli::Speaker speaker;
  double c9{};
};
}
struct Engine::Impl
{
  static constexpr std::size_t Quantum = 64;
  Settings settings, nativeSettings;
  double rate{}, osRate{};
  bool oversample{}, audible{};
  const char* settingsError{};
  const char* midiError{};
  const char* renderError{};
  std::array<Slot, 64> voices{};
  std::array<Channel, 16> channels{};
  std::array<std::array<KeyLifetimes, 128>, 16> keys{};
  Chain chain, warm;
  Smoother amplifier, tremolo, speaker;
  std::array<double, Quantum> voice{}, sum{}, out{};
  std::array<double, Quantum * 2> up{};
  uint64_t age{};
  std::size_t phase{};
  unsigned fadeLength{};

  Impl(double sampleRate, Settings s) : settings{s}, nativeSettings{s}, rate{sampleRate}
  {
    if(auto e = validate(s)) throw std::invalid_argument(e);
    // The unmodified cabinet's bypass LPF is 20 kHz: reject rates below 44.1 kHz.
    if(!std::isfinite(rate) || rate < 44100. || rate > 192000.)
      throw std::invalid_argument("OpenWurli supports sample rates from 44100 to 192000 Hz.");
    oversample = rate < 88200.; osRate = oversample ? rate * 2. : rate;
    fadeLength = std::max(1u, unsigned(rate * .005));
    chain.preamp.init(osRate); chain.tremolo.init(s.tremolo, osRate);
    chain.oversampler.init(); chain.power.init(osRate); chain.speaker.init(rate);
    warm = chain;
    amplifier.init(s.amplifier, rate); tremolo.init(s.tremolo, rate); speaker.init(s.speaker, rate);
    // All circuit settling, including the generated oscillator's startup, is off audio.
    for(std::size_t done = 0, total = std::size_t(rate * .6); done < total;) {
      const auto n = std::min(Quantum, total - done); process(nullptr, nullptr, n); done += n;
      if(renderError) throw std::runtime_error(renderError);
    }
    warm = chain; phase = 0;
  }
  void release(Slot& v) noexcept { v.state = State::Releasing; v.voice.noteOff(); }
  uint64_t latestOrder(uint8_t ch) const noexcept {
    if(settings.voiceAllocation == Allocation::Poly) return 0;
    uint64_t newest{};
    for(uint8_t c = 0; c < 16; ++c) if(sameLine(c, ch))
      for(const auto& key : keys[c])
        if(key.held || key.sustained) newest = std::max(newest, key.order);
    return newest;
  }
  void reconcile(uint8_t ch, uint64_t previous) noexcept {
    for(auto& v : voices) {
      if(v.state != State::Held && v.state != State::Sustained) continue;
      if(!sameLine(v.channel, ch)) continue;
      const auto& key = keys[v.channel][v.note];
      if(key.held || key.sustained) v.state = key.held ? State::Held : State::Sustained;
      else if(!fallback(ch, &v)) release(v);
      if(settings.voiceAllocation != Allocation::Poly) return;
    }
    // Naturally silent reeds retain musical priority without being retriggered by
    // unrelated releases. Restore a candidate only when the selected note changes.
    if(previous != latestOrder(ch)) fallback(ch);
  }
  void pedal(uint8_t ch, bool down) noexcept {
    channels[ch].sustain = down;
    if(down) return;
    const auto previous = latestOrder(ch);
    for(auto& key : keys[ch]) key.sustained = false;
    reconcile(ch, previous);
  }
  bool sameLine(uint8_t owner, uint8_t ch) const noexcept {
    return settings.voiceAllocation == Allocation::Mono || owner == ch;
  }
  Slot* allocate() noexcept {
    auto* slot = &voices[0];
    for(auto& v : voices) {
      if(v.state == State::Free && v.fade == 0) return &v;
      if(v.state < slot->state || (v.state == slot->state && v.age < slot->age)) slot = &v;
    }
    return slot;
  }
  void strike(Slot& slot, uint8_t ch, uint8_t note) noexcept {
    const auto& key = keys[ch][note];
    if(slot.state != State::Free) {
      slot.stolen = slot.voice; slot.stolenChannel = slot.channel; slot.fade = fadeLength;
    }
    double vel = double(key.velocity) / 127.;
    switch(settings.velocityCurve) { case 1: vel *= vel; break; case 2: vel = std::sqrt(vel); break; case 3: vel = .75; break; default: break; }
    // The physical keyboard is A1..C7; the source clamps pitches, not MIDI identity.
    slot.voice.noteOn(note, vel, rate, uint32_t(note) * 2654435761u + uint32_t(++age), settings.correction);
    slot.state = key.held ? State::Held : State::Sustained;
    slot.note = note; slot.channel = ch; slot.age = age; slot.noteId = key.issued;
    audible = true;
  }
  bool fallback(uint8_t ch, Slot* slot = nullptr) noexcept {
    if(settings.voiceAllocation == Allocation::Poly) return false;
    uint64_t newest{};
    uint8_t owner{}, note{};
    for(uint8_t c = 0; c < 16; ++c) if(sameLine(c, ch))
      for(int n = 0; n < 128; ++n) {
        const auto& key = keys[c][n];
        if((key.held || key.sustained) && key.order > newest) { newest = key.order; owner = c; note = uint8_t(n); }
      }
    if(!newest) return false;
    strike(slot ? *slot : *allocate(), owner, note);
    return true;
  }
  void noteOff(uint8_t ch, uint8_t note) noexcept {
    auto& key = keys[ch][note];
    if(key.issued == key.released) return;
    const auto id = ++key.released;
    if(id != key.issued || !key.held) return;
    const auto previous = latestOrder(ch);
    key.held = false; key.sustained = channels[ch].sustain;
    reconcile(ch, previous);
  }
  void noteOn(uint8_t ch, uint8_t note, uint8_t velocity) noexcept {
    auto& key = keys[ch][note];
    if(key.issued == key.released) key = {};
    if(key.issued == std::numeric_limits<uint64_t>::max()) {
      midiError = "Wurlitzer pending note lifetime counter exhausted; voices reset.";
      stop(); return;
    }
    ++key.issued; key.held = true; key.sustained = false;
    key.velocity = velocity; key.order = ++age;
    // Repeated keys retrigger one reed while older Note Offs retain FIFO debt.
    // Monophonic modes replace the current line even when its pedal is held.
    for(auto& v : voices) {
      if(v.state != State::Held && v.state != State::Sustained) continue;
      const bool reuse = settings.voiceAllocation == Allocation::Poly
          ? v.channel == ch && v.note == note : sameLine(v.channel, ch);
      if(reuse) { strike(v, ch, note); return; }
    }
    strike(*allocate(), ch, note);
  }
  void clearHeld(int ch = -1) noexcept {
    for(int c = 0; c < 16; ++c) if(ch < 0 || c == ch)
      for(auto& key : keys[c]) { key.held = false; key.sustained = false; }
  }
  void stopVoices() noexcept {
    for(auto& v : voices) { v.state = State::Free; v.fade = 0; v.voice.setInactive(); }
    clearHeld();
    resetChain();
  }
  void process(double* left, double* right, std::size_t n) noexcept {
    std::fill_n(sum.data(), n, 0.);
    for(auto& v : voices) {
      if(v.state != State::Free) {
        v.voice.render(voice.data(), n);
        const auto& ch = channels[v.channel];
        const double gain = ch.volume * ch.expression;
        for(std::size_t i = 0; i < n; ++i) sum[i] += voice[i] * gain;
      }
      if(v.fade) {
        const auto count = std::min<std::size_t>(n, v.fade);
        v.stolen.render(voice.data(), count);
        const auto& ch = channels[v.stolenChannel];
        const double gain = ch.volume * ch.expression;
        for(std::size_t i = 0; i < count; ++i) sum[i] += voice[i] * gain * double(v.fade - i) / fadeLength;
        v.fade -= unsigned(count);
      }
    }
    if(oversample) chain.oversampler.upsample2x(sum.data(), up.data(), n);
    for(std::size_t i = 0; i < n; ++i) {
      chain.tremolo.setDepth(tremolo.next());
      const double pot = amplifier.next();
      const double r11 = openWurli::R11_REED_BAR_VOLUME_DEFAULT;
      const double gain = openWurli::DkPreamp::openCircuitOutputFactor() * openWurli::volumePotGain(pot, r11);
      const double fc = openWurli::volumePotPoleHz(pot, r11);
      const double alpha = (!std::isfinite(fc) || fc >= osRate * .5) ? 1. : 1. - std::exp(-2. * M_PI * fc / osRate);
      for(unsigned j = 0; j < (oversample ? 2u : 1u); ++j) {
        const auto index = oversample ? i * 2 + j : i;
        chain.preamp.setLdrResistance(chain.tremolo.process());
        const double pre = chain.preamp.processSample(oversample ? up[index] : sum[index]);
        chain.c9 += alpha * (pre * gain - chain.c9);
        if(oversample) up[index] = chain.power.process(chain.c9);
        else out[index] = chain.power.process(chain.c9);
      }
    }
    if(oversample) chain.oversampler.downsample2x(up.data(), out.data(), n);
    for(std::size_t i = 0; i < n; ++i) {
      chain.speaker.setCharacter(speaker.next());
      const double sample = chain.speaker.process(out[i]) * openWurli::POST_SPEAKER_GAIN * 5.011872336272722;
      if(!std::isfinite(sample)) {
        renderError = "OpenWurli circuit produced non-finite audio; voices reset.";
        stop();
        if(left) { std::fill_n(left + i, n - i, 0.); std::fill_n(right + i, n - i, 0.); }
        return;
      }
      // Never suspend the oscillator, voices or filters when Volume is exactly zero.
      if(left) left[i] = right[i] = audible && settings.volume != 0.f ? sample * settings.volume : 0.;
    }
  }
  void resetChain() noexcept {
    // Oversampler vectors already have identical sizes/capacities: assignment does not allocate.
    chain = warm; audible = false; phase = 0;
    amplifier.init(settings.amplifier, rate); tremolo.init(settings.tremolo, rate); speaker.init(settings.speaker, rate);
  }
  void stop() noexcept {
    stopVoices();
    for(auto& ch : channels) ch.sustain = false;
    keys = {};
  }
};
Engine::Engine(double rate, Settings s) : m{std::make_unique<Impl>(rate, s)} { }
Engine::~Engine() = default;
bool Engine::configure(Settings s) noexcept {
  if(auto e = validate(s)) { m->settingsError = e; return false; }
  // Native edits change only fields that actually changed; unrelated gain automation
  // must not undo a MIDI timbre controller.
  if(s.volume != m->nativeSettings.volume) m->settings.volume = s.volume;
  if(s.amplifier != m->nativeSettings.amplifier) m->settings.amplifier = s.amplifier;
  if(s.tremolo != m->nativeSettings.tremolo) m->settings.tremolo = s.tremolo;
  if(s.speaker != m->nativeSettings.speaker) m->settings.speaker = s.speaker;
  if(s.correction != m->nativeSettings.correction) m->settings.correction = s.correction;
  if(s.velocityCurve != m->nativeSettings.velocityCurve) m->settings.velocityCurve = s.velocityCurve;
  if(s.voiceAllocation != m->nativeSettings.voiceAllocation) {
    m->stopVoices(); // Preserve channel controllers and pending FIFO Note Off debt.
    m->settings.voiceAllocation = s.voiceAllocation;
  }
  m->nativeSettings = s; m->settingsError = nullptr;
  m->amplifier.set(m->settings.amplifier); m->tremolo.set(m->settings.tremolo);
  m->speaker.set(m->settings.speaker); return true;
}
void Engine::panic() noexcept {
  m->settings = m->nativeSettings; m->channels = {}; m->stop();
}
void Engine::reset() noexcept { panic(); m->age = 0; m->midiError = m->renderError = nullptr; }
const char* Engine::error() const noexcept
{ return m->settingsError ? m->settingsError : m->renderError ? m->renderError : m->midiError; }
double Engine::sampleRate() const noexcept { return m->rate; }
std::size_t Engine::activeVoices() const noexcept { return std::count_if(m->voices.begin(), m->voices.end(), [](auto& v) { return v.state != State::Free; }); }
std::size_t Engine::heldVoices() const noexcept { return std::count_if(m->voices.begin(), m->voices.end(), [](auto& v) { return v.state == State::Held; }); }
std::size_t Engine::sustainedVoices() const noexcept { return std::count_if(m->voices.begin(), m->voices.end(), [](auto& v) { return v.state == State::Sustained; }); }
void Engine::render(double* left, double* right, std::size_t frames) noexcept {
  // A later successful internal quantum must not conceal a failure from this
  // render call. A subsequent render attempt can report recovery.
  if(frames) m->renderError = nullptr;
  while(frames) {
    const auto n = std::min(frames, Impl::Quantum - m->phase);
    m->process(left, right, n); m->phase += n;
    if(m->phase == Impl::Quantum) {
      m->phase = 0;
      for(auto& v : m->voices) if(v.state != State::Free && v.voice.isSilent()) { v.state = State::Free; v.voice.setInactive(); }
    }
    left += n; right += n; frames -= n;
  }
}
void Engine::midi(std::span<const uint8_t> bytes) noexcept {
  if(bytes.empty()) return;
  const auto status = bytes[0];
  if(status == 0xff) { reset(); return; }
  if(status >= 0xf0 || status < 0x80) return;
  const auto kind = status & 0xf0; const uint8_t ch = status & 15;
  const std::size_t length = kind == 0xc0 || kind == 0xd0 ? 2 : 3;
  if(bytes.size() != length || bytes[1] > 127 || (length == 3 && bytes[2] > 127)) { m->midiError = "Invalid Wurlitzer MIDI message."; return; }
  m->midiError = nullptr;
  if(kind == 0x90 && bytes[2]) { m->noteOn(ch, bytes[1], bytes[2]); return; }
  if(kind == 0x80 || kind == 0x90) { m->noteOff(ch, bytes[1]); return; }
  // Acoustic reeds have no bend/aftertouch or program bank. These messages are intentionally ignored.
  if(kind != 0xb0) return;
  const int cc = bytes[1], value = bytes[2];
  switch(cc) {
    case 1: m->settings.tremolo = value / 127.f; m->tremolo.set(m->settings.tremolo); break;
    case 7: m->channels[ch].volume = value / 127.; break;
    case 11: m->channels[ch].expression = value / 127.; break;
    case 64: m->pedal(ch, value >= 64); break;
    case 70: m->settings.correction = value == 1 || value >= 64; break;
    case 71: m->settings.speaker = value / 127.f; m->speaker.set(m->settings.speaker); break;
    case 75: m->settings.velocityCurve = value < 4 ? value : value / 32; break;
    case 120: {
      const auto previous = m->latestOrder(ch);
      m->clearHeld(ch);
      for(auto& v : m->voices) {
        if(v.channel == ch) { v.state = State::Free; v.voice.setInactive(); }
        if(v.stolenChannel == ch) v.fade = 0;
      }
      m->reconcile(ch, previous);
      if(!activeVoices() && std::none_of(m->voices.begin(), m->voices.end(), [](auto& v) { return v.fade != 0; })) m->resetChain();
      }
      break;
    case 121:
      m->channels[ch] = {};
      m->pedal(ch, false);
      // Timbre controllers address the one shared instrument, not a channel.
      // Restore its authored sound without resetting reeds, circuit tails or LFO phase.
      m->settings = m->nativeSettings;
      m->amplifier.set(m->settings.amplifier);
      m->tremolo.set(m->settings.tremolo);
      m->speaker.set(m->settings.speaker);
      break;
    case 123: {
      const auto previous = m->latestOrder(ch);
      for(auto& key : m->keys[ch]) {
        key.sustained |= key.held && m->channels[ch].sustain;
        key.held = false;
      }
      m->reconcile(ch, previous);
      break;
    }
    default: break;
  }
}
}
