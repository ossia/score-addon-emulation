#pragma once
#include <Process/GenericProcessFactory.hpp>
#include <Process/ProcessFactory.hpp>
#include <Emulation/Opl/ProcessModel.hpp>
namespace Emulation::Opl
{
class ProcessFactory final : public Process::ProcessFactory_T<ProcessModel>
{
public:
  ~ProcessFactory() override;
};
}
