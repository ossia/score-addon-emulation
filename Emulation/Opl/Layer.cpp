#include "Layer.hpp"
#include "Controls.hpp"
#include "ProcessModel.hpp"
#include <Process/Commands/SetControlValue.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <Process/Dataflow/PortItem.hpp>
#include <Control/Layout.hpp>
#include <score/application/GUIApplicationContext.hpp>
#include <score/graphics/TextItem.hpp>
#include <score/graphics/layouts/GraphicsBoxLayout.hpp>
#include <score/graphics/layouts/GraphicsGridLayout.hpp>
#include <score/graphics/layouts/GraphicsTabLayout.hpp>
#include <score/graphics/layouts/GraphicsStripDetailLayout.hpp>
#include <score/graphics/widgets/QGraphicsCombo.hpp>
#include <score/model/Skin.hpp>
#include <algorithm>
#include <array>
#include <memory>

namespace Emulation::Opl
{
namespace
{
struct UiBuilder final : Process::LayoutBuilderBase
{
  struct Grid
  {
    score::GraphicsGridColumnsLayout* layout;
    QGraphicsItem* section;
    QGraphicsItem* parent;
    std::vector<std::pair<int, QGraphicsItem*>> cells;
  };
  std::vector<Grid> grids;
  template <typename L> L* start(QGraphicsItem* parent, qreal padding = 4.)
  {
    auto* layout = new L{parent};
    layout->setPadding(padding);
    createdLayouts.push_back(layout);
    return layout;
  }
  QGraphicsItem* control(QGraphicsItem* parent, int c,
                        const char* label = nullptr, bool labelVisible = true)
  {
    Process::ControlPresentation presentation;
    presentation.labelVisible = labelVisible;
    if(c == Volume) presentation.widget = Process::ControlPresentation::Widget::Knob;
    if(label) presentation.label = QString::fromUtf8(label);
    auto item = makePort(*inlets[c + 1], presentation);
    if(item.container)
    {
      item.container->setParentItem(parent);
    }
    return item.container;
  }
  void grid(QGraphicsItem* parent, int columns,
            std::initializer_list<std::pair<int, const char*>> controls)
  {
    auto* g = start<score::GraphicsGridColumnsLayout>(parent, 2.);
    g->setColumns(columns);
    grids.push_back({g, parent, parent->parentItem(), {}});
    auto& grid = grids.back();
    for(const auto& [c, label] : controls)
    {
      auto* cell = new score::EmptyRectItem{g};
      grid.cells.emplace_back(c, cell);
      if(auto* item = control(cell, c, label))
      {
        const auto bounds = item->boundingRect();
        const auto width = std::max(70., bounds.width() + 4.);
        cell->setRect({0., 0., width, std::max(42., bounds.height())});
        item->setPos((width - bounds.width()) / 2., 0.);
      }
    }
  }
  score::GraphicsSectionLayout* section(QGraphicsItem* parent, const char* title)
  {
    auto* s = start<score::GraphicsSectionLayout>(parent, 3.);
    s->setTitle(QString::fromUtf8(title));
    s->setBrush(score::Skin::instance().Background2.darker);
    return s;
  }
};
class LayerItem final : public score::EmptyRectItem
{
public:
  LayerItem(const ProcessModel& model, const Process::Context& ctx, QGraphicsItem* parent)
      : score::EmptyRectItem{parent}, m_model{model}, m_ctx{ctx}
  {
    m_builder.reset(new UiBuilder{*this, model, ctx, ctx.app.interfaces<Process::PortFactoryList>(),
                model.inlets(), model.outlets()});
    auto& b = *m_builder;
    m_hidden = std::make_unique<score::EmptyRectItem>(nullptr);
    m_hidden->setVisible(false);
    b.marginOnNestedLayouts = false;
    auto& skin = score::Skin::instance();
    auto* main = b.start<score::GraphicsVBoxLayout>(nullptr);
    main->setBrush(skin.Background2.darker);
    auto* header = b.start<score::GraphicsHBoxLayout>(main);
    header->setBrush(skin.Background2.darker300);
    b.control(header, File, "Bank");

    auto* programCell = new score::EmptyRectItem{header};
    auto* programLabel = b.makeLabel("Instrument");
    programLabel->setParentItem(programCell);
    programLabel->setPos(10., 0.);
    m_programs = new score::QGraphicsCombo{{QStringLiteral("-")}, programCell};
    m_programs->setRect({0., 0., 240., m_programs->boundingRect().height()});
    m_programs->setPos(10., 12.);
    programCell->setRect({0., 0., 250., 12. + m_programs->boundingRect().height()});
    b.control(header, Volume, "Volume");
    b.control(header, VoiceAllocation, "Voice allocation");
    auto* instrument = static_cast<Process::ControlInlet*>(model.inlets()[InstrumentIndex + 1]);
    if(auto* factory = b.portFactory.get(instrument->concreteKey()))
      if(auto* port = factory->makePortItem(*instrument, m_ctx, programCell, this))
        port->setPos(0., 2.);
    connect(m_programs, &score::QGraphicsCombo::sliderMoved, this, [this, instrument] {
      const int index = m_programs->value();
      if(index < 0 || index >= std::ssize(m_programIndices) || m_programIndices[index] < 0) return;
      m_programs->moving = true;
      m_ctx.dispatcher.submit<Process::SetControlValue>(*instrument, m_programIndices[index]);
    });
    connect(m_programs, &score::QGraphicsCombo::sliderReleased, this, [this, instrument] {
      const int index = m_programs->value();
      if(index >= 0 && index < std::ssize(m_programIndices) && m_programIndices[index] >= 0)
        m_ctx.dispatcher.submit<Process::SetControlValue>(*instrument, m_programIndices[index]);
      m_ctx.dispatcher.commit();
      m_programs->moving = false;
    });

    auto* tabs = b.start<score::GraphicsTabLayout>(main, 3.);
    tabs->setBrush(skin.Background2.darker300);
    for(const auto* title : {" Operators ", " Patch ", " Setup "})
      tabs->addTab(QString::fromLatin1(title));
    auto* operators = b.start<score::GraphicsVBoxLayout>(tabs, 3.);
    m_operators = operators;
    operators->setBrush(skin.Background2.main);
    auto* table = b.start<score::GraphicsTableLayout>(operators, 3.);
    m_table = table;
    table->setColumnTitles({QStringLiteral("Wave"), QStringLiteral("Mult. code"),
        QStringLiteral("Atten."), QStringLiteral("Attack rate"), QStringLiteral("Decay rate"),
        QStringLiteral("Sustain atten."), QStringLiteral("Release rate")});
    table->setRowTitles(true);
    table->setRowsSelectable(true);
    for(int op = 0; op < 4; ++op)
    {
      auto* row = b.start<score::GraphicsSelectableRow>(table, 3.);
      m_rows[op] = row;
      auto* title = b.makeLabel("Op");
      static_cast<score::SimpleTextItem*>(title)->setText(QStringLiteral("Op %1").arg(op + 1));
      title->setParentItem(row);
      for(auto field : {Waveform, Multiplier, Level, Attack, Decay, Sustain, Release})
        b.control(row, operatorControl(op, field), nullptr, false);
      row->onActivated = [this, op] { selectOperator(op); };
    }
    m_details = b.start<score::GraphicsStripDetailLayout>(operators, 3.);
    m_details->setStripVisible(false);
    for(int op = 0; op < 4; ++op)
    {
      auto* page = b.section(m_details, "Scaling & modulation");
      page->setTitle(QStringLiteral("Op %1: scaling & modulation").arg(op + 1));
      b.grid(page, 5, {
          {operatorControl(op, KeyScaleLevel), "Key level"},
          {operatorControl(op, KeyScaleRate), "Key rate"},
          {operatorControl(op, EnvelopeSustain), "Sustain mode"},
          {operatorControl(op, Vibrato), "Vibrato"},
          {operatorControl(op, Tremolo), "Tremolo"}});
    }

    auto* patch = b.start<score::GraphicsVBoxLayout>(tabs, 3.);
    patch->setBrush(skin.Background2.main);
    b.grid(b.section(patch, "Voice"), 4, {
        {VoiceMode, "Mode"}, {RhythmMode, "Rhythm"}, {PercussionKey, "Drum pitch"},
        {Blank, "Blank"}});
    b.grid(b.section(patch, "Routing & tuning"), 4, {
        {Feedback1, "Feedback 1"}, {Connection1, "Connection 1"},
        {NoteOffset1, "Tune 1"}, {VelocityOffset, "Velocity"},
        {Feedback2, "Feedback 2"}, {Connection2, "Connection 2"},
        {NoteOffset2, "Tune 2"}, {Detune, "Detune 2"}});
    b.grid(b.section(patch, "Timing"), 2, {
        {DelayOn, "Key-on ms"}, {DelayOff, "Key-off ms"}});

    auto* setup = b.start<score::GraphicsVBoxLayout>(tabs, 3.);
    setup->setBrush(skin.Background2.main);
    b.grid(b.section(setup, "Engine"), 3, {
        {ChipModel, "Model"}, {Chips, "Chips"}, {FourOps, "Four-op channels"}});
    b.grid(b.section(setup, "Bank address"), 4, {
        {BankMSB, "MSB"}, {BankLSB, "LSB / kit"}, {Percussion, "Drum bank"},
        {ProgramNumber, "Program"}});
    b.grid(b.section(setup, "Modulation depth"), 2, {
        {DeepVibrato, "Vibrato"}, {DeepTremolo, "Tremolo"}});

    connect(&model, &ProcessModel::bankChanged, this, [this] { refresh(); });
    connect(&model, &ProcessModel::instrumentChanged, this, [this] { refreshContext(); });
    connect(&model, &ProcessModel::selectedAddressChanged, this, [this] { refresh(); });
    refresh();
    selectOperator(0);
  }
private:
  void selectOperator(int op)
  {
    for(int i = 0; i < 4; ++i) m_rows[i]->setSelected(i == op);
    m_details->setCurrentIndex(op);
    m_selectedOperator = op;
  }
  void refreshContext()
  {
    const auto bank = m_model.bank();
    const auto* patch = bank ? findProgram(*bank, m_model.selectedAddress()) : nullptr;
    const int mode = patch ? (patch->instrument.inst_flags & 3) : 0;
    const bool dual = mode == 3;
    const bool four = mode == 1;
    std::array<bool, ControlCount> visible;
    visible.fill(true);
    const int count = patch ? (dual || four ? 4 : 2) : 0;
    for(int c = VoiceMode; c < OperatorBase; ++c) visible[c] = bool(patch);
    visible[Feedback2] = visible[Connection2] = patch && (dual || four);
    visible[NoteOffset2] = visible[Detune] = patch && dual;
    visible[RhythmMode] = patch && m_model.selectedAddress().percussion && mode == 0;
    visible[PercussionKey] = patch && m_model.selectedAddress().percussion;
    visible[FourOps] = m_model.settings().model == Model::Opl3;
    if(count == m_operatorCount && visible == m_visible) return;
    m_operatorCount = count;
    m_visible = visible;
    if(m_selectedOperator >= count) selectOperator(0);
    reflow();
  }
  void reflow()
  {
    // Native layouts measure all children, including hidden ones. Keep inactive
    // authored controls owned by the layer but outside the measured layout tree.
    for(int op = 0; op < 4; ++op) m_rows[op]->setParentItem(m_hidden.get());
    for(int op = 0; op < m_operatorCount; ++op)
    {
      m_rows[op]->setParentItem(m_table);
      m_rows[op]->setVisible(true);
    }
    m_table->setParentItem(m_hidden.get());
    m_details->setParentItem(m_hidden.get());
    if(m_operatorCount)
    {
      m_table->setParentItem(m_operators);
      m_details->setParentItem(m_operators);
      m_table->setVisible(true);
      m_details->setVisible(true);
    }
    for(auto& grid : m_builder->grids)
    {
      for(const auto& [c, cell] : grid.cells) cell->setParentItem(m_hidden.get());
      // Grid layout creates wrapper cells. Unwrap before each new native layout
      // pass rather than nesting wrappers or retaining gaps from hidden fields.
      const auto wrappers = grid.layout->childItems();
      for(auto* wrapper : wrappers) delete wrapper;
      grid.section->setParentItem(m_hidden.get());
      bool active{};
      for(const auto& [c, cell] : grid.cells)
        if(m_visible[c])
        {
          cell->setParentItem(grid.layout);
          cell->setVisible(true);
          active = true;
        }
      if(active)
      {
        grid.section->setParentItem(grid.parent);
        grid.section->setVisible(true);
      }
    }
    for(auto* layout : m_builder->createdLayouts) layout->setRect({});
    m_builder->finalizeLayout(this);
    fitChildrenRect();
  }
  void refresh()
  {
    QStringList names;
    m_programIndices.clear();
    const auto selected = m_model.selectedAddress();
    int index = -1;
    if(auto bank = m_model.bank())
      for(std::size_t i = 0; i < bank->programs.size(); ++i)
      {
        const auto& p = bank->programs[i];
        if((p.instrument.inst_flags & ADLMIDI_Ins_IsBlank) && p.address != selected) continue;
        if(p.address == selected) index = int(m_programIndices.size());
        m_programIndices.push_back(int(i));
        names.push_back(QStringLiteral("%1 [%2:%3:%4%5]")
            .arg(QString::fromStdString(p.name)).arg(p.address.msb).arg(p.address.lsb)
            .arg(p.address.program).arg(p.address.percussion ? QStringLiteral(" drum") : QString{}));
      }
    if(index < 0)
    {
      names.prepend(m_model.selectedName());
      m_programIndices.insert(m_programIndices.begin(), -1);
      index = 0;
    }
    m_programs->array = names;
    m_programs->setValue(index >= 0 ? index : 0);
    refreshContext();
  }
  const ProcessModel& m_model;
  const Process::Context& m_ctx;
  score::QGraphicsCombo* m_programs{};
  score::GraphicsSelectableRow* m_rows[4]{};
  score::GraphicsStripDetailLayout* m_details{};
  std::unique_ptr<UiBuilder> m_builder;
  std::unique_ptr<score::EmptyRectItem> m_hidden;
  score::GraphicsVBoxLayout* m_operators{};
  score::GraphicsTableLayout* m_table{};
  std::array<bool, ControlCount> m_visible{};
  int m_operatorCount{-1};
  int m_selectedOperator{};
  std::vector<int> m_programIndices;
};
}
LayerFactory::~LayerFactory() = default;
UuidKey<Process::ProcessModel> LayerFactory::concreteKey() const noexcept
{ return Metadata<ConcreteKey_k, ProcessModel>::get(); }
bool LayerFactory::matches(const UuidKey<Process::ProcessModel>& key) const
{ return key == concreteKey(); }
std::optional<double> LayerFactory::recommendedHeight() const noexcept { return 360.; }
score::ResizeableItem* LayerFactory::makeItem(const Process::ProcessModel& proc,
    const Process::Context& ctx, QGraphicsItem* parent) const
{ return new LayerItem{static_cast<const ProcessModel&>(proc), ctx, parent}; }
}
