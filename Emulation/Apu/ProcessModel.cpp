// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProcessModel.hpp"
#include "Controls.hpp"
#include <Emulation/WaveformInlet.hpp>
#include <Process/Preset.hpp>
#include <score/document/DocumentInterface.hpp>
#include <score/tools/FilePath.hpp>
#include <score/tools/ThreadPool.hpp>
#include <score/serialization/VisitorCommon.hpp>
#include <core/document/Document.hpp>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QScopedValueRollback>
#include <cstring>
#include <stdexcept>
#include <wobjectimpl.h>
W_OBJECT_IMPL(Emulation::Apu::ProcessModel)
namespace Emulation::Apu {
namespace {
Process::ControlInlet& control(const ProcessModel& p,int c) { return *static_cast<Process::ControlInlet*>(p.inlets()[c+1]); }
constexpr const char* roleKeys[]{"Duty","Length","Level","EnvelopePeriod","LengthEnable","EnvelopeUp","SweepPeriod","SweepShift","SweepNegate","Rate","NoiseShort","Left","Right","Linear","EnvelopeEnable","SweepEnable"};
constexpr const char* scalarKeys[]{"Model","Clock","Volume",nullptr,nullptr,"DmcAddress","DmcLength","DmcLevel","DmcLoop"};
bool booleanField(int f) { return f==LengthEnable||f==EnvelopeUp||f==SweepNegate||f==NoiseShort||f==Left||f==Right||f==EnvelopeEnable||f==SweepEnable; }
double number(const QJsonObject& o,const char* key,bool boolean=false) {
  const auto v=o.value(QString::fromLatin1(key));
  if(boolean) { if(!v.isBool())throw std::runtime_error("Invalid APU boolean field."); return v.toBool(); }
  if(!v.isDouble()||!std::isfinite(v.toDouble()))throw std::runtime_error("Invalid APU numeric field.");
  return v.toDouble();
}
}
ProcessModel::ProcessModel(const TimeVal& d,const QString& data,const Id<Process::ProcessModel>& id,QObject* p)
  : Process::ProcessModel{d,id,Metadata<ObjectKey_k,ProcessModel>::get(),p} {
  metadata().setInstanceName(*this);
  m_inlets.push_back(new Process::MidiInlet{"MIDI In",Id<Process::Port>{0},this});
  for(auto* c:makeControls(this))m_inlets.push_back(c);
  auto* out=new Process::AudioOutlet{"Stereo Out",Id<Process::Port>{0},this};out->setPropagate(true);m_outlets.push_back(out);
  wireControls();if(!data.isEmpty())importDmc(data);
}
ProcessModel::~ProcessModel() { if(m_cancel)m_cancel->store(true,std::memory_order_relaxed); }
void ProcessModel::refreshError() {
  QString error=m_resourceError;
  for(const auto* message:{&m_controlError,&m_engineError}) {
    if(message->isEmpty())continue;
    if(!error.isEmpty())error+=QLatin1Char('\n');
    error+=*message;
  }
  if(error==m_error)return;m_error=std::move(error);errorChanged();
}
void ProcessModel::setEngineError(QString e) {if(e==m_engineError)return;m_engineError=std::move(e);refreshError();}
void ProcessModel::setControlError(QString e) {if(e==m_controlError)return;m_controlError=std::move(e);refreshError();}
void ProcessModel::setResourceError(QString e) {if(e==m_resourceError)return;m_resourceError=std::move(e);refreshError();}
void ProcessModel::wireControls() {
  if(m_inlets.size()!=ControlCount+1||m_outlets.size()!=1||!qobject_cast<Process::MidiInlet*>(m_inlets[0])
      ||!qobject_cast<Process::AudioOutlet*>(m_outlets[0]))throw std::runtime_error("Invalid APU port layout.");
  for(int c=0;c<ControlCount;++c)
    if(!qobject_cast<Process::ControlInlet*>(m_inlets[c+1])||m_inlets[c+1]->id().val()!=c+1)throw std::runtime_error("Invalid APU control ID.");
  auto* wave=dynamic_cast<Dataflow::WaveformInlet*>(&control(*this,Wave));
  if(!wave||wave->supportsFrames()||wave->frameSizeConstraint()!=32||wave->amplitudeLevels()!=16
      ||wave->wavetable().frameSize!=32||wave->wavetable().frameCount()!=1
      ||!qobject_cast<Process::FileChooser*>(&control(*this,DmcFile)))
    throw std::runtime_error("Invalid APU asset port types or waveform constraints.");
  if(!Process::prepareWaveform(wave->wavetable(),m_settings.wave,0,15))
    throw std::runtime_error("Invalid APU waveform samples.");
  syncControls();
  for(int c=0;c<ControlCount;++c)connect(&control(*this,c),&Process::ControlInlet::valueChanged,this,[this,c](const ossia::value&){if(!m_syncing)controlChanged(c);});
}
void ProcessModel::syncControls() {
  QScopedValueRollback guard{m_syncing,true};
  for(int c=0;c<ControlCount;++c) {
    updateControlMetadata(control(*this,c),m_settings.model,c);
    if(c==DmcFile) { control(*this,c).setValue(std::string{});continue; }
    if(c==Wave)continue;
    const auto v=controlValue(m_settings,c);
    if(c==Volume)control(*this,c).setValue(float(v));
    else if(qobject_cast<Process::Toggle*>(&control(*this,c)))control(*this,c).setValue(bool(v));
    else control(*this,c).setValue(int(v));
  }
}
void ProcessModel::controlChanged(int c) {
  if(c==DmcFile) { importDmc(QString::fromStdString(ossia::convert<std::string>(control(*this,c).value())));return; }
  auto next=m_settings;
  const bool accepted=c==Wave
    ?Process::prepareWaveform(static_cast<Dataflow::WaveformInlet&>(control(*this,c)).wavetable(),next.wave,0,15)
    :setControl(next,c,control(*this,c).value());
  if(!accepted) {syncControls();setControlError(QStringLiteral("Invalid APU control value."));return;}
  if(const auto* e=validate(next)) {syncControls();setControlError(QString::fromUtf8(e));return;}
  if(next==m_settings){setControlError({});return;}
  m_settings=next; if(c==ChipModel)syncControls();setControlError({});settingsChanged();
}
QByteArray ProcessModel::stateData() const {
  QJsonObject o{{"Schema",5},{"Backend","console-apu"},{"Routing",int(m_settings.routing)}};
  for(int c=0;c<RoleBase;++c)if(scalarKeys[c]) {
    if(c==DmcLoop)o[scalarKeys[c]]=m_settings.dmcLoop;
    else o[scalarKeys[c]]=controlValue(m_settings,c);
  }
  const auto roles=[](const std::array<Role,5>& bank) {
    Settings settings;settings.roles=bank;QJsonArray result;
    for(int r=0;r<5;++r) {QJsonObject role;for(int f=0;f<RoleFieldCount;++f) {
      const auto v=controlValue(settings,roleControl(r,RoleField(f)));
      if(booleanField(f))role[roleKeys[f]]=bool(v);else role[roleKeys[f]]=v;
    }result.append(role);}
    return result;
  };
  const bool gb=gameBoy(m_settings.model);
  o["NesRoles"]=roles(gb?m_settings.inactiveRoles:m_settings.roles);
  o["GbRoles"]=roles(gb?m_settings.roles:m_settings.inactiveRoles);
  o["NesSound"]=int(gb?m_settings.inactiveSound:m_settings.sound);
  o["GbSound"]=int(gb?m_settings.sound:m_settings.inactiveSound);
  o["Wave"]=QJsonDocument::fromJson(score::marshall<JSONObject>(control(*this,Wave).value()).toByteArray()).object();
  o["DmcData"]=QString::fromLatin1(QByteArray{reinterpret_cast<const char*>(m_dmc.bytes.data()),m_dmc.size}.toBase64());
  return QJsonDocument{o}.toJson(QJsonDocument::Compact);
}
void ProcessModel::restoreState(const QByteArray& data) {
  const auto doc=QJsonDocument::fromJson(data);const auto o=doc.object();
  const auto schema=number(o,"Schema");
  if(!doc.isObject()||schema!=5||o["Backend"].toString()!=QStringLiteral("console-apu"))throw std::runtime_error("Unsupported APU state schema.");
  Settings next;
  for(int c=0;c<RoleBase;++c)if(scalarKeys[c])if(!setScalar(next,c,number(o,scalarKeys[c],c==DmcLoop)))throw std::runtime_error("Invalid APU settings.");
  const auto readRoles=[&](const QJsonValue& value) {
    if(!value.isArray()||value.toArray().size()!=5)throw std::runtime_error("APU state needs five role records per architecture.");
    const auto roles=value.toArray();
    for(int r=0;r<5;++r) {if(!roles[r].isObject())throw std::runtime_error("Invalid APU role.");for(int f=0;f<RoleFieldCount;++f)
      if(!setScalar(next,roleControl(r,RoleField(f)),number(roles[r].toObject(),roleKeys[f],booleanField(f))))throw std::runtime_error("Invalid APU role field.");}
  };
  const auto model=next.model;
  selectModel(next,Model::NesNtsc);readRoles(o["NesRoles"]);
  selectModel(next,Model::Dmg);readRoles(o["GbRoles"]);
  selectModel(next,model);
  if(!setScalar(next,Routing,number(o,"Routing")))throw std::runtime_error("Invalid APU routing mode.");
  selectModel(next,Model::NesNtsc);
  if(!setScalar(next,Sound,number(o,"NesSound")))throw std::runtime_error("Invalid NES sound.");
  selectModel(next,Model::Dmg);
  if(!setScalar(next,Sound,number(o,"GbSound")))throw std::runtime_error("Invalid Game Boy sound.");
  selectModel(next,model);
  if(!o["Wave"].isObject())throw std::runtime_error("APU wave requires a sample table.");
  const auto json=readJson(QJsonDocument{o["Wave"].toObject()}.toJson(QJsonDocument::Compact));
  const auto value=score::unmarshall<ossia::value>(json);
  const auto table=Process::wavetableFromValue(value);
  if(!table||table->frameSize!=32||table->frameCount()!=1
      ||!Process::prepareWaveform(*table,next.wave,0,15))
    throw std::runtime_error("APU wave requires one frame of 32 normalized samples.");
  if(m_inlets.size()!=ControlCount+1)throw std::runtime_error("Invalid APU port layout.");
  auto* inlet=dynamic_cast<Dataflow::WaveformInlet*>(m_inlets[Wave+1]);
  if(!inlet||inlet->supportsFrames()||inlet->frameSizeConstraint()!=32||inlet->amplitudeLevels()!=16)
    throw std::runtime_error("Invalid APU waveform inlet.");
  if(!o["DmcData"].isString())throw std::runtime_error("Missing embedded DMC data.");
  const auto bytes=QByteArray::fromBase64(o["DmcData"].toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
  if(bytes.isEmpty()||bytes.size()>DmcData::capacity)throw std::runtime_error("DMC data must contain 1..16384 bytes.");
  DmcData asset;asset.size=bytes.size();std::memcpy(asset.bytes.data(),bytes.constData(),bytes.size());
  if(const auto* e=validate(next))throw std::runtime_error(e);
  if(const auto* e=validate(asset))throw std::runtime_error(e);
  if(m_cancel)m_cancel->store(true,std::memory_order_relaxed);
  QScopedValueRollback guard{m_syncing,true};
  m_settings=next;m_dmc=asset;inlet->setWaveform(*table);
}
Process::Preset ProcessModel::savePreset() const noexcept {
  Process::Preset p;p.name=metadata().getName();p.key={Metadata<ConcreteKey_k,ProcessModel>::get(),{}};p.data=stateData();return p;
}
void ProcessModel::loadPreset(const Process::Preset& p) {
  try {restoreState(p.data);syncControls();setControlError({});setResourceError({});settingsChanged();assetChanged();}
  catch(const std::exception& e){setResourceError(QString::fromUtf8(e.what()));}
}
bool ProcessModel::importDmcBytes(const QByteArray& bytes) {
  if(bytes.isEmpty()||bytes.size()>DmcData::capacity){setResourceError(QStringLiteral("Raw DMC must contain 1..16384 bytes."));return false;}
  DmcData next;next.size=bytes.size();std::memcpy(next.bytes.data(),bytes.constData(),bytes.size());
  if(const auto* e=validate(next)){setResourceError(QString::fromUtf8(e));return false;}
  m_dmc=next;setResourceError({});assetChanged();return true;
}
void ProcessModel::importDmc(const QString& path) {
  if(m_cancel)m_cancel->store(true,std::memory_order_relaxed);
  auto token=m_cancel=std::make_shared<std::atomic<bool>>(false);
  {QScopedValueRollback guard{m_syncing,true};control(*this,DmcFile).setValue(std::string{});}
  if(path.isEmpty())return;
  QString resolved;
  if(auto* doc=score::IDocument::try_documentFromObject(*this))resolved=score::locateFilePath(path,doc->context());
  else resolved=score::locateFilePath(path);
  QPointer<ProcessModel> self{this};
  score::TaskPool::instance().post([self,token,resolved] {
    QByteArray bytes;QString error;
    QFile file{resolved};
    if(!file.open(QIODevice::ReadOnly))error=file.errorString();
    else if(file.size()<1||file.size()>DmcData::capacity)error=QStringLiteral("Raw DMC must contain 1..16384 bytes.");
    else {bytes=file.read(DmcData::capacity+1);if(bytes.size()!=file.size()||bytes.isEmpty()||bytes.size()>DmcData::capacity)error=QStringLiteral("Could not read complete raw DMC data.");}
    if(token->load(std::memory_order_relaxed))return;
    QMetaObject::invokeMethod(qApp,[self,token,bytes=std::move(bytes),error] {
      if(!self||token->load(std::memory_order_relaxed))return;
      if(!error.isEmpty())self->setResourceError(error);else self->importDmcBytes(bytes);
    },Qt::QueuedConnection);
  });
}
}
