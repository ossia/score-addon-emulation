// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Emulation/Apu/Engine.hpp>
#include <Process/Execution/ProcessComponent.hpp>
#include <ossia/dataflow/node_process.hpp>
namespace Emulation::Apu {
class ProcessModel;
namespace Executor {
class Component final : public ::Execution::ProcessComponent_T<ProcessModel,ossia::node_process> {
  COMPONENT_METADATA("2c7205ec-a8ad-4534-8486-1a3e78dbe315")
public:
  Component(ProcessModel&,const ::Execution::Context&,QObject*);
  ~Component() override;
private:
  void prepare(Settings);
  uint64_t m_revision{};
  uint32_t m_seenWave{};
  QString m_preparationError;
};
using ComponentFactory = ::Execution::ProcessComponentFactory_T<Component>;
}
}
