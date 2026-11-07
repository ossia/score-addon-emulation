#include "ProcessModel.hpp"
#include "Controls.hpp"
#include <Emulation/Volume.hpp>
#include <Process/Dataflow/Port.hpp>
#include <score/serialization/JSONValueVisitor.hpp>
#include <score/serialization/JSONVisitor.hpp>
#include <score/document/DocumentInterface.hpp>
#include <score/tools/FilePath.hpp>
#include <score/tools/ThreadPool.hpp>
#include <core/document/Document.hpp>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QPointer>
#include <QScopedValueRollback>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <wobjectimpl.h>
W_OBJECT_IMPL(Emulation::Opl::ProcessModel)

namespace Emulation::Opl
{
namespace
{
constexpr qint64 maxBankBytes = 16 * 1024 * 1024;
Process::ControlInlet& control(const ProcessModel& p, int c)
{ return *static_cast<Process::ControlInlet*>(p.inlets()[c + 1]); }
int value(const ProcessModel& p, int c)
{ return ossia::convert<int>(control(p, c).value()); }
}
ProcessModel::ProcessModel(const TimeVal& duration, const QString& data,
    const Id<Process::ProcessModel>& id, QObject* parent)
    : Process::ProcessModel{duration, id, Metadata<ObjectKey_k, ProcessModel>::get(), parent}
    , m_bank{std::make_shared<Bank>(factoryBank())}
{
  metadata().setInstanceName(*this);
  m_inlets.push_back(new Process::MidiInlet{"MIDI In", Id<Process::Port>{0}, this});
  for(auto* c : makeControls(this)) m_inlets.push_back(c);
  auto* out = new Process::AudioOutlet{"Stereo Out", Id<Process::Port>{0}, this};
  out->setPropagate(true);
  m_outlets.push_back(out);
  wireControls();
  if(!data.isEmpty()) loadFile(data);
}
ProcessModel::~ProcessModel()
{
  if(m_cancelToken) m_cancelToken->store(true, std::memory_order_relaxed);
}
QString ProcessModel::effect() const noexcept { return m_filePath; }
Settings ProcessModel::settings() const
{
  return {static_cast<Model>(value(*this, ChipModel)), value(*this, Chips),
      value(*this, FourOps), bool(value(*this, DeepVibrato)),
      bool(value(*this, DeepTremolo)), bool(value(*this, SoftPan))};
}
Address ProcessModel::selectedAddress() const
{
  return {bool(value(*this, Percussion)), static_cast<uint8_t>(value(*this, BankMSB)),
      static_cast<uint8_t>(value(*this, BankLSB)), static_cast<uint8_t>(value(*this, ProgramNumber))};
}
QString ProcessModel::selectedName() const
{
  if(m_bank)
    if(auto* p = findProgram(*m_bank, selectedAddress()))
      return QString::fromStdString(p->name)
          + ((p->instrument.inst_flags & ADLMIDI_Ins_IsBlank) ? QStringLiteral(" (blank)") : QString{});
  return QStringLiteral("No patch at this bank/program address");
}
bool ProcessModel::presetMatches(const Process::Preset& preset) const noexcept
{
  return preset.key.key == concreteKey();
}
Process::Preset ProcessModel::savePreset() const noexcept
{
  auto preset = Process::ProcessModel::savePreset();
  preset.key = {Metadata<ConcreteKey_k, ProcessModel>::get(), {}};
  preset.data = QJsonDocument{QJsonObject{
      {QStringLiteral("Schema"), 1},
      {QStringLiteral("Backend"), QStringLiteral("ymfm")},
      {QStringLiteral("WOPL"), QString::fromLatin1(bankData().toBase64())},
      {QStringLiteral("Controls"), QJsonDocument::fromJson(preset.data).array()}}}.toJson(QJsonDocument::Compact);
  return preset;
}
void ProcessModel::loadPreset(const Process::Preset& preset)
{
  const auto json = QJsonDocument::fromJson(preset.data);
  if(!json.isObject() && !json.isArray())
  { setEditError(QStringLiteral("Invalid OPL preset.")); return; }
  auto controls = preset;
  std::shared_ptr<Bank> bank;
  if(json.isObject())
  {
    const auto object = json.object();
    if(object.value(QStringLiteral("Schema")).toInt() != 1
       || object.value(QStringLiteral("Backend")).toString() != QStringLiteral("ymfm")
       || !object.value(QStringLiteral("Controls")).isArray()
       || !object.value(QStringLiteral("WOPL")).isString())
    { setEditError(QStringLiteral("Unsupported OPL preset schema.")); return; }
    const auto bytes = QByteArray::fromBase64(object.value(QStringLiteral("WOPL")).toString().toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
    try
    {
      if(bytes.size() > maxBankBytes
         || (bytes.isEmpty() && !object.value(QStringLiteral("WOPL")).toString().isEmpty()))
        throw std::runtime_error("Invalid inline WOPL preset data.");
      if(!bytes.isEmpty())
        bank = std::make_shared<Bank>(decodeBank({reinterpret_cast<const uint8_t*>(bytes.constData()),
            std::size_t(bytes.size())}));
    }
    catch(const std::exception& e) { setEditError(QString::fromUtf8(e.what())); return; }
    controls.data = QJsonDocument{object.value(QStringLiteral("Controls")).toArray()}.toJson(QJsonDocument::Compact);
  }
  else if(m_bank) bank = std::make_shared<Bank>(*m_bank);
  std::array<ossia::value, ControlCount> staged;
  bool hasInstrumentIndex{};
  for(int c = 0; c < ControlCount; ++c) staged[c] = control(*this, c).value();
  // Older presets must restore native Poly, not inherit the current live mode.
  staged[VoiceAllocation] = 1;
  try
  {
    const auto data = readJson(controls.data);
    if(!data.IsArray()) throw std::runtime_error("Invalid OPL preset controls.");
    for(const auto& entry : data.GetArray())
    {
      if(!entry.IsArray() || entry.Size() != 2 || !entry[0].IsInt()
         || entry[0].GetInt() < 1 || entry[0].GetInt() > ControlCount)
        throw std::runtime_error("Invalid OPL preset control entry.");
      staged[entry[0].GetInt() - 1] = JsonValue{entry[1]}.to<ossia::value>();
      hasInstrumentIndex |= entry[0].GetInt() == InstrumentIndex + 1;
    }
    ADL_Instrument registers{};
    registers.version = ADLMIDI_InstrumentVersion;
    for(int c = 0; c < ControlCount; ++c)
    {
      const auto type = staged[c].get_type();
      if(c == File)
      {
        if(type != ossia::val_type::STRING) throw std::runtime_error("Invalid OPL bank path.");
        continue;
      }
      if(type != ossia::val_type::INT && type != ossia::val_type::FLOAT
         && type != ossia::val_type::BOOL)
        throw std::runtime_error("Invalid OPL numeric control.");
      const double number = ossia::convert<double>(staged[c]);
      if(!std::isfinite(number)) throw std::runtime_error("OPL controls must be finite.");
      if(c == Volume)
      {
        if(number < 0. || number > MaxVolume) throw std::runtime_error("OPL volume must be in [0, 32].");
        staged[c] = float(number);
        continue;
      }
      if(number != std::floor(number) || number < std::numeric_limits<int>::min()
         || number > std::numeric_limits<int>::max())
        throw std::runtime_error("OPL register controls must be integers.");
      const int n = int(number);
      if(dynamic_cast<Process::Toggle*>(&control(*this, c)))
      {
        if(n != 0 && n != 1) throw std::runtime_error("Invalid OPL toggle value.");
        staged[c] = bool(n);
      }
      else staged[c] = n;
      if(isInstrumentControl(c))
      {
        setInstrumentControl(registers, c, n);
        if(instrumentControlValue(registers, c) != n)
          throw std::runtime_error("OPL register value is outside its hardware domain.");
      }
    }
    const auto number = [&](int c) { return ossia::convert<int>(staged[c]); };
    if(number(VoiceAllocation) < 0 || number(VoiceAllocation) > 2)
      throw std::runtime_error("Invalid OPL voice allocation mode.");
    const Settings next{static_cast<Model>(number(ChipModel)), number(Chips), number(FourOps),
        bool(number(DeepVibrato)), bool(number(DeepTremolo)), bool(number(SoftPan))};
    if(const auto* error = settingsError(next)) throw std::runtime_error(error);
    if(next.chips > 16 || next.fourOpChannels > 96)
      throw std::runtime_error("OPL process allocation exceeds 16 chips / 96 four-op channels.");
    if(number(BankMSB) < 0 || number(BankMSB) > 127
       || number(BankLSB) < 0 || number(BankLSB) > 255
       || number(ProgramNumber) < 0 || number(ProgramNumber) > 127)
      throw std::runtime_error("Invalid OPL bank address.");
    if(number(VoiceMode) == 2 || number(RhythmMode) > 5)
      throw std::runtime_error("Unsupported OPL voice or rhythm mode.");
    const Address address{bool(number(Percussion)), uint8_t(number(BankMSB)),
        uint8_t(number(BankLSB)), uint8_t(number(ProgramNumber))};
    if((address.percussion && address.msb != 0) || (!address.percussion && address.lsb > 127))
      throw std::runtime_error("OPL bank address cannot be selected by MIDI.");
    if(const auto* error = instrumentError(registers, address, next))
      throw std::runtime_error(error);
    if(hasInstrumentIndex)
    {
      const auto selected = instrumentAddress(bank.get(), number(InstrumentIndex));
      if(!selected || *selected != address)
        throw std::runtime_error("OPL preset instrument does not match its bank address.");
    }
    // Legacy fixed-control presets own only their selected instrument's registers.
    // Validate the prospective bank before publishing controls or cancelling imports.
    if(json.isArray() && bank)
      if(auto* patch = findProgram(*bank, address))
        for(int c = VoiceMode; c < InstrumentControlEnd; ++c)
          setInstrumentControl(patch->instrument, c, number(c));
    if(bank)
      if(const auto* error = bankError(*bank, next)) throw std::runtime_error(error);
  }
  catch(const std::exception& e) { setEditError(QString::fromUtf8(e.what())); return; }
  if(m_cancelToken) m_cancelToken->store(true, std::memory_order_relaxed);
  {
    QScopedValueRollback guard{m_syncing, true};
    for(int c = 0; c < ControlCount; ++c) control(*this, c).setValue(staged[c]);
    m_filePath = QString::fromStdString(ossia::convert<std::string>(staged[File]));
    m_bank = std::move(bank);
  }
  m_acceptedSettings = settings();
  m_acceptedVolume = ossia::convert<float>(control(*this, Volume).value());
  syncPatchControls();
  setEditError({});
  setRuntimeError({});
  setEngineError({});
  setResourceError(m_bank ? QString{} : QStringLiteral("No inline WOPL bank was saved."));
  bankChanged();
  selectedAddressChanged();
}
QString ProcessModel::error() const
{
  QStringList errors;
  for(const auto* error : {&m_resourceError, &m_engineError, &m_editError, &m_runtimeError})
    if(!error->isEmpty() && !errors.contains(*error)) errors.push_back(*error);
  return errors.join(QLatin1Char('\n'));
}
void ProcessModel::setEngineError(QString error)
{
  if(m_engineError == error) return;
  m_engineError = std::move(error);
  errorChanged();
}
void ProcessModel::setRuntimeError(QString error)
{
  if(m_runtimeError == error) return;
  m_runtimeError = std::move(error);
  errorChanged();
}
void ProcessModel::setEditError(QString error)
{
  if(m_editError == error) return;
  m_editError = std::move(error);
  errorChanged();
}
void ProcessModel::setResourceError(QString error)
{
  if(m_resourceError == error) return;
  m_resourceError = std::move(error);
  errorChanged();
}
void ProcessModel::syncDomains()
{
  QScopedValueRollback guard{m_syncing, true};
  const auto config = settings();
  control(*this, FourOps).setDomain(ossia::make_domain(0,
      config.model == Model::Opl3 ? config.chips * 6 : 0));
  for(int op = 0; op < 4; ++op)
    control(*this, operatorControl(op, Waveform)).setDomain(
        ossia::make_domain(0, config.model == Model::Opl2 ? 3 : 7));
  control(*this, BankMSB).setDomain(ossia::make_domain(0, selectedAddress().percussion ? 0 : 127));
  control(*this, BankLSB).setDomain(ossia::make_domain(0, selectedAddress().percussion ? 255 : 127));
  std::vector<std::pair<QString, ossia::value>> modes{{QStringLiteral("2 operators"), 0}};
  const bool rhythm = value(*this, RhythmMode) != 0;
  if(!rhythm && config.model == Model::Opl3 && config.fourOpChannels > 0)
    modes.emplace_back(QStringLiteral("4 operators"), 1);
  if(!rhythm) modes.emplace_back(QStringLiteral("Dual 2-op"), 3);
  const int current = value(*this, VoiceMode);
  if(std::none_of(modes.begin(), modes.end(), [current](const auto& mode) {
       return ossia::convert<int>(mode.second) == current;
     }))
    modes.emplace_back(QStringLiteral("Unavailable mode (%1)").arg(current), current);
  static_cast<Process::ComboBox&>(control(*this, VoiceMode)).setAlternatives(std::move(modes));
}
void ProcessModel::wireControls()
{
  if(m_inlets.size() != ControlCount + 1)
    throw std::runtime_error("Invalid OPL control schema.");
  const double volume = ossia::convert<double>(control(*this, Volume).value());
  if(!std::isfinite(volume) || volume < 0. || volume > MaxVolume)
    throw std::runtime_error("OPL volume must be finite and in [0, 32].");
  m_acceptedVolume = float(volume);
  // Restore the current domain without rescaling values, IDs or automation.
  control(*this, Volume).setDomain(ossia::make_domain(0.f, MaxVolume));
  { QScopedValueRollback guard{m_syncing, true};
    control(*this, File).setValue(m_filePath.toStdString()); }
  syncPatchControls();
  const struct { int id; const char* oldName; const char* name; } names[]{
      {ChipModel, "Chip model", "OPL chip model"}, {Chips, "Chips", "OPL chip count"},
      {FourOps, "Four-op channels", "OPL four-op channels"},
      {Feedback1, "Feedback 1", "Voice 1 feedback"},
      {Feedback2, "Feedback 2", "Voice 2 feedback"},
      {Connection1, "Additive 1", "Voice 1 additive connection"},
      {Connection2, "Additive 2", "Voice 2 additive connection"},
      {Blank, "Blank", "Instrument blank"}};
  for(const auto& name : names)
    if(control(*this, name.id).name() == QString::fromLatin1(name.oldName))
      control(*this, name.id).setName(QString::fromLatin1(name.name));
  for(int op = 0; op < 4; ++op)
  {
    const auto prefix = QStringLiteral("Op %1 ").arg(op + 1);
    auto& multiplier = control(*this, operatorControl(op, Multiplier));
    if(multiplier.name() == prefix + QStringLiteral("Multiplier"))
      multiplier.setName(prefix + QStringLiteral("Multiplier code"));
    auto& attenuation = control(*this, operatorControl(op, Level));
    if(attenuation.name() == prefix + QStringLiteral("Attenuation"))
      attenuation.setName(prefix + QStringLiteral("Attenuation (0.75 dB steps)"));
  }
  m_acceptedSettings = settings();
  syncDomains();
  for(int c = 0; c < ControlCount; ++c)
    connect(&control(*this, c), &Process::ControlInlet::valueChanged, this,
        [this, c](const ossia::value&) { if(!m_syncing) controlChanged(c); });
}
void ProcessModel::syncPatchControls()
{
  QScopedValueRollback guard{m_syncing, true};
  const auto* p = m_bank ? findProgram(*m_bank, selectedAddress()) : nullptr;
  const int selection = p ? int(p - m_bank->programs.data()) : unresolvedInstrument(selectedAddress());
  control(*this, InstrumentIndex).setDomain(ossia::make_domain(
      std::min(0, selection), m_bank ? std::max(0, int(m_bank->programs.size()) - 1) : 0));
  control(*this, InstrumentIndex).setValue(selection);
  if(!p) { syncDomains(); return; }
  for(int c = VoiceMode; c < InstrumentControlEnd; ++c)
  {
    const int v = instrumentControlValue(p->instrument, c);
    // Keep the native toggle's boolean value type for widget synchronization.
    if(dynamic_cast<Process::Toggle*>(&control(*this, c))) control(*this, c).setValue(bool(v));
    else control(*this, c).setValue(v);
  }
  syncDomains();
}
void ProcessModel::controlChanged(int c)
{
  if(c == Volume)
  {
    const double volume = ossia::convert<double>(control(*this, c).value());
    if(!std::isfinite(volume) || volume < 0. || volume > MaxVolume)
    {
      QScopedValueRollback guard{m_syncing, true};
      control(*this, c).setValue(m_acceptedVolume);
      setEditError(QStringLiteral("OPL volume must be finite and in [0, 32]."));
      return;
    }
    m_acceptedVolume = float(volume);
    setEditError({});
    return;
  }
  if(c == File)
  {
    loadFile(QString::fromStdString(ossia::convert<std::string>(control(*this, c).value())));
    return;
  }
  if(c == InstrumentIndex)
  {
    const auto selected = instrumentAddress(m_bank.get(), value(*this, InstrumentIndex));
    if(!selected)
    {
      syncPatchControls();
      setEditError(QStringLiteral("Instrument index is outside the loaded WOPL bank."));
      return;
    }
    const auto address = *selected;
    {
      QScopedValueRollback guard{m_syncing, true};
      control(*this, BankMSB).setValue(int(address.msb));
      control(*this, BankLSB).setValue(int(address.lsb));
      control(*this, Percussion).setValue(address.percussion);
      control(*this, ProgramNumber).setValue(int(address.program));
    }
    syncPatchControls();
    setEditError(m_bank && findProgram(*m_bank, address) ? QString{}
        : QStringLiteral("No patch at this address. Select a bank/program present in the imported WOPL."));
    selectedAddressChanged();
    return;
  }
  if(c >= BankMSB && c <= ProgramNumber)
  {
    syncPatchControls();
    setEditError(m_bank && findProgram(*m_bank, selectedAddress()) ? QString{}
        : QStringLiteral("No patch at this address. Select a bank/program present in the imported WOPL."));
    selectedAddressChanged();
    return;
  }
  if(c == DeepVibrato || c == DeepTremolo)
  {
    if(m_bank)
    {
      if(m_bank.use_count() != 1) m_bank = std::make_shared<Bank>(*m_bank);
      // Native WOPL global flags: tremolo bit 0, vibrato bit 1.
      m_bank->flags = (m_bank->flags & ~3)
          | (value(*this, DeepTremolo) ? 1 : 0)
          | (value(*this, DeepVibrato) ? 2 : 0);
    }
    depthsChanged();
    return;
  }
  if(c <= SoftPan)
  {
    const auto next = settings();
    const char* error = m_bank ? bankError(*m_bank, next) : settingsError(next);
    if(error)
    {
      QScopedValueRollback guard{m_syncing, true};
      switch(c)
      {
        case ChipModel: control(*this, c).setValue(int(m_acceptedSettings.model)); break;
        case Chips: control(*this, c).setValue(m_acceptedSettings.chips); break;
        case FourOps: control(*this, c).setValue(m_acceptedSettings.fourOpChannels); break;
        case SoftPan: control(*this, c).setValue(m_acceptedSettings.softPan); break;
      }
      setEditError(QString::fromUtf8(error));
      return;
    }
    m_acceptedSettings = next;
    syncDomains();
    setEditError({});
    bankChanged();
    return;
  }
  if(!isInstrumentControl(c)) return;
  if(!m_bank || !findProgram(*m_bank, selectedAddress()))
  {
    setEditError(QStringLiteral("Cannot edit an absent patch. Import a WOPL containing this bank address."));
    return;
  }
  ADL_Instrument edited = findProgram(*m_bank, selectedAddress())->instrument;
  setInstrumentControl(edited, c, value(*this, c));
  if(const auto* error = instrumentError(edited, selectedAddress(), settings()))
  {
    setEditError(QString::fromUtf8(error));
    syncPatchControls();
    return;
  }
  if(m_bank.use_count() != 1) m_bank = std::make_shared<Bank>(*m_bank);
  findProgram(*m_bank, selectedAddress())->instrument = edited;
  setEditError({});
  syncDomains();
  if(c == RhythmMode) bankChanged();
  else instrumentChanged();
}
QString ProcessModel::storedPath() const
{
  if(auto* doc = score::IDocument::try_documentFromObject(*this))
    return score::relativizeFilePath(m_filePath, doc->context());
  return score::relativizeFilePath(m_filePath);
}
void ProcessModel::loadFile(const QString& path)
{
  if(m_cancelToken) m_cancelToken->store(true, std::memory_order_relaxed);
  auto token = m_cancelToken = std::make_shared<std::atomic<bool>>(false);
  m_filePath = path;
  { QScopedValueRollback guard{m_syncing, true}; control(*this, File).setValue(path.toStdString()); }
  // Never keep playing the previous bank or factory presets after an invalid import.
  m_bank.reset();
  syncPatchControls();
  bankChanged();
  setEditError({});
  setEngineError({});
  setRuntimeError({});
  setResourceError(path.isEmpty() ? QStringLiteral("No bank loaded.") : QStringLiteral("Loading WOPL"));
  if(path.isEmpty()) return;
  QString resolved;
  if(auto* doc = score::IDocument::try_documentFromObject(*this))
    resolved = score::locateFilePath(path, doc->context());
  else resolved = score::locateFilePath(path);
  QPointer<ProcessModel> self{this};
  score::TaskPool::instance().post([resolved, token, self] {
    std::shared_ptr<Bank> result;
    QString error;
    try
    {
      if(token->load(std::memory_order_relaxed)) return;
      if(QFileInfo{resolved}.suffix().compare(QStringLiteral("wopl"), Qt::CaseInsensitive) != 0)
        throw std::runtime_error("Only WOPL bank files are supported.");
      QFile file{resolved};
      if(!file.open(QIODevice::ReadOnly)) throw std::runtime_error(file.errorString().toStdString());
      if(file.size() <= 0 || file.size() > maxBankBytes)
        throw std::runtime_error("WOPL bank must be non-empty and at most 16 MiB.");
      auto bytes = file.read(maxBankBytes + 1);
      if(bytes.size() != file.size() || bytes.size() > maxBankBytes)
        throw std::runtime_error("Could not read the complete WOPL bank.");
      if(token->load(std::memory_order_relaxed)) return;
      result = std::make_shared<Bank>(decodeBank({reinterpret_cast<const uint8_t*>(bytes.constData()),
          static_cast<std::size_t>(bytes.size())}));
    }
    catch(const std::exception& e) { error = QString::fromUtf8(e.what()); }
    if(token->load(std::memory_order_relaxed)) return;
    QMetaObject::invokeMethod(qApp, [self, token, result = std::move(result), error] {
      if(token->load(std::memory_order_relaxed) || !self) return;
      self->m_bank = result;
      if(result)
      {
        QScopedValueRollback guard{self->m_syncing, true};
        control(*self, DeepTremolo).setValue(bool(result->flags & 1));
        control(*self, DeepVibrato).setValue(bool(result->flags & 2));
      }
      if(result && !result->programs.empty() && !findProgram(*result, self->selectedAddress()))
      {
        QScopedValueRollback guard{self->m_syncing, true};
        const auto address = result->programs.front().address;
        control(*self, BankMSB).setValue(int(address.msb));
        control(*self, BankLSB).setValue(int(address.lsb));
        control(*self, Percussion).setValue(address.percussion);
        control(*self, ProgramNumber).setValue(int(address.program));
      }
      self->syncPatchControls();
      self->setResourceError(error);
      if(result) self->setEditError({});
      self->bankChanged();
      self->selectedAddressChanged();
    }, Qt::QueuedConnection);
  });
}
QByteArray ProcessModel::bankData() const
{
  if(!m_bank) return {};
  const auto bytes = encodeBank(*m_bank);
  return QByteArray{reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())};
}
void ProcessModel::restoreBank(const QByteArray& bytes)
{
  m_bank.reset();
  try
  {
    if(bytes.isEmpty()) { setResourceError(QStringLiteral("No inline WOPL bank was saved.")); return; }
    if(bytes.size() > maxBankBytes) throw std::runtime_error("Inline WOPL exceeds the 16 MiB limit.");
    m_bank = std::make_shared<Bank>(decodeBank({reinterpret_cast<const uint8_t*>(bytes.constData()),
        static_cast<std::size_t>(bytes.size())}));
  }
  catch(const std::exception& e) { setResourceError(QString::fromUtf8(e.what())); }
}
}
