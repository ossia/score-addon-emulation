#pragma once
#include <Process/ProcessMetadata.hpp>
#include <QString>
namespace Emulation::Apu { class ProcessModel; }
PROCESS_METADATA(, Emulation::Apu::ProcessModel, "28b1e0ab-e245-4a25-8fb6-d05e43bf6f08", "Console APU",
  "Consolated", Process::ProcessCategory::Synth, "Synths/Emulation",
  "Native NES NTSC/PAL and Game Boy DMG/CGB channels", "ossia score",
  (QStringList{"NES", "APU", "Game Boy", "DMG", "CGB", "Console APU"}),
    // As the process makes them: MIDI in, the synth's controls, audio out
    (std::vector<Process::PortType>{Process::PortType::Midi, Process::PortType::Message}),
    (std::vector<Process::PortType>{Process::PortType::Audio}), QUrl{},
  Process::ProcessFlags::SupportsAll | Process::ProcessFlags::PutInNewSlot | Process::ProcessFlags::ControlSurface)
