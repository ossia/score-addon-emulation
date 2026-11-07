// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Emulation/Opl/Engine.hpp>
#include <Process/Execution/ProcessComponent.hpp>
#include <ossia/dataflow/node_process.hpp>
#include <QPointer>
#include <atomic>
#include <memory>

namespace Emulation::Opl
{
class ProcessModel;
namespace Executor
{
class Component final : public ::Execution::ProcessComponent_T<ProcessModel, ossia::node_process>
{
  COMPONENT_METADATA("3b5a1ad9-9a8a-4bca-a389-59113d2d12cb")
public:
  Component(ProcessModel&, const ::Execution::Context&, QObject*);
  ~Component() override;
private:
  void prepare(Settings);
  std::shared_ptr<std::atomic<bool>> m_cancel;
  QPointer<ProcessModel> m_model;
  const char* m_reportedError{};
};
using ComponentFactory = ::Execution::ProcessComponentFactory_T<Component>;
}
}
