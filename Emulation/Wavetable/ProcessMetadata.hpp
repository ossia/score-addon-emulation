#pragma once
#include <Process/ProcessMetadata.hpp>
#include <QString>
namespace Emulation::Wavetable { class ProcessModel; }
PROCESS_METADATA(
    , Emulation::Wavetable::ProcessModel, "989bc228-c2e7-40a8-a56e-4700b946d864", "Chip Wavetable",
    "SiCC", Process::ProcessCategory::Synth, "Synths/Emulation",
    "Physical five-voice SCC / SCC+ and six-voice HuC6280 wavetable chips", "ossia score",
    (QStringList{"SCC", "SCC+", "HuC6280", "Wavetable", "Chip Wavetable"}),
    // As the process makes them: MIDI in, the synth's controls, audio out
    (std::vector<Process::PortType>{Process::PortType::Midi, Process::PortType::Message}),
    (std::vector<Process::PortType>{Process::PortType::Audio}), QUrl{},
    Process::ProcessFlags::SupportsAll | Process::ProcessFlags::PutInNewSlot | Process::ProcessFlags::ControlSurface)
