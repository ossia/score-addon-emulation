#pragma once
#include <Process/GenericProcessFactory.hpp>
#include <Process/ProcessFactory.hpp>
#include <Emulation/Wurli/ProcessModel.hpp>
namespace Emulation::Wurli {
class ProcessFactory final : public Process::ProcessFactory_T<ProcessModel> {
public: ~ProcessFactory() override;
};
}
