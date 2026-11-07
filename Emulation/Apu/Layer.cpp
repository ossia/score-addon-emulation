// SPDX-License-Identifier: GPL-3.0-or-later
#include "Layer.hpp"
#include "Controls.hpp"
#include "ProcessModel.hpp"
#include <Dataflow/WaveformInlet.hpp>
#include <Process/Dataflow/PortItem.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <Control/Layout.hpp>
#include <score/application/GUIApplicationContext.hpp>
#include <score/graphics/layouts/GraphicsBoxLayout.hpp>
#include <score/graphics/layouts/GraphicsGridLayout.hpp>
#include <score/graphics/layouts/GraphicsTabLayout.hpp>
#include <score/model/Skin.hpp>
#include <algorithm>
namespace Emulation::Apu {
namespace {
struct UiBuilder final : Process::LayoutBuilderBase {
  template<typename L> L* start(QGraphicsItem* parent) {
    auto* layout=new L{parent};layout->setPadding(3.);createdLayouts.push_back(layout);return layout;
  }
  QGraphicsItem* control(QGraphicsItem* parent,int c,Model model) {
    Process::ControlPresentation p;p.label=controlLabel(model,c);
    if(c!=DmcFile&&c!=Wave)p.size=score::ControlSize::Small;
    if(c==Volume)p.widget=Process::ControlPresentation::Widget::Knob;
    auto item=makePort(*inlets[c+1],p);if(item.container) {
      if(c==Wave&&item.control) {
        Dataflow::setWaveformItemRect(*item.control,{0.,0.,430.,145.});
        if(const auto* factory=portFactory.get(inlets[c+1]->concreteKey()))
          factory->layoutFullItem(item,*inlets[c+1],p);
      }
      item.container->setParentItem(parent);
      item.container->setToolTip(inlets[c+1]->description());
    }
    return item.container;
  }
};
class LayerItem final : public score::EmptyRectItem {
public:
  LayerItem(const ProcessModel& model,const Process::Context& ctx,QGraphicsItem* parent)
    : score::EmptyRectItem{parent},m_model{model},m_context{ctx} {
    UiBuilder b{*this,model,ctx,ctx.app.interfaces<Process::PortFactoryList>(),model.inlets(),model.outlets()};
    b.marginOnNestedLayouts=false;auto& skin=score::Skin::instance();
    m_hidden=new score::EmptyRectItem{this};m_hidden->hide();
    m_main=b.start<score::GraphicsVBoxLayout>(nullptr);m_main->setBrush(skin.Background2.darker);
    auto* header=m_header=b.start<score::GraphicsHBoxLayout>(m_main);header->setBrush(skin.Background2.darker300);
    b.control(header,ChipModel,model.settings().model);
    m_sound=b.control(header,Sound,model.settings().model);m_sound->setZValue(1.);
    b.control(header,Volume,model.settings().model)->setZValue(2.);
    b.finalizeLayout(this);
    rebuild();
    refreshRouting();
    connect(&model,&ProcessModel::settingsChanged,this,[this] {
      if(m_model.settings().model!=m_displayModel)rebuild();
      refreshRouting();
    });
  }
private:
  void refreshRouting() {
    const bool instrument=m_model.settings().routing==RoutingMode::Instrument;
    auto* parent=instrument?static_cast<QGraphicsItem*>(m_header):m_hidden;
    if(m_sound->parentItem()==parent)return;
    m_sound->setParentItem(parent);m_sound->setVisible(instrument);
    for(auto* layout:std::array<score::GraphicsLayout*,2>{m_header,m_main}) {
      layout->setRect({});layout->layout();const auto bounds=layout->childrenBoundingRect();
      layout->setRect({0.,0.,bounds.right()+4.,bounds.bottom()+4.});
    }
    setRect(m_main->mapRectToParent(m_main->boundingRect()));
  }
  void rebuild() {
    const auto model=m_model.settings().model;
    const int roles=gameBoy(model)?4:5;
    int selected=m_tabs?m_tabs->currentIndex():0;
    const bool setup=m_tabs&&selected==(gameBoy(m_displayModel)?4:5);
    delete m_bodyContext;delete m_tabs;
    m_bodyContext=new QObject{this};m_displayModel=model;
    UiBuilder b{*m_bodyContext,m_model,m_context,m_context.app.interfaces<Process::PortFactoryList>(),m_model.inlets(),m_model.outlets()};
    b.marginOnNestedLayouts=false;b.createdLayouts.push_back(m_main);
    b.createdLayouts.push_back(m_header);
    m_main->setRect({});m_header->setRect({});
    m_tabs=b.start<score::GraphicsTabLayout>(m_main);m_tabs->setBrush(score::Skin::instance().Background2.darker300);
    for(int role=0;role<roles;++role) {
      const QString name=role==0?QStringLiteral("Pulse 1"):role==1?QStringLiteral("Pulse 2"):
        role==2?(gameBoy(model)?QStringLiteral("Wave"):QStringLiteral("Triangle")):
        role==3?QStringLiteral("Noise"):QStringLiteral("DMC");
      m_tabs->addTab(QStringLiteral(" %1 ").arg(name));
      auto* page=b.start<score::GraphicsVBoxLayout>(m_tabs);
      auto* grid=b.start<score::GraphicsGridColumnsLayout>(page);grid->setColumns(role==2&&gameBoy(model)?3:4);
      for(int field=0;field<RoleFieldCount;++field) {
        const int c=roleControl(role,RoleField(field));
        if(!available(model,c))continue;
        b.control(grid,c,model);
      }
      if(role==2&&gameBoy(model))b.control(page,Wave,model);
      if(role==4) {
        b.control(page,DmcFile,model);
        auto* row=b.start<score::GraphicsHBoxLayout>(page);
        for(int c:{DmcAddress,DmcLength,DmcLevel,DmcLoop})b.control(row,c,model);
      }
    }
    m_tabs->addTab(QStringLiteral(" Setup "));
    auto* setupPage=b.start<score::GraphicsVBoxLayout>(m_tabs);
    b.control(setupPage,Routing,model);b.control(setupPage,Clock,model);
    b.finalizeLayout(this);
    m_tabs->setCurrentIndex(setup?roles:std::min(selected,roles-1));
    setRect(m_main->mapRectToParent(m_main->boundingRect()));
  }
  const ProcessModel& m_model;
  const Process::Context& m_context;
  Model m_displayModel{Model::NesNtsc};
  score::EmptyRectItem* m_hidden{};
  score::GraphicsHBoxLayout* m_header{};
  QGraphicsItem* m_sound{};
  score::GraphicsVBoxLayout* m_main{};
  score::GraphicsTabLayout* m_tabs{};
  QObject* m_bodyContext{};
};
}
LayerFactory::~LayerFactory()=default;
UuidKey<Process::ProcessModel> LayerFactory::concreteKey() const noexcept {return Metadata<ConcreteKey_k,ProcessModel>::get();}
bool LayerFactory::matches(const UuidKey<Process::ProcessModel>& key) const {return key==concreteKey();}
std::optional<double> LayerFactory::recommendedHeight() const noexcept {return 320.;}
score::ResizeableItem* LayerFactory::makeItem(const Process::ProcessModel& proc,const Process::Context& ctx,QGraphicsItem* parent) const {
  return new LayerItem{static_cast<const ProcessModel&>(proc),ctx,parent};
}
}
