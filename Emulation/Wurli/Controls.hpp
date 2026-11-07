#pragma once
#include "Parameters.hpp"
#include <Process/Dataflow/WidgetInlets.hpp>
#include <vector>
namespace Emulation::Wurli
{
std::vector<Process::ControlInlet*> makeControls(QObject*);
Process::ComboBox* makeAllocationControl(QObject*);
void updateControlMetadata(Process::ControlInlet&, int);
}
