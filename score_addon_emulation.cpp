// SPDX-License-Identifier: GPL-3.0-or-later
#include "score_addon_emulation.hpp"
#include <Emulation/Opl/Executor/Component.hpp>
#include <Emulation/Opl/Layer.hpp>
#include <Emulation/Opl/Library.hpp>
#include <Emulation/Opl/ProcessFactory.hpp>
#include <Emulation/Psg/Executor/Component.hpp>
#include <Emulation/Psg/Layer.hpp>
#include <Emulation/Psg/ProcessFactory.hpp>
#include <Emulation/Sid/Executor/Component.hpp>
#include <Emulation/Sid/Layer.hpp>
#include <Emulation/Sid/ProcessFactory.hpp>
#include <Emulation/La/Executor/Component.hpp>
#include <Emulation/La/Layer.hpp>
#include <Emulation/La/ProcessFactory.hpp>
#include <Emulation/Wurli/Executor/Component.hpp>
#include <Emulation/Wurli/Layer.hpp>
#include <Emulation/Wurli/ProcessFactory.hpp>
#include <Emulation/Wavetable/Executor/Component.hpp>
#include <Emulation/Wavetable/Layer.hpp>
#include <Emulation/Wavetable/ProcessFactory.hpp>
#include <Emulation/Apu/Executor/Component.hpp>
#include <Emulation/Apu/Layer.hpp>
#include <Emulation/Apu/ProcessFactory.hpp>
#include <score/plugins/FactorySetup.hpp>
#include <score/plugins/PluginInstances.hpp>

score_addon_emulation::score_addon_emulation() = default;
score_addon_emulation::~score_addon_emulation() = default;

std::vector<score::InterfaceBase*> score_addon_emulation::factories(
    const score::ApplicationContext& ctx, const score::InterfaceKey& key) const
{
  return instantiate_factories<
      score::ApplicationContext,
      FW<Process::ProcessModelFactory, Emulation::Opl::ProcessFactory,
         Emulation::Psg::ProcessFactory, Emulation::Sid::ProcessFactory,
         Emulation::La::ProcessFactory, Emulation::Wurli::ProcessFactory,
         Emulation::Wavetable::ProcessFactory, Emulation::Apu::ProcessFactory>,
      FW<Process::LayerFactory, Emulation::Opl::LayerFactory,
         Emulation::Psg::LayerFactory, Emulation::Sid::LayerFactory,
         Emulation::La::LayerFactory, Emulation::Wurli::LayerFactory,
         Emulation::Wavetable::LayerFactory, Emulation::Apu::LayerFactory>,
      FW<Library::LibraryInterface, Emulation::Opl::LibraryHandler>,
      FW<Process::ProcessDropHandler, Emulation::Opl::DropHandler>,
      FW<Execution::ProcessComponentFactory, Emulation::Opl::Executor::ComponentFactory,
         Emulation::Psg::Executor::ComponentFactory, Emulation::Sid::Executor::ComponentFactory,
         Emulation::La::Executor::ComponentFactory, Emulation::Wurli::Executor::ComponentFactory,
         Emulation::Wavetable::Executor::ComponentFactory, Emulation::Apu::Executor::ComponentFactory>>(
      ctx, key);
}
SCORE_EXPORT_PLUGIN(score_addon_emulation)
