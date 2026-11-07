// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Emulation/La/Engine.hpp>
#include <Process/Execution/ProcessComponent.hpp>
#include <ossia/dataflow/node_process.hpp>
#include <QPointer>
#include <atomic>
#include <memory>
namespace Emulation::La
{
class ProcessModel;
namespace Executor
{
class Component final : public ::Execution::ProcessComponent_T<ProcessModel, ossia::node_process>
{
  COMPONENT_METADATA("6605f771-7483-455c-bbad-c5c51a6e4614")
public:
  Component(ProcessModel&, const ::Execution::Context&, QObject*);
  ~Component() override;
private:
  void prepare(std::shared_ptr<const Patch>);
  std::shared_ptr<const Patch> m_snapshot;
  std::shared_ptr<std::atomic<bool>> m_cancel;
  QPointer<ProcessModel> m_model;
  const char* m_reportedError{};
};
using ComponentFactory = ::Execution::ProcessComponentFactory_T<Component>;
}
}
