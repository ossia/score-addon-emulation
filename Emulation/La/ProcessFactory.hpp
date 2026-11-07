// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Process/GenericProcessFactory.hpp>
#include <Process/ProcessFactory.hpp>
#include "ProcessModel.hpp"
namespace Emulation::La
{
class ProcessFactory final : public Process::ProcessFactory_T<ProcessModel>
{
public:
  ~ProcessFactory() override;
};
}
