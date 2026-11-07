#pragma once
#include "Parameters.hpp"
#include <Process/Dataflow/WidgetInlets.hpp>
#include <vector>
namespace Emulation::Apu {
std::vector<Process::ControlInlet*> makeControls(QObject*);
QString controlLabel(Model,int);
void updateControlMetadata(Process::ControlInlet&,Model,int);
}
