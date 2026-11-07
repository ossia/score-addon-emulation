// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Emulation/Waveform.hpp>
#include <Dataflow/WaveformInlet.hpp>

#include <memory>

namespace Emulation
{
inline Dataflow::WaveformInlet* makeWaveformInlet(
    ossia::value value, int amplitudeLevels, QString name,
    Id<Process::Port> id, QObject* parent)
{
  const auto table = Process::wavetableFromValue(value);
  if(!table || table->frameCount() != 1 || table->frameSize != 32)
    throw std::invalid_argument("Chip waveforms require one 32-sample cycle");
  auto inlet = std::make_unique<Dataflow::WaveformInlet>(id, parent);
  inlet->setFrameSizeConstraint(32);
  inlet->setAmplitudeLevels(amplitudeLevels);
  inlet->setName(name);
  inlet->displayHandledExplicitly = true;
  inlet->setWaveform(*table);
  inlet->setInit(inlet->value());
  return inlet.release();
}
}
