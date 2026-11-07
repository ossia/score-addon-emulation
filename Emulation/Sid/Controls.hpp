// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Engine.hpp"
#include <Process/Dataflow/WidgetInlets.hpp>
#include <vector>
namespace Emulation::Sid
{
std::vector<Process::ControlInlet*> makeControls(QObject*);
void updateControlMetadata(Process::ControlInlet&, int);
}
