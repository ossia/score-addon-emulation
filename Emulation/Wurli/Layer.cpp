// SPDX-License-Identifier: GPL-3.0-or-later
#include "Layer.hpp"
#include "Controls.hpp"
#include "ProcessModel.hpp"
#include <Process/Dataflow/PortFactory.hpp>
#include <Control/Layout.hpp>
#include <score/application/GUIApplicationContext.hpp>
#include <score/graphics/layouts/GraphicsBoxLayout.hpp>
#include <score/model/Skin.hpp>
namespace Emulation::Wurli
{
namespace
{
struct UiBuilder final : Process::LayoutBuilderBase
{
  template<typename L> L* start(QGraphicsItem* parent) {
    auto* layout = new L{parent}; layout->setPadding(5.);
    createdLayouts.push_back(layout); return layout;
  }
  void control(QGraphicsItem* parent, int c) {
    Process::ControlPresentation presentation;
    presentation.size = score::ControlSize::Small;
    const char* labels[] = {"Volume", "Amplifier", "Tremolo", "Speaker", "Model correction", "Velocity", "Voice allocation"};
    presentation.label = QString::fromLatin1(labels[c]);
    if(c <= Speaker) presentation.widget = Process::ControlPresentation::Widget::Knob;
    auto item = makePort(*inlets[c + 1], presentation);
    if(item.container) item.container->setParentItem(parent);
  }
};
class LayerItem final : public score::EmptyRectItem
{
public:
  LayerItem(const ProcessModel& model, const Process::Context& ctx, QGraphicsItem* parent)
    : score::EmptyRectItem{parent}
  {
    UiBuilder b{*this, model, ctx, ctx.app.interfaces<Process::PortFactoryList>(), model.inlets(), model.outlets()};
    b.marginOnNestedLayouts = false;
    auto& skin = score::Skin::instance();
    auto* main = b.start<score::GraphicsVBoxLayout>(nullptr);
    main->setBrush(skin.Background2.darker);
    auto* header = b.start<score::GraphicsHBoxLayout>(main);
    header->setBrush(skin.Background2.darker300);
    b.control(header, Volume);
    b.control(header, VoiceAllocation);
    auto* timbre = b.start<score::GraphicsHBoxLayout>(main);
    for(int c : {Amplifier, Tremolo, Speaker}) b.control(timbre, c);
    auto* performance = b.start<score::GraphicsHBoxLayout>(main);
    b.control(performance, VelocityCurve); b.control(performance, Correction);
    b.finalizeLayout(this); fitChildrenRect();
  }
};
}
LayerFactory::~LayerFactory() = default;
UuidKey<Process::ProcessModel> LayerFactory::concreteKey() const noexcept
{ return Metadata<ConcreteKey_k, ProcessModel>::get(); }
bool LayerFactory::matches(const UuidKey<Process::ProcessModel>& key) const { return key == concreteKey(); }
std::optional<double> LayerFactory::recommendedHeight() const noexcept { return 180.; }
score::ResizeableItem* LayerFactory::makeItem(const Process::ProcessModel& proc,
    const Process::Context& ctx, QGraphicsItem* parent) const
{ return new LayerItem{static_cast<const ProcessModel&>(proc), ctx, parent}; }
}
