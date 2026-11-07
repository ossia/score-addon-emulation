// SPDX-License-Identifier: GPL-3.0-or-later
#include "Component.hpp"
#include "Node.hpp"
#include <Emulation/ExecutionProcess.hpp>
#include <Emulation/Apu/ProcessModel.hpp>
#include <Process/Dataflow/Port.hpp>
#include <Execution/DocumentPlugin.hpp>
#include <core/document/Document.hpp>
#include <ossia/dataflow/execution_state.hpp>
#include <QTimer>
#include <exception>
namespace Emulation::Apu::Executor {
Component::Component(ProcessModel& proc,const ::Execution::Context& ctx,QObject* parent)
  : ::Execution::ProcessComponent_T<ProcessModel,ossia::node_process>{proc,ctx,"ApuComponent",parent} {
  auto runtime=ossia::make_node<Node>(*ctx.execState,*ctx.execState,proc.settings());node=runtime;
  m_ossia_process=std::make_shared<Emulation::ExecutionProcess>(runtime);
  for(int c=0;c<ControlCount;++c)
    static_cast<Process::ControlInlet*>(proc.inlets()[c+1])->setupExecution(*runtime->controls[c],this);
  connect(&proc,&ProcessModel::settingsChanged,this,[this,runtime] {
    auto settings=std::make_shared<const Settings>(process().settings());
    in_exec([runtime,settings]{runtime->modelChanged(*settings);});
  });
  connect(&proc,&ProcessModel::assetChanged,this,[this,runtime] {
    const auto revision=++m_revision;
    in_exec([runtime,revision]{runtime->assetRevision(revision);});
    prepare(process().settings());
  });
  connect(&ctx.doc.coarseUpdateTimer,&QTimer::timeout,this,[this,runtime] {
    Model model;int clock{};
    if(runtime->takeConfiguration(model,clock)) {
      // Only architecture and clock are needed for allocation; install applies
      // the complete current audio-thread settings, including automation.
      Settings settings;settings.model=model;settings.clock=clock;prepare(settings);
    }
    std::array<float,32> wave;
    if(runtime->receivedWave().take(m_seenWave,wave))
      static_cast<Process::ControlInlet*>(process().inlets()[Wave+1])
          ->setExecutionValue(Waveform::fromSamples(wave,0.,15.));
    const auto* error=runtime->error();
    process().setEngineError(!m_preparationError.isEmpty()?m_preparationError:
      error?QString::fromUtf8(error):QString{});
  });
  prepare(proc.settings());
}
Component::~Component()=default;
void Component::prepare(Settings settings) {
  // Engine construction and native core preparation occur on the control thread.
  // The execution queue returns the retired shared_ptr to the control-thread GC.
  std::shared_ptr<Engine> prepared;
  try {
    prepared=std::make_shared<Engine>();
    if(!prepared->prepare(system().execState->sampleRate,settings,process().dmc())) {
      m_preparationError=QString::fromUtf8(prepared->error());
      process().setEngineError(m_preparationError);return;
    }
  } catch(const std::exception& e) {
    m_preparationError=QString::fromUtf8(e.what());process().setEngineError(m_preparationError);return;
  }
  m_preparationError.clear();
  auto runtime=std::static_pointer_cast<Node>(node);
  in_exec([runtime,prepared=std::move(prepared),model=settings.model,clock=settings.clock,revision=m_revision]() mutable {
    runtime->install(prepared,model,clock,revision);
  });
}
}
