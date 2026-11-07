// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Emulation/Psg/Engine.hpp>
#include <Process/Execution/ProcessComponent.hpp>
#include <ossia/dataflow/node_process.hpp>
namespace Emulation::Psg
{
class ProcessModel;
namespace Executor
{
class Component final : public ::Execution::ProcessComponent_T<ProcessModel, ossia::node_process>
{
  COMPONENT_METADATA("8ab0ebb3-d7dc-4dc0-b9bc-7da7d56d2eaf")
public:
  Component(ProcessModel&, const ::Execution::Context&, QObject*);
  ~Component() override;
private:
  void prepare(Settings);
  QString m_reportedError;
  QString m_preparationError;
};
using ComponentFactory = ::Execution::ProcessComponentFactory_T<Component>;
}
}
