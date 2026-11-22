#pragma once
#include <Process/ProcessMetadata.hpp>
#include <QString>
namespace Emulation::Opl { class ProcessModel; }
PROCESS_METADATA(
    , Emulation::Opl::ProcessModel, "2fe73b91-4100-4cbb-b8d8-10e82c9f317a", "OPL",
    "Opale", Process::ProcessCategory::Synth, "Synths/Emulation",
    "Native OPL2 / OPL3 FM instrument and WOPL bank editor", "ossia score",
    (QStringList{"OPL", "OPL2", "OPL3", "FM", "WOPL"}),
    // As the process makes them: MIDI in, the synth's controls, audio out
    (std::vector<Process::PortType>{Process::PortType::Midi, Process::PortType::Message}),
    (std::vector<Process::PortType>{Process::PortType::Audio}), QUrl{},
    Process::ProcessFlags::SupportsAll | Process::ProcessFlags::PutInNewSlot
        | Process::ProcessFlags::ControlSurface)
