#include "Layer.hpp"
#include "Controls.hpp"
#include "ProcessModel.hpp"
#include <Dataflow/WaveformInlet.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <Process/Dataflow/PortItem.hpp>
#include <Control/Layout.hpp>
#include <score/application/GUIApplicationContext.hpp>
#include <score/graphics/layouts/GraphicsBoxLayout.hpp>
#include <score/graphics/layouts/GraphicsTabLayout.hpp>
#include <score/model/Skin.hpp>
#include <algorithm>
namespace Emulation::Wavetable {
namespace {
struct UiBuilder final : Process::LayoutBuilderBase {
  template<typename L> L* start(QGraphicsItem* parent) {
    auto* layout = new L{parent}; layout->setPadding(3.);
    layout->setZValue(createdLayouts.size()); createdLayouts.push_back(layout); return layout;
  }
  QGraphicsItem* control(QGraphicsItem* parent, int c) {
    Process::ControlPresentation presentation; presentation.size = score::ControlSize::Small;
    if(c == Volume || (c >= ChannelBase && (c - ChannelBase) % ChannelStride == Level))
      presentation.widget = Process::ControlPresentation::Widget::Knob;
    if(c >= ChannelBase) presentation.label = inlets[c + 1]->name().mid(5);
    if(c == LfoPeriod) presentation.label = QStringLiteral("Source period");
    auto item = makePort(*inlets[c + 1], presentation);
    if(item.container) { item.container->setParentItem(parent); item.container->setZValue(c); }
    return item.container;
  }
};
class LayerItem final : public score::EmptyRectItem {
public:
  LayerItem(const ProcessModel& model, const Process::Context& ctx, QGraphicsItem* parent)
    : score::EmptyRectItem{parent}, m_model{model} {
    UiBuilder b{*this, model, ctx, ctx.app.interfaces<Process::PortFactoryList>(), model.inlets(), model.outlets()};
    b.marginOnNestedLayouts = false;
    auto& skin = score::Skin::instance();
    m_hidden = new score::EmptyRectItem{this}; m_hidden->hide();
    m_main = b.start<score::GraphicsVBoxLayout>(this); m_main->setBrush(skin.Background2.darker);
    auto* header = b.start<score::GraphicsHBoxLayout>(m_main);
    header->setBrush(skin.Background2.darker300);
    for(int c : {ChipModel, Volume, VoiceAssignment}) m_controls[c] = b.control(header, c);
    m_setup = b.start<score::GraphicsHBoxLayout>(m_main);
    for(int c : {Clock, Tuning, GlobalLeft, GlobalRight}) m_controls[c] = b.control(m_setup, c);
    m_lfo = b.start<score::GraphicsHBoxLayout>(m_main);
    for(int c : {LfoDepth, LfoFrequency}) m_controls[c] = b.control(m_lfo, c);
    m_editor = b.start<score::GraphicsVBoxLayout>(m_hidden);
    m_detail = b.start<score::GraphicsHBoxLayout>(m_editor);
    for(int ch = 0; ch < 6; ++ch) {
      auto& wave = *static_cast<Dataflow::WaveformInlet*>(model.inlets()[channelControl(ch, Wave) + 1]);
      auto* factory = b.portFactory.get(wave.concreteKey());
      SCORE_ASSERT(factory);
      m_waveFactories[ch] = factory;
      // Keep each resource's stable identity and native selectable label alive.
      // Only the sample editor is shared between these complete assemblies.
      m_waves[ch] = ch == 0
          ? factory->makeFullItem(wave, ctx, m_hidden, this)
          : factory->makeLabelItem(wave, ctx, m_hidden, this);
      m_waves[ch].container->setZValue(ControlCount);
    }
    for(int ch = 0; ch < 6; ++ch)
      for(auto f : {Enabled, Level, ChannelTuning, Left, Right, Noise, NoisePeriod}) {
        const int c = channelControl(ch, f); m_controls[c] = b.control(m_hidden, c);
      }
    m_controls[LfoPeriod] = b.control(m_hidden, LfoPeriod);
    m_waveform = m_waves[0].control;
    m_layouts = std::move(b.createdLayouts);
    connect(&model, &ProcessModel::settingsChanged, this, [this] { refresh(); });
    refresh();
  }
private:
  struct LayoutState {
    Model model;
    int channel;
    bool lfo, noise;
    bool operator==(const LayoutState&) const = default;
  };
  void place(QGraphicsItem* item, QGraphicsItem* parent, bool visible) {
    auto* destination = visible ? parent : m_hidden;
    if(item->parentItem() != destination) item->setParentItem(destination);
    item->setVisible(visible);
  }
  void selectChannel(int channel) {
    m_selected = channel;
    refresh();
  }
  void updateTabs(int channels) {
    if(m_channelCount != channels) {
      m_editor->setParentItem(m_hidden);
      delete m_tabs;
      m_tabs = new score::GraphicsTabLayout{m_main};
      m_tabs->setBrush(score::Skin::instance().Background2.darker300);
      m_tabs->setPadding(3.);
      m_tabs->setZValue(ControlCount);
      for(int ch = 0; ch < channels; ++ch) {
        m_tabs->addTab(QStringLiteral(" Channel %1 ").arg(ch + 1));
        m_pages[ch] = new score::EmptyRectItem{m_tabs};
        m_pages[ch]->setZValue(ch);
      }
      m_tabs->onCurrentIndexChanged = [this](int ch) { selectChannel(ch); };
      m_channelCount = channels;
    }
    m_editor->setParentItem(m_pages[m_selected]);
    m_editor->show();
    for(int ch = 0; ch < channels; ++ch)
      m_pages[ch]->setRect({});
    m_tabs->setCurrentIndex(m_selected);
  }
  static void measure(score::GraphicsLayout& layout) {
    layout.layout();
    const auto bounds = layout.childrenBoundingRect();
    layout.setRect({0., 0., bounds.right() + 3., bounds.bottom() + 3.});
  }
  void layoutEditor() {
    for(auto it = m_layouts.rbegin(); it != m_layouts.rend(); ++it)
      if(*it != m_main) measure(**it);
    measure(*m_tabs);
    measure(*m_main);
  }
  void refresh() {
    const auto value = [this](int c) {
      return ossia::convert<int>(static_cast<Process::ControlInlet*>(m_model.inlets()[c + 1])->value());
    };
    const auto model = static_cast<Model>(value(ChipModel));
    const bool huc = model == Model::HuC6280;
    const int channels = huc ? 6 : 5;
    m_selected = std::min(m_selected, channels - 1);
    const bool lfo = huc && value(LfoDepth);
    const bool noise = huc && m_selected >= 4 && value(channelControl(m_selected, Noise));
    const LayoutState state{model, m_selected, lfo, noise};
    // Sample edits publish every mouse move. Avoid hiding the editor and
    // releasing its mouse grab unless the panel's presentation changes.
    if(m_layoutState && *m_layoutState == state) return;
    m_layoutState = state;
    updateTabs(channels);
    const bool source = lfo && m_selected == 1;
    for(int c : {GlobalLeft, GlobalRight}) place(m_controls[c], m_setup, huc);
    place(m_lfo, m_main, huc);
    place(m_controls[LfoFrequency], m_lfo, lfo);
    for(int ch = 0; ch < 6; ++ch)
      for(auto f : {Enabled, Level, ChannelTuning, Left, Right, Noise, NoisePeriod}) {
        bool visible = ch == m_selected && !source;
        if(f == Left || f == Right) visible &= huc;
        if(f == Noise) visible &= huc && ch >= 4;
        if(f == NoisePeriod) visible &= noise;
        if(f == ChannelTuning) visible &= !noise;
        place(m_controls[channelControl(ch, f)], m_detail, visible);
      }
    place(m_controls[LfoPeriod], m_detail, source);
    const int waveChannel = model == Model::Scc && m_selected == 4 ? 3 : m_selected;
    for(int ch = 0; ch < 6; ++ch) {
      m_waves[ch].control = nullptr;
      place(m_waves[ch].container, m_editor, !noise && ch == waveChannel);
    }
    auto& wave = m_waves[waveChannel];
    wave.control = m_waveform;
    m_waveform->setParentItem(wave.container);
    auto& inlet = *static_cast<Dataflow::WaveformInlet*>(m_model.inlets()[channelControl(waveChannel, Wave) + 1]);
    if(m_waveChannel != waveChannel) {
      Dataflow::setWaveformItemModel(*m_waveform, inlet); m_waveChannel = waveChannel;
    }
    Process::ControlPresentation presentation;
    presentation.label = source ? QStringLiteral("LFO source wave")
        : model == Model::Scc && m_selected >= 3 ? QStringLiteral("Shared wave: channels 4 / 5")
        : QStringLiteral("Wave");
    // Size the stock control, then let its factory place the port and label.
    // No parked channel participates in the panel's intrinsic measurement.
    Dataflow::setWaveformItemRect(*m_waveform, {0., 0., 430., 145.});
    m_waveFactories[waveChannel]->layoutFullItem(wave, inlet, presentation);
    layoutEditor();
    // Parked controls must not contribute their old geometry to the panel.
    setRect(m_main->mapRectToParent(m_main->boundingRect()));
  }
  const ProcessModel& m_model;
  std::optional<LayoutState> m_layoutState;
  std::array<QGraphicsItem*, ControlCount> m_controls{};
  std::array<Process::ControlLayout, 6> m_waves{};
  std::array<Process::PortFactory*, 6> m_waveFactories{};
  std::array<score::EmptyRectItem*, 6> m_pages{};
  std::vector<score::GraphicsLayout*> m_layouts;
  score::EmptyRectItem* m_hidden{};
  score::GraphicsVBoxLayout *m_main{}, *m_editor{};
  score::GraphicsHBoxLayout *m_setup{}, *m_lfo{}, *m_detail{};
  score::GraphicsTabLayout* m_tabs{};
  QGraphicsItem* m_waveform{};
  int m_selected{}, m_waveChannel{-1}, m_channelCount{};
};
}
LayerFactory::~LayerFactory() = default;
UuidKey<Process::ProcessModel> LayerFactory::concreteKey() const noexcept { return Metadata<ConcreteKey_k, ProcessModel>::get(); }
bool LayerFactory::matches(const UuidKey<Process::ProcessModel>& key) const { return key == concreteKey(); }
std::optional<double> LayerFactory::recommendedHeight() const noexcept { return 330.; }
score::ResizeableItem* LayerFactory::makeItem(const Process::ProcessModel& p, const Process::Context& ctx, QGraphicsItem* parent) const { return new LayerItem{static_cast<const ProcessModel&>(p), ctx, parent}; }
}
