#include <Emulation/Opl/Engine.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace Emulation::Opl;

struct Options
{
  std::string bank{"factory"};
  std::filesystem::path output{"opl.wav"};
  Settings settings;
  Address address;
  int rate{48000};
  int block{256};
  int note{60};
  int velocity{100};
  double on{0.125};
  double off{1.25};
  double duration{3.};
  bool list{};
};

void usage()
{
  std::cout << "opl-render [options]\n"
    "  --bank factory|PATH.wopl   Original authored bank or native WOPL file\n"
    "  --output PATH.wav         Stereo PCM16 output (default opl.wav)\n"
    "  --list                    List named/nonblank native patches and exit\n"
    "  --model opl2|opl3         ymfm hardware model (default opl3)\n"
    "  --chips N --four-ops N    Chip count and total four-operator channels\n"
    "  --msb N --lsb N --program N [--drum]   Native WOPL address, zero-based\n"
    "  --note N --velocity N    MIDI key and velocity (default 60, 100)\n"
    "  --on SEC --off SEC       Timestamped MIDI note events (default .125, 1.25)\n"
    "  --duration SEC          Total duration including release (default 3)\n"
    "  --rate HZ --block FRAMES Sample rate and host block size (48000, 256)\n"
    "Drum --program is the drum key; --lsb is the kit program (128+ for XG SFX).\n";
}

int integer(std::string_view text, int low, int high, const char* name)
{
  std::size_t consumed{};
  const auto value = std::stol(std::string{text}, &consumed);
  if(consumed != text.size() || value < low || value > high)
    throw std::runtime_error{std::string{name} + " is out of range"};
  return static_cast<int>(value);
}

double seconds(std::string_view text)
{
  std::size_t consumed{};
  const auto value = std::stod(std::string{text}, &consumed);
  if(consumed != text.size() || !std::isfinite(value) || value < 0 || value > 3600)
    throw std::runtime_error{"Times must be finite seconds in [0, 3600]"};
  return value;
}

Options parse(int argc, char** argv)
{
  Options options;
  for(int i = 1; i < argc; ++i)
  {
    const std::string_view key{argv[i]};
    if(key == "--drum") { options.address.percussion = true; continue; }
    if(key == "--list") { options.list = true; continue; }
    if(i + 1 == argc)
      throw std::runtime_error{"Missing value after " + std::string{key}};
    const std::string_view value{argv[++i]};
    if(key == "--bank") options.bank = value;
    else if(key == "--output") options.output = std::string{value};
    else if(key == "--model")
    {
      if(value == "opl2") options.settings.model = Model::Opl2;
      else if(value == "opl3") options.settings.model = Model::Opl3;
      else throw std::runtime_error{"--model must be opl2 or opl3"};
    }
    else if(key == "--chips") options.settings.chips = integer(value, 1, 100, "chips");
    else if(key == "--four-ops") options.settings.fourOpChannels = integer(value, 0, 600, "four-ops");
    else if(key == "--msb") options.address.msb = std::uint8_t(integer(value, 0, 127, "MSB"));
    else if(key == "--lsb") options.address.lsb = std::uint8_t(integer(value, 0, 255, "LSB"));
    else if(key == "--program") options.address.program = std::uint8_t(integer(value, 0, 127, "program"));
    else if(key == "--note") options.note = integer(value, 0, 127, "note");
    else if(key == "--velocity") options.velocity = integer(value, 1, 127, "velocity");
    else if(key == "--rate") options.rate = integer(value, 8000, 384000, "rate");
    else if(key == "--block") options.block = integer(value, 1, 65536, "block");
    else if(key == "--on") options.on = seconds(value);
    else if(key == "--off") options.off = seconds(value);
    else if(key == "--duration") options.duration = seconds(value);
    else throw std::runtime_error{"Unknown option " + std::string{key}};
  }
  if(!(options.on < options.off && options.off < options.duration))
    throw std::runtime_error{"Require 0 <= --on < --off < --duration"};
  return options;
}

std::vector<std::uint8_t> load(const std::filesystem::path& path)
{
  std::ifstream stream{path, std::ios::binary | std::ios::ate};
  if(!stream)
    throw std::runtime_error{"Cannot open bank " + path.string()};
  const auto size = stream.tellg();
  if(size < 0 || size > 64 * 1024 * 1024)
    throw std::runtime_error{"Bank file is unreadable or exceeds 64 MiB"};
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  stream.seekg(0);
  if(!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    throw std::runtime_error{"Could not read the complete WOPL file"};
  return bytes;
}

void little(std::ostream& stream, std::uint32_t value, unsigned bytes)
{
  for(unsigned i = 0; i < bytes; ++i)
    stream.put(static_cast<char>((value >> (8 * i)) & 255));
}

void send(Engine& engine, std::initializer_list<std::uint8_t> message)
{
  engine.midi({message.begin(), message.size()});
  if(const auto* error = engine.lastError())
    throw std::runtime_error{error};
}
}

int main(int argc, char** argv)
{
  try
  {
    if(argc == 2 && (std::string_view{argv[1]} == "--help" || std::string_view{argv[1]} == "-h"))
    {
      usage();
      return 0;
    }
    const auto options = parse(argc, argv);
    const auto bank = options.bank == "factory" ? factoryBank() : decodeBank(load(options.bank));
    if(options.list)
    {
      for(const auto& program : bank.programs)
      {
        if(program.name.empty() && (program.instrument.inst_flags & ADLMIDI_Ins_IsBlank))
          continue;
        std::cout << (program.address.percussion ? "drum" : "melodic")
                  << " msb=" << int(program.address.msb) << " lsb=" << int(program.address.lsb)
                  << " program=" << int(program.address.program) << " flags=" << int(program.instrument.inst_flags)
                  << " name=" << std::quoted(program.name) << '\n';
      }
      return 0;
    }
    const auto* program = findProgram(bank, options.address);
    if(!program || (program->instrument.inst_flags & ADLMIDI_Ins_IsBlank))
      throw std::runtime_error{"Selected WOPL address is missing or blank; use --list to choose a playable patch"};

    Engine engine{double(options.rate), options.settings, bank};
    const auto channel = std::uint8_t(options.address.percussion ? 9 : 0);
    if(options.address.percussion)
    {
      send(engine, {std::uint8_t(0xB0 | channel), 0, std::uint8_t(options.address.lsb >= 128 ? 0x7E : 0x7F)});
      send(engine, {std::uint8_t(0xC0 | channel), std::uint8_t(options.address.lsb & 127)});
    }
    else
    {
      send(engine, {0xB0, 0, options.address.msb});
      send(engine, {0xB0, 32, options.address.lsb});
      send(engine, {0xC0, options.address.program});
    }
    const auto key = std::uint8_t(options.address.percussion ? options.address.program : options.note);
    const auto onFrame = std::uint64_t(std::llround(options.on * options.rate));
    const auto offFrame = std::uint64_t(std::llround(options.off * options.rate));
    const auto total = std::uint64_t(std::llround(options.duration * options.rate));
    if(onFrame >= offFrame || offFrame >= total)
      throw std::runtime_error{"Event timestamps collapse to the same sample; increase their separation"};
    if(total > (std::numeric_limits<std::uint32_t>::max() - 36u) / 4u)
      throw std::runtime_error{"Requested output exceeds the RIFF WAV 4 GiB limit"};
    std::ofstream wave{options.output, std::ios::binary};
    if(!wave) throw std::runtime_error{"Cannot create WAV output"};
    wave.write("RIFF", 4); little(wave, std::uint32_t(total * 4 + 36), 4);
    wave.write("WAVEfmt ", 8); little(wave, 16, 4); little(wave, 1, 2); little(wave, 2, 2);
    little(wave, options.rate, 4); little(wave, options.rate * 4, 4);
    little(wave, 4, 2); little(wave, 16, 2);
    wave.write("data", 4); little(wave, std::uint32_t(total * 4), 4);

    std::vector<double> left(options.block), right(options.block);
    std::vector<char> pcm(std::size_t(options.block) * 4);
    std::uint64_t frame{}, firstAudible = total, clipped{}, hash = 14695981039346656037ULL;
    double peak{}, preEnergy{}, activeEnergy{}, releaseEnergy{}, endingEnergy{};
    const auto endingStart = std::max(offFrame, total - std::min(total, std::uint64_t(options.rate / 4)));
    bool sentOn{}, sentOff{};
    while(frame < total)
    {
      if(frame == onFrame && !sentOn)
      {
        send(engine, {std::uint8_t(0x90 | channel), key, std::uint8_t(options.velocity)});
        sentOn = true;
      }
      if(frame == offFrame && !sentOff)
      {
        send(engine, {std::uint8_t(0x80 | channel), key, 0});
        sentOff = true;
      }
      auto end = std::min(total, frame + std::uint64_t(options.block));
      if(!sentOn) end = std::min(end, onFrame);
      else if(!sentOff) end = std::min(end, offFrame);
      const auto frames = std::size_t(end - frame);
      engine.render(left.data(), right.data(), frames);
      if(const auto* error = engine.lastError()) throw std::runtime_error{error};
      for(std::size_t i = 0; i < frames; ++i)
      {
        const auto absolute = frame + i;
        const auto energy = (left[i] * left[i] + right[i] * right[i]) * .5;
        if(absolute < onFrame) preEnergy += energy;
        else if(absolute < offFrame) activeEnergy += energy;
        else releaseEnergy += energy;
        if(absolute >= endingStart) endingEnergy += energy;
        for(int channelIndex = 0; channelIndex < 2; ++channelIndex)
        {
          const auto sample = channelIndex ? right[i] : left[i];
          if(!std::isfinite(sample)) throw std::runtime_error{"Nonfinite sample from OPL renderer"};
          peak = std::max(peak, std::abs(sample));
          if(std::abs(sample) > 1.) ++clipped;
          if(std::abs(sample) > 1. / 32768. && firstAudible == total) firstAudible = absolute;
          const auto value = std::int16_t(std::lround(std::clamp(sample, -1., 1.) * 32767.));
          const auto unsignedValue = std::uint16_t(value);
          const auto offset = i * 4 + std::size_t(channelIndex) * 2;
          pcm[offset] = char(unsignedValue & 255);
          pcm[offset + 1] = char(unsignedValue >> 8);
        }
      }
      for(std::size_t i = 0; i < frames * 4; ++i)
      {
        hash ^= static_cast<unsigned char>(pcm[i]);
        hash *= 1099511628211ULL;
      }
      wave.write(pcm.data(), static_cast<std::streamsize>(frames * 4));
      if(!wave) throw std::runtime_error{"WAV output write failed"};
      frame = end;
    }
    wave.close();
    if(!wave) throw std::runtime_error{"Could not finalize WAV output"};
    const auto activeRms = std::sqrt(activeEnergy / double(offFrame - onFrame));
    std::cout << std::setprecision(10)
              << "backend=ymfm model=" << (options.settings.model == Model::Opl2 ? "opl2" : "opl3")
              << " preset=" << std::quoted(program->name) << " note=" << int(key)
              << " rate=" << options.rate << " frames=" << total << " channels=2 pcm_bits=16\n"
              << "note_on_frame=" << onFrame << " note_off_frame=" << offFrame
              << " first_audible_frame=" << (firstAudible == total ? -1 : std::int64_t(firstAudible)) << '\n'
              << "peak=" << peak << " pre_note_rms=" << std::sqrt(preEnergy / double(std::max<std::uint64_t>(1, onFrame)))
              << " active_rms=" << activeRms
              << " release_rms=" << std::sqrt(releaseEnergy / double(total - offFrame))
              << " final_quarter_second_rms=" << std::sqrt(endingEnergy / double(total - endingStart))
              << " clipped_samples=" << clipped << '\n'
              << "pcm_fnv1a64=" << std::hex << hash << std::dec
              << " wav_bytes=" << total * 4 + 44 << " output=" << std::quoted(options.output.string()) << '\n';
    if(activeRms < 1e-7)
      throw std::runtime_error{"Selected patch produced no measurable note signal"};
    return 0;
  }
  catch(const std::exception& e)
  {
    std::cerr << "opl-render: " << e.what() << '\n';
    return 1;
  }
}
