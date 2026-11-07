// SPDX-License-Identifier: GPL-3.0-or-later
#include "Component.hpp"
#include "Node.hpp"
#include <Emulation/ExecutionProcess.hpp>
#include <Emulation/Wavetable/ProcessModel.hpp>
#include <Process/Dataflow/Port.hpp>
#include <Execution/DocumentPlugin.hpp>
#include <core/document/Document.hpp>
#include <ossia/dataflow/execution_state.hpp>
#include <QTimer>
#include <exception>
namespace Emulation::Wavetable::Executor
{
Component::Component(ProcessModel& proc, const ::Execution::Context& ctx, QObject* parent)
  : ::Execution::ProcessComponent_T<ProcessModel, ossia::node_process>{proc, ctx, "WavetableComponent", parent}
{
  auto runtime = ossia::make_node<Node>(*ctx.execState, *ctx.execState, proc.settings(), proc.levelMaxima());
  node = runtime;
  m_ossia_process = std::make_shared<Emulation::ExecutionProcess>(runtime);
  for(int c = 0; c < ControlCount; ++c)
    static_cast<Process::ControlInlet*>(proc.inlets()[c + 1])->setupExecution(*runtime->controls[c], this);
  connect(&proc, &ProcessModel::settingsChanged, this, [this, runtime] {
    auto settings = std::make_shared<const Settings>(process().settings());
    in_exec([runtime, settings] { runtime->modelChanged(*settings); });
  });
  connect(&ctx.doc.coarseUpdateTimer, &QTimer::timeout, this, [this, runtime] {
    Model model; int clock{};
    if(runtime->takeConfiguration(model, clock)) {
      auto settings = process().settings(); settings.model = model; settings.clock = clock;
      prepare(settings);
    }
    for(int ch = 0; ch < 6; ++ch) {
      std::array<float, 32> wave;
      if(runtime->receivedWave(ch).take(m_seenWaves[ch], wave))
        static_cast<Process::ControlInlet*>(process().inlets()[channelControl(ch, Wave) + 1])
            ->setExecutionValue(Waveform::fromSamples(wave, 0., 1.));
    }
    if(const auto* error = runtime->error(); error != m_reportedError) {
      m_reportedError = error;
      process().setEngineError(error ? QString::fromUtf8(error) : QString{});
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
  catch(const std::exception& e) { process().setEngineError(QString::fromUtf8(e.what())); return; }
  auto runtime = std::static_pointer_cast<Node>(node);
  in_exec([runtime, prepared = std::move(prepared), model = settings.model, clock = settings.clock]() mutable {
    runtime->install(prepared, model, clock);
  });
}
}
