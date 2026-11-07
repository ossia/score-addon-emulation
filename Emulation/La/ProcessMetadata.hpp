// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Process/ProcessMetadata.hpp>
#include <QString>
namespace Emulation::La { class ProcessModel; }
PROCESS_METADATA(
    , Emulation::La::ProcessModel, "c39b18a6-7840-45b1-a5bc-7e884ecfea02", "Roland LA",
    "LAdrenaline", Process::ProcessCategory::Synth, "Synths/Emulation",
    "Roland MT-32 and CM-32L linear arithmetic synthesis with user-provided ROMs", "ossia score",
    (QStringList{"Roland", "LA", "MT-32", "CM-32L", "munt"}), {}, {}, QUrl{},
    Process::ProcessFlags::SupportsAll | Process::ProcessFlags::PutInNewSlot
        | Process::ProcessFlags::ControlSurface)
