// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Emulation/Wavetable/Engine.hpp>
#include <Process/Execution/ProcessComponent.hpp>
#include <ossia/dataflow/node_process.hpp>
namespace Emulation::Wavetable
{
class ProcessModel;
namespace Executor
{
class Component final : public ::Execution::ProcessComponent_T<ProcessModel, ossia::node_process>
{
  COMPONENT_METADATA("523de9e8-4320-4019-9a6a-4857407a38ad")
public:
  Component(ProcessModel&, const ::Execution::Context&, QObject*);
  ~Component() override;
private:
  void prepare(Settings);
  const char* m_reportedError{};
  std::array<uint32_t, 6> m_seenWaves{};
};
using ComponentFactory = ::Execution::ProcessComponentFactory_T<Component>;
}
}
