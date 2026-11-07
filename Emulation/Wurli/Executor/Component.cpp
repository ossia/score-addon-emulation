// SPDX-License-Identifier: GPL-3.0-or-later
#include "Component.hpp"
#include "Node.hpp"
#include <Emulation/ExecutionProcess.hpp>
#include <Emulation/Wurli/ProcessModel.hpp>
#include <Process/Dataflow/WidgetInlets.hpp>
#include <Execution/DocumentPlugin.hpp>
#include <score/tools/ThreadPool.hpp>
#include <core/document/Document.hpp>
#include <ossia/dataflow/execution_state.hpp>
#include <QCoreApplication>
#include <QTimer>
#include <exception>
namespace Emulation::Wurli::Executor
{
Component::Component(ProcessModel& proc, const ::Execution::Context& ctx, QObject* parent)
  : ::Execution::ProcessComponent_T<ProcessModel, ossia::node_process>{proc, ctx, "WurliComponent", parent}
  , m_model{&proc}
{
  auto runtime = ossia::make_node<Node>(*ctx.execState, *ctx.execState, proc.settings());
  node = runtime;
  m_ossia_process = std::make_shared<Emulation::ExecutionProcess>(runtime);
  for(int c = 0; c < ControlCount; ++c)
    static_cast<Process::ControlInlet*>(proc.inlets()[c + 1])->setupExecution(*runtime->controls[c], this);
  connect(&proc, &ProcessModel::settingsChanged, this, [this, runtime] {
    const auto settings = process().settings();
    in_exec([runtime, settings] { runtime->update(settings); });
  });
  connect(&ctx.doc.coarseUpdateTimer, &QTimer::timeout, this, [this, runtime] {
    if(!m_model) return;
    if(const auto* error = runtime->error(); error != m_reportedError) {
      m_reportedError = error;
      process().setEngineError(error ? QString::fromUtf8(error) : QString{});
    }
  });
  prepare(proc.settings());
}
Component::~Component() { if(m_cancel) m_cancel->store(true, std::memory_order_relaxed); }
void Component::prepare(Settings settings)
{
  if(!m_model) return;
  auto cancelled = m_cancel = std::make_shared<std::atomic<bool>>(false);
  const double rate = system().execState->sampleRate;
  QPointer<Component> self{this};
  score::TaskPool::instance().post([self, cancelled, settings, rate] {
    std::shared_ptr<Engine> prepared;
    QString error;
    try {
      if(cancelled->load(std::memory_order_relaxed)) return;
      prepared = std::make_shared<Engine>(rate, settings);
    } catch(const std::exception& e) { error = QString::fromUtf8(e.what()); }
    if(cancelled->load(std::memory_order_relaxed)) return;
    QMetaObject::invokeMethod(qApp, [self, cancelled, prepared = std::move(prepared), error]() mutable {
      if(cancelled->load(std::memory_order_relaxed) || !self || !self->m_model) return;
      self->process().setPreparationError(error);
      if(!prepared) return;
      auto runtime = std::static_pointer_cast<Node>(self->node);
      self->system().executionQueue.enqueue([runtime, prepared = std::move(prepared)]() mutable {
        runtime->install(prepared);
      });
    }, Qt::QueuedConnection);
  });
}
}
