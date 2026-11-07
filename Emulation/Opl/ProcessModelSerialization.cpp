#include "ProcessModel.hpp"
#include "Controls.hpp"
#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <score/application/ApplicationComponents.hpp>
#include <score/serialization/DataStreamVisitor.hpp>
#include <score/serialization/JSONValueVisitor.hpp>
#include <score/serialization/JSONVisitor.hpp>
#include <stdexcept>

// The edited inline WOPL is authoritative. The external path is provenance only:
// restoring a document never reloads it and never substitutes factory presets.
template <>
void DataStreamReader::read(const Emulation::Opl::ProcessModel& proc)
{
  m_stream << quint32{4} << QStringLiteral("ymfm");
  m_stream << proc.storedPath() << proc.bankData();
  readPorts(*this, proc.m_inlets, proc.m_outlets);
  insertDelimiter();
}
template <>
void DataStreamWriter::write(Emulation::Opl::ProcessModel& proc)
{
  quint32 schema{};
  QString backend;
  m_stream >> schema >> backend;
  if((schema < 1 || schema > 4) || backend != QStringLiteral("ymfm"))
    throw std::runtime_error("Unsupported OPL process schema or emulator backend");
  QByteArray bank;
  m_stream >> proc.m_filePath >> bank;
  writePorts(*this, components.interfaces<Process::PortFactoryList>(),
      proc.m_inlets, proc.m_outlets, &proc);
  if(schema == 1 && proc.m_inlets.size() == Emulation::Opl::Volume + 1)
    proc.m_inlets.push_back(Emulation::Opl::makeVolumeControl(&proc));
  if(schema <= 2 && proc.m_inlets.size() == Emulation::Opl::InstrumentIndex + 1)
    proc.m_inlets.push_back(Emulation::Opl::makeInstrumentIndexControl(&proc));
  if(schema <= 3 && proc.m_inlets.size() == Emulation::Opl::VoiceAllocation + 1)
    proc.m_inlets.push_back(Emulation::Opl::makeAllocationControl(&proc));
  proc.restoreBank(bank);
  checkDelimiter();
}
template <>
void JSONReader::read(const Emulation::Opl::ProcessModel& proc)
{
  obj["Schema"] = 4;
  obj["Backend"] = QStringLiteral("ymfm");
  obj["File"] = proc.storedPath();
  obj["WOPL"] = QString::fromLatin1(proc.bankData().toBase64());
  readPorts(*this, proc.m_inlets, proc.m_outlets);
}
template <>
void JSONWriter::write(Emulation::Opl::ProcessModel& proc)
{
  const auto schema = obj.tryGet("Schema");
  const auto backend = obj.tryGet("Backend");
  if(!schema || (schema->toInt() < 1 || schema->toInt() > 4)
      || !backend || backend->toString() != QStringLiteral("ymfm"))
    throw std::runtime_error("Unsupported OPL process schema or emulator backend");
  if(auto file = obj.tryGet("File")) proc.m_filePath = file->toString();
  QByteArray bank;
  if(auto encoded = obj.tryGet("WOPL"))
    bank = QByteArray::fromBase64(encoded->toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
  writePorts(*this, components.interfaces<Process::PortFactoryList>(),
      proc.m_inlets, proc.m_outlets, &proc);
  if(schema->toInt() == 1 && proc.m_inlets.size() == Emulation::Opl::Volume + 1)
    proc.m_inlets.push_back(Emulation::Opl::makeVolumeControl(&proc));
  if(schema->toInt() <= 2 && proc.m_inlets.size() == Emulation::Opl::InstrumentIndex + 1)
    proc.m_inlets.push_back(Emulation::Opl::makeInstrumentIndexControl(&proc));
  if(schema->toInt() <= 3 && proc.m_inlets.size() == Emulation::Opl::VoiceAllocation + 1)
    proc.m_inlets.push_back(Emulation::Opl::makeAllocationControl(&proc));
  proc.restoreBank(bank);
}
