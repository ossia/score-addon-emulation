#pragma once
#include <Process/ProcessMetadata.hpp>
#include <QString>
namespace Emulation::Psg { class ProcessModel; }
PROCESS_METADATA(
    , Emulation::Psg::ProcessModel, "5aabbeda-10c7-40ca-985a-90d7863df990", "PSG",
    "AYstral", Process::ProcessCategory::Synth, "Synths/Emulation",
    "Three native tones and shared noise: AY-3-8910, YM2149, Sega YM7101", "ossia score",
    (QStringList{"PSG", "AY-3-8910", "YM2149", "YM7101", "Sega"}), {}, {}, QUrl{},
    Process::ProcessFlags::SupportsAll | Process::ProcessFlags::PutInNewSlot
        | Process::ProcessFlags::ControlSurface)
