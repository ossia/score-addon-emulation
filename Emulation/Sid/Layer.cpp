// SPDX-License-Identifier: GPL-3.0-or-later
#include "Layer.hpp"
#include "Controls.hpp"
#include "ProcessModel.hpp"
#include <Process/Dataflow/PortFactory.hpp>
#include <Control/Layout.hpp>
#include <score/application/GUIApplicationContext.hpp>
#include <score/graphics/TextItem.hpp>
#include <score/graphics/layouts/GraphicsBoxLayout.hpp>
#include <score/graphics/layouts/GraphicsGridLayout.hpp>
#include <score/graphics/layouts/GraphicsTabLayout.hpp>
#include <score/graphics/layouts/GraphicsStripDetailLayout.hpp>
#include <score/model/Skin.hpp>
#include <algorithm>
namespace Emulation::Sid
{
namespace
{
struct UiBuilder final : Process::LayoutBuilderBase
{
  template<typename L> L* start(QGraphicsItem* parent, qreal padding = 3.)
  {
    auto* l = new L{parent}; l->setPadding(padding); createdLayouts.push_back(l); return l;
  }
  QGraphicsItem* control(QGraphicsItem* parent, int c, const char* label = nullptr, bool labels = true)
  {
    Process::ControlPresentation p;
    p.size = score::ControlSize::Small;
    p.labelVisible = labels;
    if(c == Volume) p.widget = Process::ControlPresentation::Widget::Knob;
    if(label) p.label = QString::fromUtf8(label);
    auto item = makePort(*inlets[c + 1], p);
    if(item.container) item.container->setParentItem(parent);
    return item.container;
  }
  score::GraphicsSectionLayout* section(QGraphicsItem* parent, const char* title)
  {
    auto* s = start<score::GraphicsSectionLayout>(parent);
    s->setTitle(QString::fromUtf8(title)); s->setBrush(score::Skin::instance().Background2.darker); return s;
  }
  void grid(QGraphicsItem* parent, int columns, std::initializer_list<std::pair<int, const char*>> controls)
  {
    auto* g = start<score::GraphicsGridColumnsLayout>(parent, 2.); g->setColumns(columns);
    for(const auto& [c, label] : controls)
    {
      auto* cell = new score::EmptyRectItem{g};
      if(auto* item = control(cell, c, label))
      {
        const auto b = item->boundingRect();
        const auto width = std::max(70., b.width() + 4.);
        cell->setRect({0., 0., width, std::max(42., b.height())});
        item->setPos((width - b.width()) / 2., 0.);
      }
    }
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
    b.control(header, ChipModel, "Model");
    b.control(header, VoiceMode, "Voice allocation");
    b.control(header, Volume, "Volume");
    auto* tabs = b.start<score::GraphicsTabLayout>(main);
    tabs->setBrush(skin.Background2.darker300);
    tabs->addTab(QStringLiteral(" Oscillators "));
    tabs->addTab(QStringLiteral(" Filter "));
    tabs->addTab(QStringLiteral(" Chip setup "));
    auto* oscillators = b.start<score::GraphicsVBoxLayout>(tabs);
    oscillators->setBrush(skin.Background2.main);
    auto* table = b.start<score::GraphicsTableLayout>(oscillators);
    table->setColumnTitles({QStringLiteral("Wave"), QStringLiteral("Pulse"), QStringLiteral("Attack rate"),
        QStringLiteral("Decay rate"), QStringLiteral("Sustain level"), QStringLiteral("Release rate"), QStringLiteral("Filter")});
    table->setRowTitles(true); table->setRowsSelectable(true);
    for(int o = 0; o < 3; ++o)
    {
      auto* row = b.start<score::GraphicsSelectableRow>(table); m_rows[o] = row;
      auto* label = b.makeLabel("Osc");
      static_cast<score::SimpleTextItem*>(label)->setText(QStringLiteral("Osc %1").arg(o + 1));
      label->setParentItem(row);
      for(auto field : {Waveform, PulseWidth, Attack, Decay, Sustain, Release, FilterRoute})
        b.control(row, oscillatorControl(o, field), nullptr, false);
      row->onActivated = [this, o] { select(o); };
    }
    m_details = b.start<score::GraphicsStripDetailLayout>(oscillators);
    m_details->setStripVisible(false);
    for(int o = 0; o < 3; ++o)
    {
      auto* section = b.section(m_details, "Tuning & modulation");
      const auto source = (o + 2) % 3 + 1;
      section->setTitle(QStringLiteral("Osc %1: modulation from Osc %2").arg(o + 1).arg(source));
      auto* row = b.start<score::GraphicsHBoxLayout>(section);
      b.control(row, oscillatorControl(o, Semitones), "Semitones");
      b.control(row, oscillatorControl(o, Cents), "Cents");
      auto* sync = b.control(row, oscillatorControl(o, Sync), "Sync");
      auto* ring = b.control(row, oscillatorControl(o, Ring), "Ring");
      if(sync) sync->setToolTip(QStringLiteral("Hard sync: Osc %1 to Osc %2").arg(source).arg(o + 1));
      if(ring) ring->setToolTip(QStringLiteral("Triangle ring modulation: Osc %1 to Osc %2").arg(source).arg(o + 1));
    }
    auto* filter = b.start<score::GraphicsVBoxLayout>(tabs);
    filter->setBrush(skin.Background2.main);
    b.grid(b.section(filter, "Shared filter"), 4, {{FilterMode, "Mode"}, {Cutoff, "Cutoff register"},
        {Resonance, "Resonance"}, {Voice3Off, "Voice 3 direct off"}});
    auto* setup = b.start<score::GraphicsVBoxLayout>(tabs);
    setup->setBrush(skin.Background2.main);
    b.grid(b.section(setup, "Chip calibration & setup"), 4, {{ClockType, "Clock"},
        {FilterCurve, "Filter calibration"}, {CombinedStrength, "Combined waves"}, {ChipVolume, "Chip volume"}});
    b.finalizeLayout(this); select(0); fitChildrenRect();
  }
private:
  void select(int o)
  { for(int i = 0; i < 3; ++i) m_rows[i]->setSelected(i == o); m_details->setCurrentIndex(o); }
  score::GraphicsSelectableRow* m_rows[3]{};
  score::GraphicsStripDetailLayout* m_details{};
};
}
LayerFactory::~LayerFactory() = default;
UuidKey<Process::ProcessModel> LayerFactory::concreteKey() const noexcept
{ return Metadata<ConcreteKey_k, ProcessModel>::get(); }
bool LayerFactory::matches(const UuidKey<Process::ProcessModel>& key) const { return key == concreteKey(); }
std::optional<double> LayerFactory::recommendedHeight() const noexcept { return 320.; }
score::ResizeableItem* LayerFactory::makeItem(const Process::ProcessModel& p, const Process::Context& c, QGraphicsItem* parent) const
{ return new LayerItem{static_cast<const ProcessModel&>(p), c, parent}; }
}
