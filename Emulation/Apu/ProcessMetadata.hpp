#pragma once
#include <Process/ProcessMetadata.hpp>
#include <QString>
namespace Emulation::Apu { class ProcessModel; }
PROCESS_METADATA(, Emulation::Apu::ProcessModel, "28b1e0ab-e245-4a25-8fb6-d05e43bf6f08", "Console APU",
  "Consolated", Process::ProcessCategory::Synth, "Synths/Emulation",
  "Native NES NTSC/PAL and Game Boy DMG/CGB channels", "ossia score",
  (QStringList{"NES", "APU", "Game Boy", "DMG", "CGB", "Console APU"}), {}, {}, QUrl{},
  Process::ProcessFlags::SupportsAll | Process::ProcessFlags::PutInNewSlot | Process::ProcessFlags::ControlSurface)
