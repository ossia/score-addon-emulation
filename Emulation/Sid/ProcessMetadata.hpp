// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Process/ProcessMetadata.hpp>
#include <QString>
namespace Emulation::Sid { class ProcessModel; }
PROCESS_METADATA(
    , Emulation::Sid::ProcessModel, "ce87e734-2cf5-4044-bff4-c849e98bce4a", "SID",
    "SIDérant", Process::ProcessCategory::Synth, "Synths/Emulation",
    "Native three-oscillator SID 6581 / 8580 instrument", "ossia score",
    (QStringList{"SID", "6581", "8580", "C64", "Chiptune"}),
    // As the process makes them: MIDI in, the synth's controls, audio out
    (std::vector<Process::PortType>{Process::PortType::Midi, Process::PortType::Message}),
    (std::vector<Process::PortType>{Process::PortType::Audio}), QUrl{},
    Process::ProcessFlags::SupportsAll | Process::ProcessFlags::PutInNewSlot
        | Process::ProcessFlags::ControlSurface)
