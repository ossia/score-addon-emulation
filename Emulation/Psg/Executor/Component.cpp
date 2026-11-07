// SPDX-License-Identifier: GPL-3.0-or-later
#include "Component.hpp"
#include "Node.hpp"
#include <Emulation/ExecutionProcess.hpp>
#include <Emulation/Psg/ProcessModel.hpp>
#include <Process/Dataflow/Port.hpp>
#include <Execution/DocumentPlugin.hpp>
#include <core/document/Document.hpp>
#include <ossia/dataflow/execution_state.hpp>
#include <QTimer>
#include <exception>
namespace Emulation::Psg::Executor
{
Component::Component(ProcessModel& proc, const ::Execution::Context& ctx, QObject* parent)
  : ::Execution::ProcessComponent_T<ProcessModel, ossia::node_process>{proc, ctx, "PsgComponent", parent}
{
  auto runtime = ossia::make_node<Node>(*ctx.execState, *ctx.execState, proc.settings());
  node = runtime;
  m_ossia_process = std::make_shared<Emulation::ExecutionProcess>(runtime);
  for(int c = 0; c < ControlCount; ++c)
    static_cast<Process::ControlInlet*>(proc.inlets()[c + 1])->setupExecution(*runtime->controls[c], this);
  connect(&proc, &ProcessModel::settingsChanged, this, [this, runtime] {
    auto settings = std::make_shared<const Settings>(process().settings());
    in_exec([runtime, settings] { runtime->update(*settings); });
  });
  connect(&ctx.doc.coarseUpdateTimer, &QTimer::timeout, this, [this, runtime] {
    Model model; int clock{};
    if(runtime->takeConfiguration(model, clock)) {
      auto settings = process().settings(); settings.model = model; settings.clock = clock;
      prepare(settings);
    }
    const auto* runtimeError = runtime->error();
    auto error = m_preparationError.isEmpty()
        ? (runtimeError ? QString::fromUtf8(runtimeError) : QString{}) : m_preparationError;
    if(error != m_reportedError) {
      m_reportedError = error;
      process().setEngineError(error);
    }
  });
  prepare(proc.settings());
}
Component::~Component() = default;
void Component::prepare(Settings settings)
{
  // Both cores have bounded, small initialization, with no disk I/O or ROM.
  // Allocate / initialize on the control thread, never the audio thread.
  std::shared_ptr<Engine> prepared;
  try { prepared = std::make_shared<Engine>(system().execState->sampleRate, settings); }
  catch(const std::exception& e) {
    m_preparationError = QString::fromUtf8(e.what());
    process().setEngineError(m_preparationError); return;
  }
  m_preparationError.clear();
  m_reportedError.clear();
  process().setEngineError({});
  auto runtime = std::static_pointer_cast<Node>(node);
  in_exec([runtime, prepared = std::move(prepared), model = settings.model, clock = settings.clock]() mutable {
    runtime->install(prepared, model, clock);
  });
}
}
