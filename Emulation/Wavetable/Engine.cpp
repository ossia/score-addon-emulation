// SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine.hpp"
#include <Emulation/Volume.hpp>
#include <Emulation/HeldNotes.hpp>
#include <emu2212.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
extern "C" {
void* score_huc_new(unsigned, unsigned);
void score_huc_delete(void*);
void score_huc_write(void*, unsigned, unsigned);
void score_huc_sample(void*, int*, int*);
int score_huc_wave(void*, int, int);
}
namespace Emulation::Wavetable {
Channel::Channel() noexcept {
  for(int i = 0; i < 32; ++i)
    wave[i] = i < 16 ? i / 16. : (32 - i) / 16.;
}
const char* modelTag(Model m) noexcept {
  switch(m) { case Model::Scc: return "SCC"; case Model::SccPlus: return "SCC+"; case Model::HuC6280: return "HuC6280"; }
  return "unsupported";
}
const char* validate(const Settings& s) noexcept {
  if(s.model < Model::Scc || s.model > Model::HuC6280) return "Unsupported wavetable model.";
  if(s.clock < 100000 || s.clock > 8000000) return "Clock must be 100000 to 8000000 Hz.";
  if(s.assignment < Assignment::Polyphonic || s.assignment > Assignment::Mono) return "Invalid voice allocation.";
  if(!std::isfinite(s.volume) || s.volume < 0. || s.volume > MaxVolume) return "Volume must be finite and within 0 to 32.";
  if(s.tuning < -2400 || s.tuning > 2400 || s.left < 0 || s.left > 15 || s.right < 0 || s.right > 15
      || s.lfoDepth < 0 || s.lfoDepth > 3 || s.lfoFrequency < 0 || s.lfoFrequency > 255
      || s.lfoPeriod < 0 || s.lfoPeriod > 4095) return "Invalid global tuning, balance or LFO field.";
  for(const auto& c : s.channels) {
    if(!std::isfinite(c.level) || c.level < 0. || c.level > 1.
        || c.tuning < -2400 || c.tuning > 2400 || c.left < 0 || c.left > 15 || c.right < 0 || c.right > 15
        || c.noisePeriod < 0 || c.noisePeriod > 31) return "Invalid channel field.";
    for(double x : c.wave)
      if(!std::isfinite(x) || x < 0. || x > 1.) return "Wave samples must be normalized to 0 to 1.";
  }
  return nullptr;
}
bool setScalarControl(Settings& s, int c, double x) noexcept {
  if(c < 0 || c >= ControlCount || isWave(c) || !std::isfinite(x)) return false;
  if(c == Volume) {
    if(x < 0. || x > MaxVolume) return false;
    s.volume = x; return true;
  }
  if(c >= ChannelBase && (c - ChannelBase) % ChannelStride == Level) {
    if(x < 0. || x > 1.) return false;
    s.channels[(c - ChannelBase) / ChannelStride].level = x; return true;
  }
  int low{}, high{};
  switch(c) {
    case ChipModel: high = 2; break;
    case Clock: low = 100000; high = 8000000; break;
    case VoiceAssignment: high = 2; break;
    case Tuning: low = -2400; high = 2400; break;
    case GlobalLeft: case GlobalRight: high = 15; break;
    case LfoDepth: high = 3; break;
    case LfoFrequency: high = 255; break;
    case LfoPeriod: high = 4095; break;
    default: switch((c - ChannelBase) % ChannelStride) {
      case Enabled: case Noise: high = 1; break;
      case Left: case Right: high = 15; break;
      case NoisePeriod: high = 31; break;
      case ChannelTuning: low = -2400; high = 2400; break;
      default: return false;
    } break;
  }
  if(x < low || x > high) return false;
  const int n = int(std::lround(x));
  switch(c) {
    case ChipModel: s.model = Model(n); return true;
    case Clock: s.clock = n; return true;
    case VoiceAssignment: s.assignment = Assignment(n); return true;
    case Tuning: s.tuning = n; return true;
    case GlobalLeft: s.left = n; return true;
    case GlobalRight: s.right = n; return true;
    case LfoDepth: s.lfoDepth = n; return true;
    case LfoFrequency: s.lfoFrequency = n; return true;
    case LfoPeriod: s.lfoPeriod = n; return true;
  }
  auto& ch = s.channels[(c - ChannelBase) / ChannelStride];
  switch((c - ChannelBase) % ChannelStride) {
    case Enabled: ch.enabled = n; break;
    case ChannelTuning: ch.tuning = n; break;
    case Left: ch.left = n; break;
    case Right: ch.right = n; break;
    case Noise: ch.noise = n; break;
    case NoisePeriod: ch.noisePeriod = n; break;
  }
  return true;
}
struct Engine::Impl {
  struct Voice { int note{-1}, channel{}, velocity{}; uint64_t age{}; bool held{}, sustained{}; };
  Settings settings;
  SCC* scc{};
  void* huc{};
  double rate{}, sourceRate{}, remaining{}, heldLeft{}, heldRight{};
  std::array<Voice, 6> voices{};
  HeldNotes notes;
  std::array<int, 16> bend{}, ccVolume{};
  std::array<bool, 16> pedal{};
  const char* lastError{};
  std::array<int, 6> pitchKeys{}, periods{};
  std::array<int, 256> sccRegisters{};
  std::array<std::array<int, 8>, 6> hucRegisters{};
  std::array<int, 10> hucGlobals{};
  int selectedChannel{-1};
  int count() const noexcept { return huc ? 6 : 5; }
  bool usable(int i) const noexcept { return i < count() && settings.channels[i].enabled && !(huc && settings.lfoDepth && i == 1); }
  Impl(double r, Settings s) : settings{s}, rate{r} {
    if(auto e = validate(s)) throw std::runtime_error(e);
    if(!std::isfinite(r) || r < 8000 || r > 384000 || std::floor(r) != r) throw std::runtime_error("Integer sample rate 8000 to 384000 Hz required.");
    ccVolume.fill(127);
    pitchKeys.fill(std::numeric_limits<int>::min());
    sccRegisters.fill(-1); hucGlobals.fill(-1);
    for(auto& registers : hucRegisters) registers.fill(-1);
    if(s.model == Model::HuC6280) {
      // Native clock/16 oversampling keeps noise/LFO advancement bounded and correct at low host rates.
      sourceRate = std::max(1, s.clock / 16);
      huc = score_huc_new(s.clock, unsigned(sourceRate));
      if(!huc) throw std::bad_alloc{};
    } else {
      scc = SCC_new(s.clock, unsigned(rate)); if(!scc) throw std::bad_alloc{};
      SCC_reset(scc); SCC_set_type(scc, s.model == Model::SccPlus ? SCC_ENHANCED : SCC_STANDARD);
      SCC_writeReg(scc, 0xe0, s.model == Model::SccPlus); SCC_set_quality(scc, 1);
    }
    writeWaves(nullptr); registers();
  }
  ~Impl() { if(scc) SCC_delete(scc); if(huc) score_huc_delete(huc); }
  void hw(int reg, int val) noexcept {
    score_huc_write(huc, reg, val);
    if(reg == 0) selectedChannel = val;
    else if(reg >= 2 && reg <= 7 && reg != 6) hucRegisters[selectedChannel][reg] = val;
    else if(reg != 6) hucGlobals[reg] = val;
  }
  void hwGlobal(int reg, int val) noexcept { if(hucGlobals[reg] != val) hw(reg, val); }
  void hwChannel(int ch, int reg, int val) noexcept {
    if(hucRegisters[ch][reg] == val) return;
    if(selectedChannel != ch) hw(0, ch);
    hw(reg, val);
  }
  void sw(int reg, int val) noexcept {
    if(sccRegisters[reg] == val) return;
    SCC_writeReg(scc, reg, val); sccRegisters[reg] = val;
  }
  void writeWaves(const Settings* previous) noexcept {
    for(int ch = 0; ch < count(); ++ch) {
      const auto& c = settings.channels[ch];
      if(huc) {
        if(previous && c.wave == previous->channels[ch].wave) continue;
        hw(0, ch); hw(4, 0x40); hw(4, 0);
        for(double sample : c.wave) hw(6, int(std::lround(31. * sample)));
      } else {
        if(settings.model == Model::Scc && ch == 4) continue;
        if(previous && c.wave == previous->channels[ch].wave) continue;
        for(int i = 0; i < 32; ++i)
          SCC_writeReg(scc, 32 * ch + i, uint8_t(std::lround(-128. + 255. * c.wave[i])));
      }
    }
  }
  void registers() noexcept {
    if(huc) { hwGlobal(1, settings.left * 16 + settings.right); hwGlobal(8, settings.lfoFrequency); hwGlobal(9, settings.lfoDepth); }
    int mask{};
    for(int i = 0; i < count(); ++i) {
      const auto& c = settings.channels[i]; const auto& v = voices[i];
      const bool active = usable(i) && v.note >= 0;
      // Integer key captures all pitch inputs exactly; gain and balance never run exp2.
      const int key = ((v.note < 0 ? 69 : v.note) - 69) * 204800
          + (settings.tuning + c.tuning) * 2048 + bend[v.channel] * 50;
      if(pitchKeys[i] != key) {
        pitchKeys[i] = key;
        const double hz = 440. * std::exp2(key / (204800. * 12.));
        periods[i] = std::clamp(int(std::lround(settings.clock / (32. * hz))) - (huc ? 0 : 1), huc ? 1 : 9, 4095);
      }
      const int period = periods[i];
      const int level = active ? int(std::lround((huc ? 31. : 15.) * c.level * v.velocity / 127. * ccVolume[v.channel] / 127.)) : 0;
      if(huc) {
        const int p = settings.lfoDepth && i == 1 ? settings.lfoPeriod : period;
        hwChannel(i, 2, p & 255); hwChannel(i, 3, p >> 8); hwChannel(i, 5, c.left * 16 + c.right);
        hwChannel(i, 7, (i >= 4 && c.noise ? 128 : 0) | c.noisePeriod);
        hwChannel(i, 4, (active ? 128 : 0) | level);
      } else {
        sw(0xc0 + i * 2, period & 255); sw(0xc1 + i * 2, period >> 8);
        sw(0xd0 + i, level); if(active) mask |= 1 << i;
      }
    }
    if(scc) sw(0xe1, mask);
  }
  void stop(Voice& v) noexcept { v = {}; }
  void start(int slot, HeldNotes::Selection selected) noexcept {
    const auto& key = notes.keys[selected.channel][selected.note];
    voices[slot] = {selected.note, selected.channel, key.velocity, key.order, key.held, !key.held};
  }
  void choose(int channel) noexcept {
    if(channel >= 0 && !usable(channel)) return;
    const auto selected = notes.latest(channel);
    const int first = channel < 0 ? 0 : channel;
    const int end = channel < 0 ? count() : channel + 1;
    for(int slot = first; slot < end; ++slot) if(usable(slot)) {
      auto& voice = voices[slot];
      if(!selected.order) stop(voice);
      else if(voice.age != selected.order || voice.note < 0) start(slot, selected);
      else {
        voice.held = notes.keys[selected.channel][selected.note].held;
        voice.sustained = !voice.held;
      }
    }
  }
  void chooseChannel(int ch) noexcept {
    if(settings.assignment == Assignment::Mono) choose(-1);
    else if(settings.assignment == Assignment::Fixed) choose(ch);
  }
  void resetChannel(int ch, bool immediate) noexcept {
    notes.stopChannel(ch, !immediate && pedal[ch]);
    for(auto& v : voices) if(v.note >= 0 && v.channel == ch) {
      if(immediate) stop(v);
      else if(v.held) { v.held = false; if(pedal[ch]) v.sustained = true; else stop(v); }
    }
    chooseChannel(ch);
  }
  void releasePedal(int ch) noexcept {
    pedal[ch] = false;
    notes.releasePedal(ch);
    for(auto& v : voices) if(v.channel == ch && v.sustained) stop(v);
    chooseChannel(ch);
  }
  void noteOn(int ch, int note, int velocity) noexcept {
    const bool accepted = settings.assignment != Assignment::Fixed || usable(ch);
    if(!notes.press(ch, note, velocity, accepted)) { lastError = "Note lifetime counter exhausted; note rejected."; return; }
    if(!accepted) return;
    if(settings.assignment != Assignment::Polyphonic) { chooseChannel(ch); return; }
    int slot = -1;
    for(int i = 0; i < count(); ++i) if(usable(i) && voices[i].note == note && voices[i].channel == ch) { slot = i; break; }
    if(slot < 0) for(int i = 0; i < count(); ++i) if(usable(i)) {
      if(voices[i].note < 0) { slot = i; break; }
      if(slot < 0 || voices[i].age < voices[slot].age) slot = i;
    }
    if(slot >= 0) start(slot, {ch, note, notes.keys[ch][note].order});
  }
  void noteOff(int ch, int note) noexcept {
    if(!notes.release(ch, note, pedal[ch])) return;
    if(settings.assignment != Assignment::Polyphonic) { chooseChannel(ch); return; }
    for(auto& v : voices) if(v.note == note && v.channel == ch && v.held) {
      v.held = false; if(pedal[ch]) v.sustained = true; else stop(v);
    }
  }
  void stopNotes() noexcept {
    voices = {}; notes.stop(); registers();
  }
  void panic() noexcept {
    notes = {}; pedal = {}; bend = {}; ccVolume.fill(127); stopNotes();
  }
};
Engine::Engine(double rate, Settings s) : m{std::make_unique<Impl>(rate, s)} {}
Engine::~Engine() = default;
bool Engine::configure(const Settings& s) noexcept {
  if(s == m->settings) { m->lastError = nullptr; return true; }
  if(auto e = validate(s)) { m->lastError = e; return false; }
  if(s.model != m->settings.model || s.clock != m->settings.clock) { m->lastError = "Model and clock require off-thread engine preparation."; return false; }
  const auto previous = m->settings; m->settings = s;
  if(s.assignment != previous.assignment) m->stopNotes();
  for(int i = 0; i < 6; ++i) if(!m->usable(i)) {
    m->voices[i] = {};
    if(s.assignment == Assignment::Fixed) m->notes.stopChannel(i, false);
  }
  m->writeWaves(&previous); m->registers(); m->lastError = nullptr; return true;
}
bool Engine::setScalarControl(int c, double x) noexcept {
  if(c == ChipModel || c == Clock) {
    m->lastError = "Model and clock require off-thread engine preparation."; return false;
  }
  const double previous = controlValue(m->settings, c);
  if(!Wavetable::setScalarControl(m->settings, c, x)) {
    m->lastError = "Invalid wavetable scalar control."; return false;
  }
  m->lastError = nullptr;
  if(previous == controlValue(m->settings, c) || c == Volume) return true;
  if(c == VoiceAssignment) m->stopNotes();
  else {
    for(int i = 0; i < 6; ++i) if(!m->usable(i)) {
      m->voices[i] = {};
      if(m->settings.assignment == Assignment::Fixed) m->notes.stopChannel(i, false);
    }
    m->registers();
  }
  return true;
}
void Engine::midi(std::span<const uint8_t> bytes) noexcept {
  if(bytes.empty()) return;
  const int status = bytes[0], ch = status & 15;
  if(status == 0xff) { panic(); return; }
  if(status >= 0xf8) { m->lastError = nullptr; return; }
  if(status < 0x80 || status >= 0xf0) { m->lastError = "Only channel MIDI is supported."; return; }
  const int kind = status & 0xf0; const std::size_t length = kind == 0xc0 || kind == 0xd0 ? 2 : 3;
  if(bytes.size() != length || bytes[1] > 127 || (length == 3 && bytes[2] > 127)) { m->lastError = "Malformed MIDI message."; return; }
  m->lastError = nullptr;
  switch(kind) {
    case 0x80: m->noteOff(ch, bytes[1]); break;
    case 0x90: if(bytes[2]) m->noteOn(ch, bytes[1], bytes[2]); else m->noteOff(ch, bytes[1]); break;
    case 0xe0: m->bend[ch] = (bytes[1] | bytes[2] << 7) - 8192; break;
    case 0xb0: switch(bytes[1]) {
      case 7: m->ccVolume[ch] = bytes[2]; break;
      case 64: if(bytes[2] >= 64) m->pedal[ch] = true; else m->releasePedal(ch); break;
      case 120: m->resetChannel(ch, true); break;
      case 121: m->bend[ch] = 0; m->ccVolume[ch] = 127; m->releasePedal(ch); break;
      case 123: m->resetChannel(ch, false); break;
      default: if(bytes[1] >= 124) m->resetChannel(ch, false); break;
    } break;
    default: break;
  }
  m->registers();
}
void Engine::panic() noexcept { m->lastError = nullptr; m->panic(); }
void Engine::render(double* left, double* right, std::size_t frames) noexcept {
  for(std::size_t i = 0; i < frames; ++i) {
    double l{}, r{};
    if(m->scc) l = r = SCC_calc(m->scc) / 9600.;
    else {
      const double duration = m->sourceRate / m->rate; double todo = duration;
      while(todo > 0.) {
        if(m->remaining <= 0.) { int a{}, b{}; score_huc_sample(m->huc, &a, &b); m->heldLeft = a / 32768.; m->heldRight = b / 32768.; m->remaining = 1.; }
        const double step = std::min(todo, m->remaining); l += step * m->heldLeft; r += step * m->heldRight; todo -= step; m->remaining -= step;
      }
      l /= duration; r /= duration;
    }
    left[i] = l * m->settings.volume; right[i] = r * m->settings.volume;
  }
}
const char* Engine::error() const noexcept { return m->lastError; }
int Engine::activeVoices() const noexcept { return int(std::count_if(m->voices.begin(), m->voices.end(), [](auto& v) { return v.note >= 0; })); }
int Engine::voiceNote(int slot) const noexcept { return slot >= 0 && slot < m->count() ? m->voices[slot].note : -1; }
int Engine::nativeWave(int ch, int sample) const noexcept {
  if(ch < 0 || ch >= m->count() || sample < 0 || sample >= 32) return -999;
  return m->scc ? m->scc->wave[ch][sample] : score_huc_wave(m->huc, ch, sample);
}
}
