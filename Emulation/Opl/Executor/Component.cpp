// SPDX-License-Identifier: GPL-3.0-or-later
#include "Component.hpp"
#include "Node.hpp"
#include <Emulation/Opl/ProcessModel.hpp>
#include <Emulation/ExecutionProcess.hpp>
#include <Execution/DocumentPlugin.hpp>
#include <score/tools/ThreadPool.hpp>
#include <core/document/Document.hpp>
#include <ossia/dataflow/execution_state.hpp>
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>

namespace Emulation::Opl::Executor
{
Component::Component(ProcessModel& proc, const ::Execution::Context& ctx, QObject* parent)
    : ::Execution::ProcessComponent_T<ProcessModel, ossia::node_process>{proc, ctx, "OplComponent", parent}
    , m_model{&proc}
{
  auto runtime = ossia::make_node<Node>(*ctx.execState, *ctx.execState);
  node = runtime;
  m_ossia_process = std::make_shared<Emulation::ExecutionProcess>(runtime);
  for(int c = 0; c < ControlCount; ++c)
    if(auto* control = qobject_cast<Process::ControlInlet*>(proc.inlets()[c + 1]))
      control->setupExecution(*runtime->controls[c], this);
  auto& volume = *static_cast<Process::ControlInlet*>(proc.inlets()[Volume + 1]);
  runtime->setVolume(ossia::convert<double>(volume.value()));
  connect(&volume, &Process::ControlInlet::valueChanged, this,
      [this, runtime](const ossia::value& value) {
    const double gain = ossia::convert<double>(value);
    in_exec([runtime, gain] { runtime->setVolume(gain); });
  });
  auto& allocation = *static_cast<Process::ControlInlet*>(proc.inlets()[VoiceAllocation + 1]);
  runtime->setAllocation(ossia::convert<int>(allocation.value()));
  connect(&allocation, &Process::ControlInlet::valueChanged, this,
      [this, runtime](const ossia::value& value) {
    const int mode = ossia::convert<int>(value);
    in_exec([runtime, mode] { runtime->setAllocation(mode); });
  });

  connect(&proc, &ProcessModel::bankChanged, this, [this] {
    prepare(process().settings());
  });
  connect(&proc, &ProcessModel::instrumentChanged, this, [this, runtime] {
    if(!runtime->ready()) { prepare(process().settings()); return; }
    const auto bank = process().bank();
    const auto address = process().selectedAddress();
    if(bank)
      if(const auto* patch = findProgram(*bank, address))
        in_exec([runtime, address, instrument = patch->instrument] {
          runtime->edit(address, instrument);
        });
  });
  connect(&proc, &ProcessModel::selectedAddressChanged, this, [this, runtime] {
    const auto address = process().selectedAddress();
    in_exec([runtime, address] { runtime->select(address); });
  });
  connect(&proc, &ProcessModel::depthsChanged, this, [this, runtime] {
    const auto settings = process().settings();
    in_exec([runtime, settings] {
      runtime->depths(settings.deepVibrato, settings.deepTremolo);
    });
  });
  connect(&ctx.doc.coarseUpdateTimer, &QTimer::timeout, this, [this, runtime] {
    if(!m_model) return;
    Settings requested;
    if(runtime->takeConfiguration(requested)) prepare(requested);
    const auto* error = runtime->error();
    if(error != m_reportedError)
    {
      m_reportedError = error;
      process().setRuntimeError(error ? QString::fromUtf8(error) : QString{});
    }
  });
  prepare(proc.settings());
}
Component::~Component()
{
  if(m_cancel) m_cancel->store(true, std::memory_order_relaxed);
}
void Component::prepare(Settings settings)
{
  if(!m_model) return;
  if(m_cancel) m_cancel->store(true, std::memory_order_relaxed);
  auto cancelled = m_cancel = std::make_shared<std::atomic<bool>>(false);
  auto runtime = std::static_pointer_cast<Node>(node);
  in_exec([runtime] { runtime->suspend(); });
  auto bank = process().bank();
  if(!bank) return; // The model reports loading/invalid import without fallback.
  const double rate = system().execState->sampleRate;
  QPointer<Component> self{this};
  process().setEngineError(QStringLiteral("Preparing OPL engine"));
  score::TaskPool::instance().post([self, cancelled, bank = std::move(bank), settings, rate]() mutable {
    std::shared_ptr<Engine> prepared;
    QString error;
    try
    {
      if(cancelled->load(std::memory_order_relaxed)) return;
      prepared = std::make_shared<Engine>(rate, settings, *bank);
    }
    catch(const std::exception& e) { error = QString::fromUtf8(e.what()); }
    if(cancelled->load(std::memory_order_relaxed)) return;
    QMetaObject::invokeMethod(qApp,
        [self, cancelled, bank = std::move(bank), prepared = std::move(prepared), settings, error]() mutable {
      if(cancelled->load(std::memory_order_relaxed) || !self || !self->m_model) return;
      self->process().setEngineError(error);
      self->process().setRuntimeError({});
      self->m_reportedError = nullptr;
      auto runtime = std::static_pointer_cast<Node>(self->node);
      const auto selected = self->process().selectedAddress();
      // Edits to depth controls need not restart a pending worker.
      const auto current = self->process().settings();
      settings.deepVibrato = current.deepVibrato;
      settings.deepTremolo = current.deepTremolo;
      if(prepared) prepared->setDepths(settings.deepVibrato, settings.deepTremolo);
      self->system().executionQueue.enqueue([runtime, bank = std::move(bank), prepared = std::move(prepared), settings, selected]() mutable {
        runtime->install(prepared, bank, settings, selected);
      });
    }, Qt::QueuedConnection);
  });
}
}
