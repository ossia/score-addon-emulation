#pragma once
#include <Process/GenericProcessFactory.hpp>
#include <Process/ProcessFactory.hpp>
#include "ProcessModel.hpp"
namespace Emulation::Apu {
class ProcessFactory final : public Process::ProcessFactory_T<ProcessModel> {
public: ~ProcessFactory() override;
};
}
