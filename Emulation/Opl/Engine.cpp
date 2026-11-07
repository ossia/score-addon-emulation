#include "Engine.hpp"

#include <wopl_file.h>
// The dependency is pinned. Public adl_setInstrument does not invalidate its
// pointer-based patch cache; see setInstrument below. These private headers must
// be compiled with the same feature definitions as the linked library.
#include <adlmidi_midiplay.hpp>
#include <adlmidi_opl3.hpp>
#include <chips/opl_chip_base.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>

namespace Emulation::Opl
{
namespace
{
constexpr std::size_t maxBankBytes = 64 * 1024 * 1024;

// Count the pinned ymfm FIFO exactly at the native-clock boundary, while
// retaining its register pacing and the dependency's existing resampler.
class GuardedChip final : public OPLChipBaseT<GuardedChip>
{
public:
  GuardedChip(const AdlMIDI_SPtr<OPLChipBase>& chip, uint32_t rate, bool& overflow)
      : m_chip{chip}, m_overflow{overflow}
  {
    // Preparation writes must finish before the first live note. The private
    // queue has no size accessor; its fixed capacity bounds this setup drain.
    int16_t frame[2];
    for(unsigned i = 0; i < queueCapacity; ++i) m_chip->nativeGenerate(frame);
    OPLChipBaseT::setRate(rate);
    setChipId(chip->chipId());
  }
  bool canRunAtPcmRate() const override { return false; }
  void reset() override
  {
    OPLChipBaseT::reset();
    m_chip->reset();
  }
  void writeReg(uint16_t address, uint8_t data) override
  {
    if(m_overflow) return;
    if(m_pending == queueCapacity) { m_overflow = true; return; }
    m_chip->writeReg(address, data);
    ++m_pending;
  }
  void writePan(uint16_t address, uint8_t data) override { m_chip->writePan(address, data); }
  void nativePreGenerate() override {}
  void nativePostGenerate() override {}
  void nativeGenerate(int16_t* frame) override
  {
    m_chip->nativeGenerate(frame);
    if(m_pending) --m_pending;
  }
  const char* emulatorName() override { return m_chip->emulatorName(); }
  ChipType chipType() override { return m_chip->chipType(); }
  bool hasFullPanning() override { return m_chip->hasFullPanning(); }
private:
  static constexpr unsigned queueCapacity = 2048;
  AdlMIDI_SPtr<OPLChipBase> m_chip;
  bool& m_overflow;
  unsigned m_pending{};
};
using WoplPtr = std::unique_ptr<WOPLFile, decltype(&WOPL_Free)>;

unsigned bankKey(bool percussion, std::uint8_t msb, std::uint8_t lsb)
{
  return (unsigned(percussion) << 16) | (unsigned(msb) << 8) | lsb;
}

std::string nameFrom(const char* name)
{
  const auto end = std::find(name, name + 32, '\0');
  return {name, end};
}

void nameTo(const std::string& name, char* dest)
{
  if(name.size() > 32 || name.find('\0') != std::string::npos)
    throw std::runtime_error{"WOPL names must contain at most 32 bytes and no NUL characters"};
  std::memcpy(dest, name.data(), name.size());
}

template <typename To, typename From>
void copyInstrument(To& to, const From& from)
{
  to.note_offset1 = from.note_offset1;
  to.note_offset2 = from.note_offset2;
  to.midi_velocity_offset = from.midi_velocity_offset;
  to.second_voice_detune = from.second_voice_detune;
  to.percussion_key_number = from.percussion_key_number;
  to.inst_flags = from.inst_flags;
  to.fb_conn1_C0 = from.fb_conn1_C0;
  to.fb_conn2_C0 = from.fb_conn2_C0;
  to.delay_on_ms = from.delay_on_ms;
  to.delay_off_ms = from.delay_off_ms;
  for(int op = 0; op < 4; ++op)
  {
    to.operators[op].avekf_20 = from.operators[op].avekf_20;
    to.operators[op].ksl_l_40 = from.operators[op].ksl_l_40;
    to.operators[op].atdec_60 = from.operators[op].atdec_60;
    to.operators[op].susrel_80 = from.operators[op].susrel_80;
    to.operators[op].waveform_E0 = from.operators[op].waveform_E0;
  }
}


ADL_Instrument voice(std::uint8_t multiplier, std::uint8_t modulation,
                     std::uint8_t feedback, std::uint8_t attackDecay,
                     std::uint8_t sustainRelease)
{
  // Original authored patches, not extracted from any upstream or vendor bank.
  // WOPL operator ordering is carrier, modulator, carrier, modulator.
  ADL_Instrument ins{};
  ins.version = ADLMIDI_InstrumentVersion;
  ins.fb_conn1_C0 = feedback << 1;
  ins.operators[0] = {0x21, 0x08, attackDecay, sustainRelease, 0};
  ins.operators[1] = {std::uint8_t(0x20 | multiplier), modulation,
                      attackDecay, sustainRelease, 0};
  ins.delay_on_ms = 2000;
  ins.delay_off_ms = 400;
  return ins;
}
}
const char* instrumentError(const ADL_Instrument& ins, Address address,
                            const Settings& settings) noexcept
{
  if(ins.version != ADLMIDI_InstrumentVersion)
    return "Unsupported ADL instrument structure version";
  if(ins.inst_flags & ADLMIDI_Ins_IsBlank)
    return nullptr; // Preserve inactive native fields without reinterpreting them.
  if(ins.inst_flags & 0x80)
    return "Unsupported reserved instrument flag 0x80";
  const auto mode = ins.inst_flags & 3;
  if(mode == 2)
    return "Pseudo-four-operator instruments require native WOPL mode bits 0x03, not 0x02";
  if(mode == 1 && settings.model == Model::Opl2)
    return "OPL2 cannot play true four-operator instruments; select OPL3";
  if(mode == 1 && settings.fourOpChannels == 0)
    return "This bank requires at least one configured four-operator channel";
  if(ins.note_offset1 < -128 || ins.note_offset1 > 127
     || ins.note_offset2 < -128 || ins.note_offset2 > 127)
    return "libADLMIDI supports runtime note offsets only in [-128, 127]; native WOPL data is preserved";
  const auto rhythm = ins.inst_flags & ADLMIDI_Ins_RhythmModeMask;
  if(rhythm > ADLMIDI_RM_HiHat || (rhythm && !address.percussion))
    return "Rhythm mode requires a percussion bank and a supported drum type";
  if(rhythm && mode != 0)
    return "Rhythm-mode instruments cannot use four-operator voice modes";
  const int operators = mode == 0 ? 2 : 4;
  for(int op = 0; op < operators; ++op)
  {
    if(ins.operators[op].waveform_E0 > (settings.model == Model::Opl2 ? 3 : 7))
      return settings.model == Model::Opl2
          ? "OPL2 supports only waveforms 0 through 3; select OPL3 or edit the patch"
          : "OPL3 supports only waveforms 0 through 7";
  }
  return nullptr;
}
const char* settingsError(const Settings& settings) noexcept
{
  if(settings.model != Model::Opl2 && settings.model != Model::Opl3)
    return "Unknown OPL model";
  if(settings.chips < 1 || settings.chips > 100)
    return "OPL chip count must be in [1, 100]";
  if(settings.fourOpChannels < 0 || settings.fourOpChannels > settings.chips * 6
     || (settings.model == Model::Opl2 && settings.fourOpChannels != 0))
    return "Four-operator channel count must be zero for OPL2, or in [0, 6 * chips] for OPL3";
  if(settings.softPan)
    return "The ymfm backend does not support soft panning; OPL3 offers native left/centre/right panning";
  return nullptr;
}
const char* bankError(const Bank& bank, const Settings& settings) noexcept
{
  if(const auto* error = settingsError(settings)) return error;
  if(bank.flags & ~0x07)
    return "This bank uses unsupported reserved WOPL global flags";
  if(bank.volumeModel > WOPL_VM_HMI)
    return "This bank uses an unsupported WOPL volume model";
  for(const auto& program : bank.programs)
  {
    if(program.address.msb > 127 || (!program.address.percussion && program.address.lsb > 127))
      return "This WOPL bank address cannot be selected by standard MIDI bank messages";
    if(program.address.percussion && program.address.msb != 0)
      return "libADLMIDI percussion kits use MSB 0 and LSB as the MIDI drum-kit program (128--255 for XG SFX)";
    if(const auto* error = instrumentError(program.instrument, program.address, settings))
      return error;
  }
  return nullptr;
}

const Program* findProgram(const Bank& bank, Address address) noexcept
{
  const auto it = std::find_if(bank.programs.begin(), bank.programs.end(),
      [address](const Program& p) { return p.address == address; });
  return it == bank.programs.end() ? nullptr : &*it;
}

Program* findProgram(Bank& bank, Address address) noexcept
{
  const auto it = std::find_if(bank.programs.begin(), bank.programs.end(),
      [address](const Program& p) { return p.address == address; });
  return it == bank.programs.end() ? nullptr : &*it;
}

Bank factoryBank()
{
  Bank bank;
  bank.metadata = {{false, 0, 0, "score original voices"},
                   {true, 0, 0, "score original percussion"}};
  bank.programs.reserve(256);
  for(int percussion = 0; percussion < 2; ++percussion)
    for(int program = 0; program < 128; ++program)
    {
      Program p;
      p.address = {percussion != 0, 0, 0, std::uint8_t(program)};
      p.instrument.inst_flags = ADLMIDI_Ins_IsBlank;
      bank.programs.push_back(std::move(p));
    }

  auto put = [&](bool percussion, int index, const char* name, ADL_Instrument ins) {
    auto& p = bank.programs[std::size_t(percussion) * 128 + index];
    p.name = name;
    p.instrument = ins;
  };
  put(false, 0, "Copper keys", voice(2, 0x26, 2, 0xF5, 0x68));
  put(false, 1, "Glass tine", voice(5, 0x22, 1, 0xF4, 0x88));
  put(false, 2, "Hollow reed", voice(1, 0x18, 3, 0xD3, 0x47));
  put(false, 3, "Rounded bass", voice(1, 0x29, 1, 0xF5, 0x57));
  auto organ = voice(2, 0x15, 0, 0xF0, 0x07);
  organ.fb_conn1_C0 |= 1;
  put(false, 4, "Open fifth organ", organ);
  auto pad = voice(3, 0x2D, 1, 0x73, 0x56);
  pad.operators[0].avekf_20 |= 0xC0;
  pad.operators[1].avekf_20 |= 0xC0;
  pad.delay_off_ms = 1000;
  put(false, 5, "Slow brass cloud", pad);
  auto sine = voice(1, 0x3F, 0, 0xF0, 0x08);
  put(false, 6, "Plain sine", sine);
  auto bell = voice(7, 0x14, 1, 0xF6, 0xA7);
  bell.operators[0].avekf_20 &= ~0x20;
  bell.operators[1].avekf_20 &= ~0x20;
  put(false, 7, "Bright chime", bell);

  auto drum = [&](int note, const char* name, int tone, std::uint8_t multiplier,
                  std::uint8_t modulation, std::uint8_t feedback, std::uint8_t decay) {
    auto ins = voice(multiplier, modulation, feedback, std::uint8_t(0xF0 | decay), 0xF8);
    ins.operators[0].avekf_20 &= ~0x20;
    ins.operators[1].avekf_20 &= ~0x20;
    ins.percussion_key_number = std::uint8_t(tone);
    ins.delay_on_ms = 200;
    ins.delay_off_ms = 200;
    put(true, note, name, ins);
  };
  drum(36, "Low copper kick", 30, 1, 0x20, 2, 6);
  drum(38, "Metal snare", 55, 13, 0x08, 7, 8);
  drum(42, "Short wire hat", 91, 15, 0x03, 7, 12);
  drum(46, "Open wire hat", 91, 15, 0x03, 7, 7);
  drum(45, "Low hollow tom", 43, 2, 0x1B, 3, 6);
  drum(49, "Copper splash", 84, 11, 0x05, 7, 5);
  return bank;
}

Bank decodeBank(std::span<const std::uint8_t> bytes)
{
  if(bytes.size() < 19)
    throw std::runtime_error{"WOPL header is truncated (at least 19 bytes required)"};
  if(bytes.size() > maxBankBytes)
    throw std::runtime_error{"WOPL file exceeds the 64 MiB engine safety limit"};
  constexpr char magic[] = "WOPL3-BANK\0";
  if(std::memcmp(bytes.data(), magic, 11) != 0)
    throw std::runtime_error{"Not a WOPL bank: expected WOPL3-BANK signature"};
  const auto version = unsigned(bytes[11]) | (unsigned(bytes[12]) << 8);
  if(version < 1 || version > 3)
    throw std::runtime_error{"Unsupported WOPL version; versions 1, 2 and 3 are supported"};
  const unsigned melodic = (unsigned(bytes[13]) << 8) | bytes[14];
  const unsigned percussion = (unsigned(bytes[15]) << 8) | bytes[16];
  const std::size_t count = melodic + percussion;
  if(count == 0)
    throw std::runtime_error{"WOPL contains no banks"};
  const auto expected = 19 + count * ((version >= 2 ? 34 : 0) + 128 * (version >= 3 ? 66 : 62));
  if(bytes.size() != expected)
    throw std::runtime_error{"WOPL length does not match its bank counts/version (truncated data or unsupported trailing extension)"};
  if(version == 1 && (melodic > 1 || percussion > 1))
    throw std::runtime_error{"WOPL v1 with multiple same-kind banks has no unambiguous MIDI addresses; export as WOPL v3"};

  int error{};
  // Upstream's parser takes void* but only reads the supplied bytes.
  WoplPtr file{WOPL_LoadBankFromMem(const_cast<std::uint8_t*>(bytes.data()), bytes.size(), &error), WOPL_Free};
  if(!file)
    throw std::runtime_error{"WOPL decoder failed (error " + std::to_string(error) + ")"};
  Bank bank;
  bank.version = std::uint16_t(version);
  bank.flags = file->opl_flags;
  bank.volumeModel = file->volume_model;
  bank.programs.reserve(count * 128);
  bank.metadata.reserve(count);
  std::map<unsigned, bool> seen;
  for(int kind = 0; kind < 2; ++kind)
  {
    const auto* source = kind ? file->banks_percussive : file->banks_melodic;
    const auto banks = kind ? percussion : melodic;
    for(unsigned b = 0; b < banks; ++b)
    {
      const auto& entry = source[b];
      const auto key = bankKey(kind != 0, entry.bank_midi_msb, entry.bank_midi_lsb);
      if(!seen.emplace(key, true).second)
        throw std::runtime_error{"WOPL contains duplicate MIDI bank addresses"};
      bank.metadata.push_back({kind != 0, entry.bank_midi_msb, entry.bank_midi_lsb, nameFrom(entry.bank_name)});
      for(unsigned p = 0; p < 128; ++p)
      {
        Program program;
        program.address = {kind != 0, entry.bank_midi_msb, entry.bank_midi_lsb, std::uint8_t(p)};
        program.name = nameFrom(entry.ins[p].inst_name);
        copyInstrument(program.instrument, entry.ins[p]);
        bank.programs.push_back(std::move(program));
      }
    }
  }
  return bank;
}

std::vector<std::uint8_t> encodeBank(const Bank& bank)
{
  if(bank.version < 1 || bank.version > 3)
    throw std::runtime_error{"Cannot encode unsupported WOPL version"};
  struct Slot
  {
    const BankMetadata* metadata{};
    std::array<const Program*, 128> programs{};
  };
  std::map<unsigned, Slot> slots;
  for(const auto& metadata : bank.metadata)
  {
    auto& slot = slots[bankKey(metadata.percussion, metadata.msb, metadata.lsb)];
    if(slot.metadata)
      throw std::runtime_error{"Duplicate WOPL bank metadata address"};
    slot.metadata = &metadata;
  }
  auto version = bank.version;
  for(const auto& program : bank.programs)
  {
    if(program.address.program > 127)
      throw std::runtime_error{"WOPL program numbers must be in [0, 127]"};
    if(program.instrument.version != ADLMIDI_InstrumentVersion)
      throw std::runtime_error{"Cannot encode an unsupported ADL instrument structure version"};
    auto& p = slots[bankKey(program.address.percussion, program.address.msb, program.address.lsb)].programs[program.address.program];
    if(p)
      throw std::runtime_error{"Duplicate WOPL instrument address"};
    p = &program;
    if(program.instrument.delay_on_ms || program.instrument.delay_off_ms)
      version = 3;
  }
  if(slots.empty())
    throw std::runtime_error{"Cannot encode an empty WOPL bank"};
  std::size_t melodic{}, percussion{};
  for(const auto& [key, slot] : slots)
  {
    (key & 0x10000 ? percussion : melodic)++;
    if((key & 0xFFFF) || (slot.metadata && !slot.metadata->name.empty()))
      version = std::max<std::uint16_t>(version, 2);
  }
  if(melodic > 65535 || percussion > 65535
     || 19 + slots.size() * (34 + 128 * 66) > maxBankBytes)
    throw std::runtime_error{"WOPL bank exceeds the 64 MiB engine safety limit"};
  WoplPtr file{WOPL_Init(std::uint16_t(melodic), std::uint16_t(percussion)), WOPL_Free};
  if(!file || !file->banks_melodic || !file->banks_percussive)
    throw std::runtime_error{"Unable to allocate WOPL encoder storage"};
  // WOPL_Init creates a default bank for zero counts; retain the actual counts.
  file->banks_count_melodic = std::uint16_t(melodic);
  file->banks_count_percussion = std::uint16_t(percussion);
  file->opl_flags = bank.flags;
  file->volume_model = bank.volumeModel;
  std::size_t m{}, d{};
  for(const auto& [key, slot] : slots)
  {
    auto& dest = key & 0x10000 ? file->banks_percussive[d++] : file->banks_melodic[m++];
    dest.bank_midi_msb = std::uint8_t(key >> 8);
    dest.bank_midi_lsb = std::uint8_t(key);
    if(slot.metadata)
      nameTo(slot.metadata->name, dest.bank_name);
    for(std::size_t p = 0; p < 128; ++p)
    {
      if(const auto* program = slot.programs[p])
      {
        nameTo(program->name, dest.ins[p].inst_name);
        copyInstrument(dest.ins[p], program->instrument);
      }
      else
        dest.ins[p].inst_flags = ADLMIDI_Ins_IsBlank;
    }
  }
  std::vector<std::uint8_t> bytes(WOPL_CalculateBankFileSize(file.get(), version));
  if(WOPL_SaveBankToMem(file.get(), bytes.data(), bytes.size(), version, 0) != 0)
    throw std::runtime_error{"WOPL encoder could not serialize the bank"};
  return bytes;
}

struct Engine::Impl
{
  struct Slot { Address address; ADL_Bank bank; ADL_Instrument instrument; };
  std::unique_ptr<ADL_MIDIPlayer, decltype(&adl_close)> player{nullptr, adl_close};
  Settings settings;
  std::vector<Slot> slots;
  int fourOperatorVoices{};
  int opl3Waveforms{};
  void countRequirements(const ADL_Instrument& instrument, int delta) noexcept
  {
    if(instrument.inst_flags & ADLMIDI_Ins_IsBlank) return;
    const int mode = instrument.inst_flags & 3;
    if(mode == 1) fourOperatorVoices += delta;
    for(int op = 0; op < (mode == 0 ? 2 : 4); ++op)
      if(instrument.operators[op].waveform_E0 > 3) opl3Waveforms += delta;
  }
  bool rhythmMode{};
  bool registerOverflow{};
  const char* error{};
};

Engine::Engine(double sampleRate, const Settings& settings, const Bank& bank)
    : m_impl{std::make_unique<Impl>()}
{
  if(!std::isfinite(sampleRate) || sampleRate < 8000 || sampleRate > 384000
     || std::floor(sampleRate) != sampleRate)
    throw std::runtime_error{"OPL sample rate must be an integer in [8000, 384000] Hz"};
  if(const auto* error = bankError(bank, settings)) throw std::runtime_error{error};
  for(const auto& program : bank.programs)
  {
    m_impl->countRequirements(program.instrument, 1);
    if(!(program.instrument.inst_flags & ADLMIDI_Ins_IsBlank)
       && (program.instrument.inst_flags & ADLMIDI_Ins_RhythmModeMask))
      m_impl->rhythmMode = true;
  }
  auto bytes = encodeBank(bank);
  m_impl->settings = settings;
  m_impl->player.reset(adl_init(static_cast<long>(sampleRate)));
  if(!m_impl->player)
    throw std::runtime_error{"Unable to create libADLMIDI instance"};
  auto* player = m_impl->player.get();
  auto check = [&](int result, const char* operation) {
    if(result < 0)
      throw std::runtime_error{std::string{operation} + ": " + adl_errorInfo(player)};
  };
  check(adl_switchEmulator(player, settings.model == Model::Opl2 ? ADLMIDI_EMU_YMFM_OPL2 : ADLMIDI_EMU_YMFM_OPL3), "Selecting ymfm backend");
  check(adl_setNumChips(player, settings.chips), "Setting OPL chip count");
  check(adl_openBankData(player, bytes.data(), static_cast<unsigned long>(bytes.size())), "Loading WOPL bank");
  // Loading with automatic allocation detects rhythm-mode requirements. Set the
  // explicit four-op reservation only AFTER this detection and bank load.
  check(adl_setNumFourOpsChn(player, settings.fourOpChannels), "Setting four-operator channel count");
  adl_setAutoArpeggio(player, 0);
  adl_setSoftPanEnabled(player, 0);
  setDepths(settings.deepVibrato, settings.deepTremolo);
  // No debug callback: upstream's missing-bank diagnostics insert into std::set
  // only when a callback is installed. Normal note/control paths use fixed lists.
  m_impl->slots.reserve(bank.programs.size());
  for(const auto& program : bank.programs)
  {
    ADL_BankId id{std::uint8_t(program.address.percussion), program.address.msb, program.address.lsb};
    ADL_Bank handle{};
    check(adl_getBank(player, &id, 0, &handle), "Finding loaded bank slot");
    m_impl->slots.push_back({program.address, handle, program.instrument});
  }
  adl_rt_resetState(player);
  auto& synth = *static_cast<MIDIplay*>(player->adl_midiPlayer)->m_synth;
  for(unsigned i = 0; i < synth.m_numChips; ++i)
  {
    auto& chip = synth.m_chips[i];
    chip.reset(new GuardedChip{chip, static_cast<uint32_t>(sampleRate), m_impl->registerOverflow});
  }
}

Engine::~Engine() = default;
const char* Engine::configurationError(const Settings& settings) const noexcept
{
  if(const auto* error = settingsError(settings)) return error;
  if(m_impl->fourOperatorVoices)
  {
    if(settings.model == Model::Opl2)
      return "OPL2 cannot play true four-operator instruments; select OPL3";
    if(settings.fourOpChannels == 0)
      return "This bank requires at least one configured four-operator channel";
  }
  if(m_impl->opl3Waveforms && settings.model == Model::Opl2)
    return "OPL2 supports only waveforms 0 through 3; select OPL3 or edit the patch";
  return nullptr;
}

void Engine::midi(std::span<const std::uint8_t> message) noexcept
{
  auto& impl = *m_impl;
  impl.error = nullptr;
  if(impl.registerOverflow) return;
  if(message.empty())
  {
    impl.error = "Empty MIDI message";
    return;
  }
  auto* player = impl.player.get();
  const auto status = message[0];
  if(status == 0xF0)
  {
    if(message.size() < 4 || message.back() != 0xF7
       || std::any_of(message.begin() + 1, message.end() - 1, [](auto b) { return b >= 0x80; }))
      impl.error = "Expected one complete 7-bit SysEx message including F0 and F7";
    else if(adl_rt_systemExclusive(player, message.data(), message.size()) <= 0)
      impl.error = "Unsupported SysEx, invalid checksum, or nonmatching device ID";
    return;
  }
  if(status == 0xFF && message.size() == 1)
  {
    panic();
    adl_rt_resetState(player);
    return;
  }
  if(status < 0x80 || status >= 0xF0)
  {
    impl.error = "Unsupported MIDI system message or missing explicit channel status";
    return;
  }
  const auto type = status & 0xF0;
  const std::size_t size = type == 0xC0 || type == 0xD0 ? 2 : 3;
  if(message.size() != size || message[1] > 127 || (size == 3 && message[2] > 127))
  {
    impl.error = "Invalid MIDI channel-message size or data byte";
    return;
  }
  const auto channel = status & 15;
  const auto a = message[1];
  const auto b = size == 3 ? message[2] : 0;
  switch(type)
  {
    case 0x80: adl_rt_noteOff(player, channel, a); break;
    case 0x90:
      if(b) adl_rt_noteOn(player, channel, a, b);
      else adl_rt_noteOff(player, channel, a);
      break;
    case 0xA0: adl_rt_noteAfterTouch(player, channel, a, b); break;
    case 0xB0: adl_rt_controllerChange(player, channel, a, b); break;
    case 0xC0: adl_rt_patchChange(player, channel, a); break;
    case 0xD0: adl_rt_channelAfterTouch(player, channel, a); break;
    case 0xE0: adl_rt_pitchBend(player, channel, a | (unsigned(b) << 7)); break;
  }
}

void Engine::render(double* left, double* right, std::size_t frames) noexcept
{
  if(frames == 0)
    return;
  if(!left || !right || left == right)
  {
    m_impl->error = "OPL render requires two distinct non-null planar buffers";
    return;
  }
  if(m_impl->registerOverflow)
  {
    std::fill_n(left, frames, 0.);
    std::fill_n(right, frames, 0.);
    return;
  }
  constexpr ADLMIDI_AudioFormat format{ADLMIDI_SampleType_F64, sizeof(double), sizeof(double)};
  while(frames)
  {
    // Never exceed upstream's 512-frame internal scratch buffer. Smaller
    // chunks bound software modulation and short drum-note timer granularity.
    const auto count = std::min<std::size_t>(frames, 64);
    const auto generated = adl_generateFormat(m_impl->player.get(), int(count * 2),
        reinterpret_cast<ADL_UInt8*>(left), reinterpret_cast<ADL_UInt8*>(right), &format);
    if(generated != int(count * 2) || m_impl->registerOverflow)
    {
      std::fill_n(left, frames, 0.);
      std::fill_n(right, frames, 0.);
      m_impl->error = "libADLMIDI did not produce the requested audio frames";
      return;
    }
    // Contrary to the public header's wording, the pinned generateFormat calls
    // TickIterators itself (adlmidi.cpp). A second tick doubles release aging.
    left += count;
    right += count;
    frames -= count;
  }
}

void Engine::panic() noexcept
{
  m_impl->error = nullptr;
  if(m_impl->registerOverflow) return;
  auto* player = m_impl->player.get();
  adl_panic(player);
  auto& synth = *static_cast<MIDIplay*>(player->adl_midiPlayer)->m_synth;
  // Key-off alone cannot stop a native envelope with release rate zero.
  // Silence register writes are queued by ymfm and settle as audio advances.
  synth.silenceAll();
  // Pinned upstream silenceAll writes the modulator's 0x80 register twice
  // (adlmidi_opl3.cpp:835), leaving an RR=0 carrier audible indefinitely.
  // Set release on every physical operator without resetting/allocating chips.
  const unsigned banks = m_impl->settings.model == Model::Opl3 ? 2 : 1;
  for(unsigned chip = 0; chip < synth.m_numChips; ++chip)
    for(unsigned bank = 0; bank < banks; ++bank)
      for(unsigned group = 0; group < 3; ++group)
        for(unsigned op = 0; op < 6; ++op)
          synth.writeRegI(chip, bank * 0x100 + 0x80 + group * 8 + op, 0xff);
  // silenceAll changes attenuation/envelope registers; reload on the next note.
  synth.clearInstCache();
}

void Engine::setInstrument(Address address, const ADL_Instrument& instrument) noexcept
{
  auto& impl = *m_impl;
  impl.error = instrumentError(instrument, address, impl.settings);
  if(impl.error)
    return;
  if(!impl.rhythmMode && !(instrument.inst_flags & ADLMIDI_Ins_IsBlank)
     && (instrument.inst_flags & ADLMIDI_Ins_RhythmModeMask))
  {
    impl.error = "Enabling hardware rhythm mode requires rebuilding the engine with the edited bank";
    return;
  }
  auto it = std::find_if(impl.slots.begin(), impl.slots.end(),
      [address](const Impl::Slot& slot) { return slot.address == address; });
  if(it == impl.slots.end())
  {
    impl.error = "Cannot edit a nonexistent bank slot; load or create the bank off the audio thread";
    return;
  }
  auto* player = impl.player.get();
  panic();
  if(adl_setInstrument(player, &it->bank, address.program, &instrument) < 0)
  {
    impl.error = "libADLMIDI rejected the instrument edit";
    return;
  }
  impl.countRequirements(it->instrument, -1);
  impl.countRequirements(instrument, 1);
  it->instrument = instrument;
  // panic() also invalidated the upstream pointer-identity patch cache, so the
  // next note reloads this edited slot even when it reuses the same chip voice.
}

bool Engine::getInstrument(Address address, ADL_Instrument& instrument) const noexcept
{
  const auto& slots = m_impl->slots;
  const auto it = std::find_if(slots.begin(), slots.end(),
      [address](const Impl::Slot& slot) { return slot.address == address; });
  if(it == slots.end())
    return false;
  // Keep the exact authored fields, not the lossy runtime conversion returned
  // by adl_getInstrument (notably second-voice detune and reserved native bits).
  instrument = it->instrument;
  return true;
}

void Engine::setDepths(bool vibrato, bool tremolo) noexcept
{
  m_impl->error = nullptr;
  m_impl->settings.deepVibrato = vibrato;
  m_impl->settings.deepTremolo = tremolo;
  adl_setHVibrato(m_impl->player.get(), vibrato);
  adl_setHTremolo(m_impl->player.get(), tremolo);
}

const char* Engine::lastError() const noexcept
{
  return m_impl->registerOverflow
      ? "ymfm register queue overflow: execution silenced; recreate the OPL engine."
      : m_impl->error;
}
}
