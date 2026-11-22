#pragma once
#include <Process/ProcessMetadata.hpp>
#include <QString>
namespace Emulation::Psg { class ProcessModel; }
PROCESS_METADATA(
    , Emulation::Psg::ProcessModel, "5aabbeda-10c7-40ca-985a-90d7863df990", "PSG",
    "AYstral", Process::ProcessCategory::Synth, "Synths/Emulation",
    "Three native tones and shared noise: AY-3-8910, YM2149, Sega YM7101", "ossia score",
    (QStringList{"PSG", "AY-3-8910", "YM2149", "YM7101", "Sega"}),
    // As the process makes them: MIDI in, the synth's controls, audio out
    (std::vector<Process::PortType>{Process::PortType::Midi, Process::PortType::Message}),
    (std::vector<Process::PortType>{Process::PortType::Audio}), QUrl{},
    Process::ProcessFlags::SupportsAll | Process::ProcessFlags::PutInNewSlot
        | Process::ProcessFlags::ControlSurface)
