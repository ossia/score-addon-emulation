// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProcessModel.hpp"
#include "Controls.hpp"
#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <score/application/ApplicationComponents.hpp>
#include <score/serialization/DataStreamVisitor.hpp>
#include <score/serialization/JSONValueVisitor.hpp>
#include <score/serialization/JSONVisitor.hpp>
#include <stdexcept>
// Only typed semantic control ports persist: never model state, voices or learned assets.
template<> void DataStreamReader::read(const Emulation::Wurli::ProcessModel& p)
{
  m_stream << quint32{2} << QStringLiteral("openwurli") << QStringLiteral("200A");
  readPorts(*this, p.m_inlets, p.m_outlets); insertDelimiter();
}
template<> void DataStreamWriter::write(Emulation::Wurli::ProcessModel& p)
{
  quint32 schema{}; QString backend, model;
  m_stream >> schema >> backend >> model;
  if((schema != 1 && schema != 2) || backend != QStringLiteral("openwurli") || model != QStringLiteral("200A"))
    throw std::runtime_error("Unsupported Wurlitzer semantic state schema.");
  writePorts(*this, components.interfaces<Process::PortFactoryList>(), p.m_inlets, p.m_outlets, &p);
  if(schema == 1 && p.m_inlets.size() == Emulation::Wurli::ControlCount)
    p.m_inlets.push_back(Emulation::Wurli::makeAllocationControl(&p));
  p.restoreControls(); checkDelimiter();
}
template<> void JSONReader::read(const Emulation::Wurli::ProcessModel& p)
{
  obj["Schema"] = 2; obj["Backend"] = QStringLiteral("openwurli"); obj["Model"] = QStringLiteral("200A");
  readPorts(*this, p.m_inlets, p.m_outlets);
}
template<> void JSONWriter::write(Emulation::Wurli::ProcessModel& p)
{
  const auto schema = obj.tryGet("Schema"), backend = obj.tryGet("Backend"), model = obj.tryGet("Model");
  if(!schema || (schema->toInt() != 1 && schema->toInt() != 2) || !backend || backend->toString() != QStringLiteral("openwurli")
      || !model || model->toString() != QStringLiteral("200A"))
    throw std::runtime_error("Unsupported Wurlitzer semantic state schema.");
  writePorts(*this, components.interfaces<Process::PortFactoryList>(), p.m_inlets, p.m_outlets, &p);
  if(schema->toInt() == 1 && p.m_inlets.size() == Emulation::Wurli::ControlCount)
    p.m_inlets.push_back(Emulation::Wurli::makeAllocationControl(&p));
  p.restoreControls();
}
