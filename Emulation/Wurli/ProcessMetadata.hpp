#pragma once
#include <Process/ProcessMetadata.hpp>
#include <QString>
namespace Emulation::Wurli { class ProcessModel; }
PROCESS_METADATA(
    , Emulation::Wurli::ProcessModel, "270d9ee2-2a2d-48fd-8bac-9e8ba73a5f4d", "Wurlitzer",
    "Wurlie", Process::ProcessCategory::Synth, "Synths/Emulation",
    "Wurlitzer 200A physical reed, pickup and amplifier model with OpenWurli", "ossia score",
    (QStringList{"Wurlitzer", "200A", "Electric piano", "Physical model"}), {}, {}, QUrl{},
    Process::ProcessFlags::SupportsAll | Process::ProcessFlags::PutInNewSlot
        | Process::ProcessFlags::ControlSurface)
