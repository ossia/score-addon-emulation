#pragma once
#include <Process/GenericProcessFactory.hpp>
#include <Process/ProcessFactory.hpp>
#include <Emulation/Wavetable/ProcessModel.hpp>
namespace Emulation::Wavetable {
class ProcessFactory final : public Process::ProcessFactory_T<ProcessModel> {
public: ~ProcessFactory() override;
};
}
