#pragma once
#include <Process/ProcessMetadata.hpp>
#include <QString>
namespace Emulation::Wurli { class ProcessModel; }
PROCESS_METADATA(
    , Emulation::Wurli::ProcessModel, "270d9ee2-2a2d-48fd-8bac-9e8ba73a5f4d", "Wurlitzer",
    "Wurlie", Process::ProcessCategory::Synth, "Synths/Emulation",
    "Wurlitzer 200A physical reed, pickup and amplifier model with OpenWurli", "ossia score",
    (QStringList{"Wurlitzer", "200A", "Electric piano", "Physical model"}),
    // As the process makes them: MIDI in, the synth's controls, audio out
    (std::vector<Process::PortType>{Process::PortType::Midi, Process::PortType::Message}),
    (std::vector<Process::PortType>{Process::PortType::Audio}), QUrl{},
    Process::ProcessFlags::SupportsAll | Process::ProcessFlags::PutInNewSlot
        | Process::ProcessFlags::ControlSurface)
