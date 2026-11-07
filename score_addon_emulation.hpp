// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <score/application/ApplicationContext.hpp>
#include <score/plugins/Interface.hpp>
#include <score/plugins/qt_interfaces/FactoryInterface_QtInterface.hpp>

#include <vector>
#include <verdigris>

class score_addon_emulation final
    : public score::Plugin_QtInterface
    , public score::FactoryInterface_QtInterface
{
  SCORE_PLUGIN_METADATA(1, "84a2d329-b4c6-413d-9c89-5f17031baed4")
public:
  score_addon_emulation();
  ~score_addon_emulation() override;

private:
  std::vector<score::InterfaceBase*> factories(
      const score::ApplicationContext&, const score::InterfaceKey&) const override;
};
