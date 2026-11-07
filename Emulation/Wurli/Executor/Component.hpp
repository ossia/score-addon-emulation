// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Emulation/Wurli/Engine.hpp>
#include <Process/Execution/ProcessComponent.hpp>
#include <ossia/dataflow/node_process.hpp>
#include <QPointer>
#include <atomic>
namespace Emulation::Wurli
{
class ProcessModel;
namespace Executor
{
class Component final : public ::Execution::ProcessComponent_T<ProcessModel, ossia::node_process>
{
  COMPONENT_METADATA("8a7df691-7f79-4c96-bba4-93613004fa8b")
public:
  Component(ProcessModel&, const ::Execution::Context&, QObject*);
  ~Component() override;
private:
  void prepare(Settings);
  const char* m_reportedError{};
  QPointer<ProcessModel> m_model;
  std::shared_ptr<std::atomic<bool>> m_cancel;
};
using ComponentFactory = ::Execution::ProcessComponentFactory_T<Component>;
}
}
