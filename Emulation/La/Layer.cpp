// SPDX-License-Identifier: GPL-3.0-or-later
#include "Layer.hpp"
#include "Controls.hpp"
#include "ProcessModel.hpp"
#include <Process/Commands/SetControlValue.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <Control/Layout.hpp>
#include <score/application/GUIApplicationContext.hpp>
#include <score/graphics/TextItem.hpp>
#include <score/graphics/layouts/GraphicsBoxLayout.hpp>
#include <score/graphics/layouts/GraphicsGridLayout.hpp>
#include <score/graphics/layouts/GraphicsTabLayout.hpp>
#include <score/model/Skin.hpp>
#include <score/graphics/widgets/QGraphicsCombo.hpp>
#include <score/graphics/widgets/QGraphicsToggle.hpp>
#include <algorithm>
namespace Emulation::La
{
namespace
{
struct UiBuilder final : Process::LayoutBuilderBase
{
  template<typename L> L* start(QGraphicsItem* parent, qreal padding = 3.)
  { auto* l = new L{parent}; l->setPadding(padding); createdLayouts.push_back(l); return l; }
  QGraphicsItem* control(QGraphicsItem* parent, int c, const char* label = nullptr, bool labels = true)
  {
    Process::ControlPresentation p;
    p.size = c == ControlROM || c == PcmROM ? score::ControlSize::Normal : score::ControlSize::Small;
    p.labelVisible = labels;
    if(c == Volume) p.widget = Process::ControlPresentation::Widget::Knob;
    p.label = QString::fromUtf8(label ? label : controlField(c).name);
    auto item = makePort(*inlets[c + 1], p);
    if(c >= PartBase && c < EditTimbre)
    {
      const auto field = (c - PartBase) % PartFieldCount;
      if(field == Bank || field == Program)
        if(auto* combo = dynamic_cast<score::QGraphicsCombo*>(item.control))
        {
          // Reserve space before the worker supplies native ten-character names.
          auto rect = combo->boundingRect();
          rect.setWidth(std::max(rect.width(), field == Program ? 150. : 120.));
          combo->setRect(rect);
          portFactory.get(inlets[c + 1]->concreteKey())->layoutFullItem(item, *inlets[c + 1], p);
        }
    }
    if(item.container) item.container->setParentItem(parent);
    return item.container;
  }
  score::GraphicsGridColumnsLayout* grid(QGraphicsItem* parent, int columns = 5)
  { auto* g = start<score::GraphicsGridColumnsLayout>(parent, 2.); g->setColumns(columns); return g; }
  void cell(QGraphicsItem* parent, int c) { control(parent, c); }
  void envelope(QGraphicsItem* parent, int partial, int time, int level, bool pitch)
  {
    auto* table = start<score::GraphicsTableLayout>(parent);
    table->setColumnTitles(pitch
        ? QStringList{QStringLiteral("Initial"), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("Sustain"), QStringLiteral("End")}
        : QStringList{QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("Sustain"), QStringLiteral("Release")});
    table->setRowTitles(true);
    for(int row = 0; row < 2; ++row)
    {
      auto* cells = start<score::GraphicsHBoxLayout>(table);
      auto* label = makeLabel(row == 0 ? "Time" : "Level"); label->setParentItem(cells);
      for(int stage = 0; stage < 5; ++stage)
      {
        if(row == 0 && pitch && stage == 0)
        { auto* empty = makeLabel("-"); empty->setParentItem(cells); }
        else if(row == 1 && !pitch && stage == 4)
        { auto* zero = makeLabel("0"); zero->setParentItem(cells); }
        else
          control(cells, partialControl(partial, row == 0 ? time + stage - int(pitch) : level + stage), nullptr, false);
      }
    }
  }
};
class LayerItem final : public score::EmptyRectItem
{
public:
  LayerItem(const ProcessModel& model, const Process::Context& ctx, QGraphicsItem* parent)
      : score::EmptyRectItem{parent}, m_model{model}, m_ctx{ctx}
  {
    UiBuilder b{*this, model, ctx, ctx.app.interfaces<Process::PortFactoryList>(), model.inlets(), model.outlets()};
    b.marginOnNestedLayouts = false;
    auto& skin = score::Skin::instance();
    m_hidden = new score::EmptyRectItem{this}; m_hidden->hide();
    auto* main = m_main = b.start<score::GraphicsVBoxLayout>(nullptr); main->setBrush(skin.Background2.darker);
    auto* header = b.start<score::GraphicsHBoxLayout>(main); header->setBrush(skin.Background2.darker300);
    b.control(header, VoiceAllocation); b.control(header, ReverbEnabled); b.control(header, Volume);
    auto* tabs = m_tabs = b.start<score::GraphicsTabLayout>(main); tabs->setBrush(skin.Background2.darker300);
    for(auto name : {" Instrument ", " Routing ", " System ", " ROMs "}) tabs->addTab(QString::fromLatin1(name));
    auto* instrument = m_instrument = b.start<score::GraphicsVBoxLayout>(tabs);
    auto* layers = b.start<score::GraphicsTabLayout>(instrument);
    m_layers = layers;
    layers->addTab(QStringLiteral(" Instrument "));
    for(int part = 1; part < 8; ++part) layers->addTab(QStringLiteral(" Layer %1 ").arg(part + 1));
    for(int part = 0; part < 8; ++part)
    {
      auto* page = b.start<score::GraphicsVBoxLayout>(layers);
      auto* sound = b.start<score::GraphicsHBoxLayout>(page);
      auto* enable = new score::QGraphicsToggle{QStringLiteral("Enable"), QStringLiteral("Enable"), sound};
      const auto* channel = static_cast<Process::ControlInlet*>(model.inlets()[partControl(part, Channel) + 1]);
      const auto update = [enable](const ossia::value& value) {
        enable->setState(ossia::convert<int>(value) != 16);
      };
      update(channel->value());
      connect(channel, &Process::ControlInlet::valueChanged, enable, update);
      connect(channel, &Process::ControlInlet::executionValueChanged, enable, update);
      connect(enable, &score::QGraphicsToggle::toggled, this, [this, channel](bool on) {
        m_ctx.dispatcher.submit<Process::SetControlValue>(
            *channel, channelForEnabled(ossia::convert<int>(channel->value()), on));
        m_ctx.dispatcher.commit();
      });
      b.control(sound, partControl(part, Bank));
      b.control(sound, partControl(part, Program));
      auto* parameters = b.grid(page, 4);
      for(auto field : {Level, Pan, KeyShift, FineTune}) b.cell(parameters, partControl(part, field));
    }
    layers->onCurrentIndexChanged = [this](int part) { selectPart(part); };
    auto* timbres = b.start<score::GraphicsVBoxLayout>(instrument);
    m_timbres = timbres;
    auto* common = b.start<score::GraphicsHBoxLayout>(timbres);
    for(int c : std::array<int, 3>{TimbreBase, TimbreBase + 1, TimbreBase + 3}) b.control(common, c);
    auto* enabled = b.start<score::GraphicsHBoxLayout>(timbres);
    auto* enabledLabel = b.makeLabel("Enabled partials"); enabledLabel->setParentItem(enabled);
    const auto* mask = static_cast<Process::ControlInlet*>(model.inlets()[TimbreBase + 3]);
    for(int partial = 0; partial < 4; ++partial)
    {
      auto* toggle = new score::QGraphicsToggle{
          QStringLiteral("%1 Off").arg(partial + 1), QStringLiteral("%1 On").arg(partial + 1), enabled};
      const auto update = [toggle, partial](const ossia::value& value) {
        toggle->setState((ossia::convert<int>(value) & (1 << partial)) != 0);
      };
      update(mask->value());
      connect(mask, &Process::ControlInlet::valueChanged, toggle, update);
      connect(mask, &Process::ControlInlet::executionValueChanged, toggle, update);
      connect(toggle, &score::QGraphicsToggle::toggled, this, [this, mask, partial](bool on) {
        const int value = ossia::convert<int>(mask->value());
        m_ctx.dispatcher.submit<Process::SetControlValue>(*mask, on ? value | (1 << partial) : value & ~(1 << partial));
        m_ctx.dispatcher.commit();
      });
    }
    auto* partials = b.start<score::GraphicsTabLayout>(timbres);
    for(int part = 0; part < 4; ++part) partials->addTab(QStringLiteral(" Partial %1 ").arg(part + 1));
    for(int partial = 0; partial < 4; ++partial)
    {
      auto* groups = b.start<score::GraphicsTabLayout>(partials);
      for(auto name : {" Oscillator ", " Pitch & LFO ", " Filter ", " Amplitude "}) groups->addTab(QString::fromLatin1(name));
      auto* oscillator = b.grid(groups);
      for(int field = 0; field < 8; ++field) b.cell(oscillator, partialControl(partial, field));
      for(int group = 0; group < 3; ++group)
      {
        auto* section = b.start<score::GraphicsVBoxLayout>(groups);
        auto* parameters = b.grid(section);
        const int first = group == 0 ? 8 : group == 1 ? 23 : 41;
        const int last = group == 0 ? 11 : group == 1 ? 32 : 49;
        for(int field = first; field < last; ++field) b.cell(parameters, partialControl(partial, field));
        b.envelope(section, partial, group == 0 ? 11 : group == 1 ? 32 : 49,
            group == 0 ? 15 : group == 1 ? 37 : 54, group == 0);
        if(group == 0)
        {
          auto* lfo = b.grid(section, 3);
          for(int field = 20; field < 23; ++field) b.cell(lfo, partialControl(partial, field));
        }
      }
    }
    auto* routing = b.start<score::GraphicsTabLayout>(tabs);
    routing->addTab(QStringLiteral(" Instrument "));
    for(int part = 1; part < 8; ++part) routing->addTab(QStringLiteral(" Layer %1 ").arg(part + 1));
    for(int part = 0; part < 8; ++part)
    {
      auto* grid = b.grid(routing, 5);
      for(auto field : {Channel, BendRange, AssignMode, Reverb, Reserve}) b.cell(grid, partControl(part, field));
    }
    auto* system = b.start<score::GraphicsVBoxLayout>(tabs);
    auto* sound = b.grid(system);
    for(auto c : {ReverbMode,ReverbTime,ReverbLevel,MasterTune,MasterLevel,ReverseStereo,DacMode,MidiDelay,NiceRamp,NicePan,NiceMix}) b.cell(sound, c);
    auto* rhythm = b.grid(system);
    for(auto c : {RhythmChannel,RhythmLevel,RhythmPan,RhythmReverb,RhythmReserve}) b.cell(rhythm, c);
    auto* roms = b.start<score::GraphicsVBoxLayout>(tabs);
    b.control(roms, ControlROM); b.control(roms, PcmROM);
    connect(&model, &ProcessModel::patchChanged, this, [this] { refreshEditor(); });
    b.finalizeLayout(this); refreshEditor();
    setRect(m_main->mapRectToParent(m_main->boundingRect()));
  }
private:
  void selectPart(int part)
  {
    m_selectedPart = part;
    const_cast<ProcessModel&>(m_model).setSelectedPart(part);
    const auto& patch = m_model.patch();
    if(patch.values[partControl(part, Bank)] == 1
        && patch.values[EditTimbre] != patch.values[partControl(part, Program)])
    {
      const auto& target = *static_cast<Process::ControlInlet*>(m_model.inlets()[EditTimbre + 1]);
      m_ctx.dispatcher.submit<Process::SetControlValue>(target, patch.values[partControl(part, Program)]);
      m_ctx.dispatcher.commit();
    }
    refreshEditor();
  }
  void refreshEditor()
  {
    const auto& patch = m_model.patch();
    m_selectedPart = m_model.selectedPart();
    m_layers->setCurrentIndex(m_selectedPart);
    if(m_editorSlot != patch.values[EditTimbre])
    {
      m_editorSlot = patch.values[EditTimbre];
      // Restore an existing memory-editor target by changing only the visible
      // tab, never the loaded patch or its native receive assignments.
      if(patch.values[partControl(m_selectedPart, Bank)] != 1
          || patch.values[partControl(m_selectedPart, Program)] != m_editorSlot)
        for(int part = 0; part < 8; ++part)
          if(patch.values[partControl(part, Bank)] == 1
              && patch.values[partControl(part, Program)] == m_editorSlot)
          {
            m_selectedPart = part;
            const_cast<ProcessModel&>(m_model).setSelectedPart(part);
            m_layers->setCurrentIndex(part);
            break;
          }
    }
    // A factory or ROM selection has no editable memory payload. Never present
    // an unrelated memory slot as though it were the selected layer's sound.
    const bool editable = patch.values[partControl(m_selectedPart, Bank)] == 1
        && patch.values[partControl(m_selectedPart, Program)] == patch.values[EditTimbre];
    auto* parent = editable ? static_cast<QGraphicsItem*>(m_instrument) : m_hidden;
    if(m_timbres->parentItem() != parent)
    {
      m_timbres->setParentItem(parent);
      // Only the ancestor layouts change; keep the native grid assemblies intact.
      for(auto* layout : std::array<score::GraphicsLayout*, 3>{m_instrument, m_tabs, m_main})
      {
        layout->setRect({});
        layout->layout();
        const auto bounds = layout->childrenBoundingRect();
        layout->setRect({0., 0., bounds.right() + 4., bounds.bottom() + 4.});
      }
      setRect(m_main->mapRectToParent(m_main->boundingRect()));
    }
    m_timbres->setVisible(editable);
    m_timbres->setEnabled(editable);
  }
  const ProcessModel& m_model;
  const Process::Context& m_ctx;
  int m_selectedPart{};
  int m_editorSlot{-1};
  score::EmptyRectItem* m_hidden{};
  score::GraphicsVBoxLayout *m_main{}, *m_instrument{};
  score::GraphicsTabLayout* m_tabs{};
  score::GraphicsTabLayout* m_layers{};
  score::GraphicsVBoxLayout* m_timbres{};
};
}
LayerFactory::~LayerFactory() = default;
UuidKey<Process::ProcessModel> LayerFactory::concreteKey() const noexcept
{ return Metadata<ConcreteKey_k, ProcessModel>::get(); }
bool LayerFactory::matches(const UuidKey<Process::ProcessModel>& key) const { return key == concreteKey(); }
std::optional<double> LayerFactory::recommendedHeight() const noexcept { return 360.; }
score::ResizeableItem* LayerFactory::makeItem(const Process::ProcessModel& p, const Process::Context& c, QGraphicsItem* parent) const
{ return new LayerItem{static_cast<const ProcessModel&>(p), c, parent}; }
}
