// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProcessModel.hpp"
#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <score/application/ApplicationComponents.hpp>
#include <score/serialization/DataStreamVisitor.hpp>
#include <score/serialization/JSONValueVisitor.hpp>
#include <score/serialization/JSONVisitor.hpp>
#include <stdexcept>

template<> void DataStreamReader::read(const Emulation::Sid::ProcessModel& p)
{
  m_stream << quint32{1} << QStringLiteral("libresidfp") << p.patchData();
  readPorts(*this, p.m_inlets, p.m_outlets);
  insertDelimiter();
}
template<> void DataStreamWriter::write(Emulation::Sid::ProcessModel& p)
{
  quint32 schema{};
  QString backend;
  QByteArray data;
  m_stream >> schema >> backend >> data;
  if(schema != 1 || backend != QStringLiteral("libresidfp"))
    throw std::runtime_error("Unsupported SID process schema or backend.");
  p.restorePatch(data);
  writePorts(*this, components.interfaces<Process::PortFactoryList>(), p.m_inlets, p.m_outlets, &p);
  checkDelimiter();
}
template<> void JSONReader::read(const Emulation::Sid::ProcessModel& p)
{
  obj["Schema"] = 1;
  obj["Backend"] = QStringLiteral("libresidfp");
  obj["Patch"] = QString::fromLatin1(p.patchData().toBase64());
  readPorts(*this, p.m_inlets, p.m_outlets);
}
template<> void JSONWriter::write(Emulation::Sid::ProcessModel& p)
{
  const auto schema = obj.tryGet("Schema");
  const auto backend = obj.tryGet("Backend");
  const auto patch = obj.tryGet("Patch");
  if(!schema || schema->toInt() != 1 || !backend || backend->toString() != QStringLiteral("libresidfp") || !patch)
    throw std::runtime_error("Unsupported SID process schema or backend.");
  p.restorePatch(QByteArray::fromBase64(patch->toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors));
  writePorts(*this, components.interfaces<Process::PortFactoryList>(), p.m_inlets, p.m_outlets, &p);
}
