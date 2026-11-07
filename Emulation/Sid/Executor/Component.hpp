// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Emulation/Sid/Engine.hpp>
#include <Process/Execution/ProcessComponent.hpp>
#include <ossia/dataflow/node_process.hpp>
#include <QPointer>
#include <atomic>
#include <memory>
namespace Emulation::Sid
{
class ProcessModel;
namespace Executor
{
class Component final : public ::Execution::ProcessComponent_T<ProcessModel, ossia::node_process>
{
  COMPONENT_METADATA("a8420adc-e366-47ab-9386-ed546e9abdce")
public:
  Component(ProcessModel&, const ::Execution::Context&, QObject*);
  ~Component() override;
private:
  void prepare(std::shared_ptr<const Patch>);
  Patch m_preparing;
  std::shared_ptr<std::atomic<bool>> m_cancel;
  QPointer<ProcessModel> m_model;
  const char* m_reportedError{};
};
using ComponentFactory = ::Execution::ProcessComponentFactory_T<Component>;
}
}
