#pragma once
#include <Effect/EffectFactory.hpp>
namespace Emulation::Opl
{
class LayerFactory final : public Process::EffectLayerFactory_Base
{
public:
  ~LayerFactory() override;
private:
  UuidKey<Process::ProcessModel> concreteKey() const noexcept override;
  bool matches(const UuidKey<Process::ProcessModel>&) const override;
  std::optional<double> recommendedHeight() const noexcept override;
  score::ResizeableItem* makeItem(const Process::ProcessModel&,
      const Process::Context&, QGraphicsItem*) const override;
};
}
