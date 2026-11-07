// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Engine.hpp"
#include <Process/Dataflow/WidgetInlets.hpp>
#include <vector>
namespace Emulation::La
{
std::vector<Process::ControlInlet*> makeControls(QObject*);
std::vector<std::pair<QString, ossia::value>> programChoices(const Patch&, int part, const ProgramCatalog*);
}
