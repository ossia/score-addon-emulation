// SPDX-License-Identifier: GPL-3.0-or-later
#include "Component.hpp"
#include "Node.hpp"
#include <Emulation/La/ProcessModel.hpp>
#include <Emulation/ExecutionProcess.hpp>
#include <Process/Dataflow/WidgetInlets.hpp>
#include <Execution/DocumentPlugin.hpp>
#include <score/tools/ThreadPool.hpp>
#include <core/document/Document.hpp>
#include <ossia/dataflow/execution_state.hpp>
#include <QCoreApplication>
#include <QTimer>
#include <utility>
namespace Emulation::La::Executor
{
Component::Component(ProcessModel& proc, const ::Execution::Context& ctx, QObject* parent)
    : ::Execution::ProcessComponent_T<ProcessModel, ossia::node_process>{proc, ctx, "LaComponent", parent}
    , m_model{&proc}
{
  auto runtime = ossia::make_node<Node>(*ctx.execState, *ctx.execState);
  node = runtime;
  m_ossia_process = std::make_shared<Emulation::ExecutionProcess>(runtime);
  for(int c = Volume; c < ControlCount; ++c)
    if(auto* control = qobject_cast<Process::ControlInlet*>(proc.inlets()[c + 1]))
      control->setupExecution(*runtime->controls[c], this);
  auto patch = m_snapshot = std::make_shared<const Patch>(proc.patch());
  in_exec([runtime, patch] { runtime->edit(*patch); });
  connect(&proc, &ProcessModel::patchChanged, this, [this, runtime] {
    auto patch = std::make_shared<const Patch>(process().patch());
    auto retired = std::exchange(m_snapshot, patch);
    in_exec([runtime, patch, retired] { runtime->edit(*patch); (void)retired; });
  });
  // Only intentional resource writes (including same-path reselection) retry
  // preparation. Ordinary sound edits must never reopen failed ROM files.
  connect(&proc, &ProcessModel::romPreparationRequested, this, [this] {
    prepare(std::make_shared<const Patch>(process().patch()));
  });
  connect(&ctx.doc.coarseUpdateTimer, &QTimer::timeout, this, [this, runtime] {
    if(!m_model) return;
    const auto* error = runtime->error();
    if(error != m_reportedError)
    {
      m_reportedError = error;
      process().setRuntimeError(error ? QString::fromUtf8(error) : QString{});
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
      prepared = std::make_shared<Engine>(rate, *patch, cancelled.get());
    }
    catch(const std::exception& e) { error = QString::fromUtf8(e.what()); }
    if(cancelled->load(std::memory_order_relaxed)) return;
    QMetaObject::invokeMethod(qApp, [self, cancelled, prepared = std::move(prepared), error]() mutable {
      if(cancelled->load(std::memory_order_relaxed) || !self || !self->m_model) return;
      self->process().setEngineError(error); self->m_reportedError = nullptr;
      self->process().setRuntimeError({});
      if(prepared)
      {
        self->process().acceptROMIdentity(prepared->resolvedROMs(),
            QString::fromStdString(prepared->machine()), prepared->programCatalog());
      }
      auto patch = std::make_shared<const Patch>(self->process().patch());
      auto retired = std::exchange(self->m_snapshot, patch);
      auto runtime = std::static_pointer_cast<Node>(self->node);
      self->system().executionQueue.enqueue([runtime, patch, retired, prepared = std::move(prepared)]() mutable {
        runtime->edit(*patch); runtime->install(prepared); (void)retired;
      });
    }, Qt::QueuedConnection);
  });
}
}
