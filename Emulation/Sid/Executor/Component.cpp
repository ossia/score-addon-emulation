// SPDX-License-Identifier: GPL-3.0-or-later
#include "Component.hpp"
#include "Node.hpp"
#include <Emulation/ExecutionProcess.hpp>
#include <Emulation/Sid/ProcessModel.hpp>
#include <Process/Dataflow/WidgetInlets.hpp>
#include <Execution/DocumentPlugin.hpp>
#include <score/tools/ThreadPool.hpp>
#include <core/document/Document.hpp>
#include <ossia/dataflow/execution_state.hpp>
#include <QCoreApplication>
#include <QTimer>
namespace Emulation::Sid::Executor
{
Component::Component(ProcessModel& proc, const ::Execution::Context& ctx, QObject* parent)
    : ::Execution::ProcessComponent_T<ProcessModel, ossia::node_process>{proc, ctx, "SidComponent", parent}
    , m_model{&proc}
{
  auto runtime = ossia::make_node<Node>(*ctx.execState, *ctx.execState);
  node = runtime;
  m_ossia_process = std::make_shared<Emulation::ExecutionProcess>(runtime);
  for(int c = 0; c < ControlCount; ++c)
    if(auto* control = qobject_cast<Process::ControlInlet*>(proc.inlets()[c + 1]))
      control->setupExecution(*runtime->controls[c], this);
  auto patch = std::make_shared<const Patch>(proc.patch());
  in_exec([runtime, patch] { runtime->edit(*patch); });
  connect(&proc, &ProcessModel::patchChanged, this, [this, runtime] {
    auto patch = std::make_shared<const Patch>(process().patch());
    in_exec([runtime, patch] { runtime->edit(*patch); });
    if(!sameSetup(*patch, m_preparing)) prepare(std::move(patch));
  });
  connect(&ctx.doc.coarseUpdateTimer, &QTimer::timeout, this, [this, runtime] {
    if(!m_model) return;
    auto requested = process().patch();
    if(runtime->takeConfiguration(requested))
      prepare(std::make_shared<const Patch>(requested));
    const auto* error = runtime->error();
    if(error != m_reportedError)
    {
      m_reportedError = error;
      process().setEngineError(error ? QString::fromUtf8(error) : QString{});
    }
  });
  prepare(std::move(patch));
}
Component::~Component() { if(m_cancel) m_cancel->store(true, std::memory_order_relaxed); }
void Component::prepare(std::shared_ptr<const Patch> patch)
{
  if(!m_model) return;
  if(m_cancel) m_cancel->store(true, std::memory_order_relaxed);
  auto cancelled = m_cancel = std::make_shared<std::atomic<bool>>(false);
  m_preparing = *patch;
  auto runtime = std::static_pointer_cast<Node>(node);
  in_exec([runtime] { runtime->suspend(); });
  const double rate = system().execState->sampleRate;
  QPointer<Component> self{this};
  score::TaskPool::instance().post([self, cancelled, patch = std::move(patch), rate]() mutable {
    std::shared_ptr<Engine> prepared;
    QString error;
    try
    {
      if(cancelled->load(std::memory_order_relaxed)) return;
      prepared = std::make_shared<Engine>(rate, *patch);
    }
    catch(const std::exception& e) { error = QString::fromUtf8(e.what()); }
    if(cancelled->load(std::memory_order_relaxed)) return;
    QMetaObject::invokeMethod(qApp, [self, cancelled, prepared = std::move(prepared), error]() mutable {
      if(cancelled->load(std::memory_order_relaxed) || !self || !self->m_model) return;
      self->process().setPreparationError(error);
      auto runtime = std::static_pointer_cast<Node>(self->node);
      self->system().executionQueue.enqueue([runtime, prepared = std::move(prepared)]() mutable {
        runtime->install(prepared);
      });
    }, Qt::QueuedConnection);
  });
}
}
