#include "ProcessModel.hpp"
#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <score/application/ApplicationComponents.hpp>
#include <score/serialization/DataStreamVisitor.hpp>
#include <score/serialization/JSONValueVisitor.hpp>
#include <score/serialization/JSONVisitor.hpp>
#include <stdexcept>

// Semantic controls, never native pointers, oscillators, MIDI lifetimes or ROM data.
template<> void DataStreamReader::read(const Emulation::Psg::ProcessModel& proc)
{
  m_stream << quint32{2} << QStringLiteral("emu2149+nuked-psg")
      << QString::fromLatin1(Emulation::Psg::modelTag(proc.settings().model));
  readPorts(*this, proc.m_inlets, proc.m_outlets);
  insertDelimiter();
}
template<> void DataStreamWriter::write(Emulation::Psg::ProcessModel& proc)
{
  quint32 schema{}; QString backend, model;
  m_stream >> schema >> backend >> model;
  if((schema != 1 && schema != 2) || backend != QStringLiteral("emu2149+nuked-psg"))
    throw std::runtime_error("Unsupported PSG state schema or backend.");
  writePorts(*this, components.interfaces<Process::PortFactoryList>(), proc.m_inlets, proc.m_outlets, &proc);
  proc.validatePorts();
  if(model != QString::fromLatin1(Emulation::Psg::modelTag(proc.settings().model)))
    throw std::runtime_error("PSG model tag does not match semantic controls.");
  checkDelimiter();
}
template<> void JSONReader::read(const Emulation::Psg::ProcessModel& proc)
{
  obj["Schema"] = 2;
  obj["Backend"] = QStringLiteral("emu2149+nuked-psg");
  obj["Model"] = QString::fromLatin1(Emulation::Psg::modelTag(proc.settings().model));
  readPorts(*this, proc.m_inlets, proc.m_outlets);
}
template<> void JSONWriter::write(Emulation::Psg::ProcessModel& proc)
{
  const auto schema = obj.tryGet("Schema"), backend = obj.tryGet("Backend"), model = obj.tryGet("Model");
  if(!schema || (schema->toInt() != 1 && schema->toInt() != 2) || !backend || backend->toString() != QStringLiteral("emu2149+nuked-psg") || !model)
    throw std::runtime_error("Unsupported PSG state schema or backend.");
  writePorts(*this, components.interfaces<Process::PortFactoryList>(), proc.m_inlets, proc.m_outlets, &proc);
  proc.validatePorts();
  if(model->toString() != QString::fromLatin1(Emulation::Psg::modelTag(proc.settings().model)))
    throw std::runtime_error("PSG model tag does not match semantic controls.");
}
