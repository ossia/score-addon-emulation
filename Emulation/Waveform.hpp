// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Process/Dataflow/Wavetable.hpp>

#include <atomic>
#include <cstdint>

namespace Emulation::Waveform
{
inline std::optional<double> number(const ossia::value& value) noexcept
{
  if(const auto* integer = value.target<int>()) return *integer;
  if(const auto* real = value.target<float>(); real && std::isfinite(*real)) return *real;
  return std::nullopt;
}

inline ossia::value triangle()
{
  return Process::wavetableToValue(Process::makeTriangleWavetable());
}

//! A chip plays one periodic frame: the first frame of a bank, or a flat list
//! of samples, resampled to the chip's wave RAM. Reads the value in place, so
//! the audio thread can call it on what an inlet received.
template <typename T, std::size_t N>
bool sample(const ossia::value& value, std::array<T, N>& output, double low, double high) noexcept
{
  return Process::prepareWaveform(value, output, low, high);
}

//! The last wave RAM a node received through execution, handed to the UI
//! thread without locks. The audio thread is the only writer. A reader keeps
//! its copy only when no write overlapped it, and otherwise retries later.
template <std::size_t N>
class ReceivedWave
{
public:
  template <typename T>
  void publish(const std::array<T, N>& samples) noexcept
  {
    const auto sequence = m_sequence.load(std::memory_order_relaxed);
    m_sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for(std::size_t i = 0; i < N; ++i)
      m_samples[i].store(float(samples[i]), std::memory_order_relaxed);
    m_sequence.store(sequence + 2, std::memory_order_release);
  }

  //! True when a wave newer than `seen` was copied into `samples`.
  bool take(uint32_t& seen, std::array<float, N>& samples) const noexcept
  {
    const auto before = m_sequence.load(std::memory_order_acquire);
    if(before == seen || (before & 1))
      return false;
    for(std::size_t i = 0; i < N; ++i)
      samples[i] = m_samples[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if(m_sequence.load(std::memory_order_relaxed) != before)
      return false;
    seen = before;
    return true;
  }

private:
  std::array<std::atomic<float>, N> m_samples{};
  std::atomic<uint32_t> m_sequence{};
};

template <typename T, std::size_t N>
ossia::value fromSamples(const std::array<T, N>& samples, double low, double high)
{
  if(!std::isfinite(low) || !std::isfinite(high) || low >= high)
    throw std::invalid_argument("Invalid chip sample range");
  Process::WavetableData table{N, std::vector<float>(N)};
  for(std::size_t i = 0; i < N; ++i)
    table.samples[i] = float(2. * (double(samples[i]) - low) / (high - low) - 1.);
  return Process::wavetableToValue(table);
}
}
