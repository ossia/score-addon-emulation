#pragma once
#include <Process/GenericProcessFactory.hpp>
#include <Process/ProcessFactory.hpp>
#include <Emulation/Psg/ProcessModel.hpp>
namespace Emulation::Psg {
class ProcessFactory final : public Process::ProcessFactory_T<ProcessModel> {
public: ~ProcessFactory() override;
};
}
