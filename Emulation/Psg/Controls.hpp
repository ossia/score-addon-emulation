#pragma once
#include "Parameters.hpp"
#include <Process/Dataflow/WidgetInlets.hpp>
#include <vector>
namespace Emulation::Psg {
void updateAllocationControl(Process::ControlInlet&);
std::vector<Process::ControlInlet*> makeControls(QObject*);
}
