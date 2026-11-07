#include "Layer.hpp"
#include "Controls.hpp"
#include "ProcessModel.hpp"
#include <Process/Dataflow/PortFactory.hpp>
#include <Control/Layout.hpp>
#include <score/application/GUIApplicationContext.hpp>
#include <score/graphics/TextItem.hpp>
#include <score/graphics/layouts/GraphicsBoxLayout.hpp>
#include <score/graphics/layouts/GraphicsGridLayout.hpp>
#include <score/model/Skin.hpp>
#include <algorithm>
#include <memory>
#include <vector>
namespace Emulation::Psg
{
namespace
{
struct UiBuilder final : Process::LayoutBuilderBase
{
  template<typename L> L* start(QGraphicsItem* parent, qreal padding = 4.) {
    auto* layout = new L{parent}; layout->setPadding(padding);
    createdLayouts.push_back(layout); return layout;
  }
  QGraphicsItem* control(QGraphicsItem* parent, int c, bool label = true) {
    Process::ControlPresentation presentation;
    presentation.size = score::ControlSize::Small; presentation.labelVisible = label;
    switch(c) {
      case AyNoisePeriod: presentation.label = QStringLiteral("Noise period"); break;
      case AyEnvelopePeriod: presentation.label = QStringLiteral("Envelope period"); break;
      case AyEnvelopeShape: presentation.label = QStringLiteral("Shape"); break;
      default: break;
    }
    if(c == Volume) presentation.widget = Process::ControlPresentation::Widget::Knob;
    auto item = makePort(*inlets[c + 1], presentation);
    if(item.container) item.container->setParentItem(parent);
    return item.container;
  }
  score::GraphicsSectionLayout* section(QGraphicsItem* parent, const char* title) {
    auto* s = start<score::GraphicsSectionLayout>(parent, 3.);
    s->setTitle(QString::fromUtf8(title)); s->setBrush(score::Skin::instance().Background2.darker); return s;
  }
};
class LayerItem final : public score::EmptyRectItem
{
public:
  LayerItem(const ProcessModel& model, const Process::Context& ctx, QGraphicsItem* parent)
    : score::EmptyRectItem{parent}, m_model{model}
  {
    UiBuilder b{*this, model, ctx, ctx.app.interfaces<Process::PortFactoryList>(), model.inlets(), model.outlets()};
    b.marginOnNestedLayouts = false;
    auto& skin = score::Skin::instance();
    m_hidden = std::make_unique<score::EmptyRectItem>(nullptr);
    m_hidden->hide();
    m_main = b.start<score::GraphicsVBoxLayout>(this);
    m_main->setBrush(skin.Background2.darker);
    auto* header = b.start<score::GraphicsHBoxLayout>(m_main);
    header->setBrush(skin.Background2.darker300);
    m_controls[ChipModel] = b.control(header, ChipModel);
    m_controls[Volume] = b.control(header, Volume);
    auto* performance = b.start<score::GraphicsHBoxLayout>(m_main);
    performance->setZValue(1.);
    m_controls[VoiceAssignment] = b.control(performance, VoiceAssignment);
    m_controls[SoftwareEnvelope] = b.control(performance, SoftwareEnvelope);
    m_table = b.start<score::GraphicsTableLayout>(m_main, 3.);
    m_table->setZValue(2.);
    m_table->setRowTitles(true);
    for(int ch = 0; ch < 3; ++ch) {
      auto* row = b.start<score::GraphicsHBoxLayout>(m_table, 3.);
      auto* title = b.makeLabel(ch == 0 ? "Tone A" : ch == 1 ? "Tone B" : "Tone C");
      title->setParentItem(row);
      for(auto field : {Tone, Noise, Level, HardwareEnvelope}) {
        const int c = channelControl(ch, field);
        m_controls[c] = b.control(row, c, false);
      }
    }
    auto* global = b.section(m_main, "Global");
    global->setZValue(3.);
    m_global = b.start<score::GraphicsHBoxLayout>(global, 3.);
    m_controls[Clock] = b.control(m_global, Clock);
    m_ayNoise = m_controls[AyNoisePeriod] = b.control(m_global, AyNoisePeriod);
    m_ayNoise->setZValue(1.);
    m_sega = b.start<score::GraphicsHBoxLayout>(m_global, 3.);
    m_sega->setZValue(1.);
    for(int c : {SegaNoiseEnabled, SegaNoiseRate, SegaWhiteNoise, SegaNoiseLevel})
      m_controls[c] = b.control(m_sega, c);
    m_hardwareEnvelope = b.section(m_main, "Hardware envelope");
    m_hardwareEnvelope->setZValue(4.);
    auto* envelope = b.start<score::GraphicsHBoxLayout>(m_hardwareEnvelope, 3.);
    for(int c : {AyEnvelopePeriod, AyEnvelopeShape})
      m_controls[c] = b.control(envelope, c);
    m_adsr = b.section(m_main, "Software ADSR");
    m_adsr->setZValue(4.);
    auto* adsr = b.start<score::GraphicsHBoxLayout>(m_adsr, 3.);
    for(int c : {Attack, Decay, Sustain, Release})
      m_controls[c] = b.control(adsr, c);
    m_layouts = std::move(b.createdLayouts);
    connect(&model, &ProcessModel::settingsChanged, this, [this] { refresh(); });
    refresh();
  }
private:
  void place(QGraphicsItem* item, QGraphicsItem* parent, bool visible) {
    auto* destination = visible ? parent : m_hidden.get();
    if(item->parentItem() != destination) item->setParentItem(destination);
    item->setVisible(visible);
  }
  void refresh() {
    const auto& settings = m_model.settings();
    const bool sega = settings.model == Model::SegaYm7101;
    unsigned context = unsigned(sega) | (unsigned(settings.softwareEnvelope) << 1);
    for(int ch = 0; ch < 3; ++ch)
      context |= unsigned(settings.channels[ch].hardwareEnvelope) << (ch + 2);
    if(context == m_context) return;
    m_context = context;
    place(m_ayNoise, m_global, !sega);
    place(m_sega, m_global, sega);
    place(m_adsr, m_main, settings.softwareEnvelope);
    for(int ch = 0; ch < 3; ++ch) {
      m_controls[channelControl(ch, Noise)]->setVisible(!sega);
      m_controls[channelControl(ch, HardwareEnvelope)]->setVisible(!sega && !settings.softwareEnvelope);
      m_controls[channelControl(ch, Level)]->setVisible(
          sega || settings.softwareEnvelope || !settings.channels[ch].hardwareEnvelope);
    }
    const bool hardware = !sega && !settings.softwareEnvelope && std::any_of(
        settings.channels.begin(), settings.channels.end(), [](const Channel& ch) { return ch.hardwareEnvelope; });
    place(m_hardwareEnvelope, m_main, hardware);
    m_table->setColumnTitles({QStringLiteral("Tone"), sega ? QString{} : QStringLiteral("Noise"),
        QStringLiteral("Level"), sega || settings.softwareEnvelope ? QString{} : QStringLiteral("AY routing")});
    for(auto it = m_layouts.rbegin(); it != m_layouts.rend(); ++it) {
      (*it)->setRect({});
      (*it)->layout();
      const auto bounds = (*it)->childrenBoundingRect();
      (*it)->setRect({0., 0., bounds.right() + 4., bounds.bottom() + 4.});
    }
    setRect(m_main->mapRectToParent(m_main->boundingRect()));
  }
  const ProcessModel& m_model;
  std::array<QGraphicsItem*, ControlCount> m_controls{};
  std::vector<score::GraphicsLayout*> m_layouts;
  std::unique_ptr<score::EmptyRectItem> m_hidden;
  score::GraphicsVBoxLayout* m_main{};
  score::GraphicsHBoxLayout* m_global{};
  score::GraphicsTableLayout* m_table{};
  QGraphicsItem *m_ayNoise{}, *m_sega{}, *m_adsr{}, *m_hardwareEnvelope{};
  unsigned m_context{~0u};
};
}
LayerFactory::~LayerFactory() = default;
UuidKey<Process::ProcessModel> LayerFactory::concreteKey() const noexcept
{ return Metadata<ConcreteKey_k, ProcessModel>::get(); }
bool LayerFactory::matches(const UuidKey<Process::ProcessModel>& key) const { return key == concreteKey(); }
std::optional<double> LayerFactory::recommendedHeight() const noexcept { return 300.; }
score::ResizeableItem* LayerFactory::makeItem(const Process::ProcessModel& proc,
    const Process::Context& ctx, QGraphicsItem* parent) const
{ return new LayerItem{static_cast<const ProcessModel&>(proc), ctx, parent}; }
}
