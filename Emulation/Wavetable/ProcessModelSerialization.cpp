#include "ProcessModel.hpp"
#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <score/application/ApplicationComponents.hpp>
#include <score/serialization/DataStreamVisitor.hpp>
#include <score/serialization/JSONValueVisitor.hpp>
#include <score/serialization/JSONVisitor.hpp>
#include <stdexcept>

// Semantic controls, never native pointers, oscillators, MIDI lifetimes or ROM data.
template<> void DataStreamReader::read(const Emulation::Wavetable::ProcessModel& proc)
{
  m_stream << quint32{4} << QStringLiteral("emu2212+libvgm-huc6280")
      << QString::fromLatin1(Emulation::Wavetable::modelTag(proc.settings().model));
  readPorts(*this, proc.m_inlets, proc.m_outlets);
  insertDelimiter();
}
template<> void DataStreamWriter::write(Emulation::Wavetable::ProcessModel& proc)
{
  quint32 schema{}; QString backend, model;
  m_stream >> schema >> backend >> model;
  if(schema != 4 || backend != QStringLiteral("emu2212+libvgm-huc6280"))
    throw std::runtime_error("Unsupported Chip Wavetable state schema or backend.");
  writePorts(*this, components.interfaces<Process::PortFactoryList>(), proc.m_inlets, proc.m_outlets, &proc);
  proc.validatePorts();
  if(model != QString::fromLatin1(Emulation::Wavetable::modelTag(proc.settings().model)))
    throw std::runtime_error("Chip Wavetable model tag does not match semantic controls.");
  checkDelimiter();
}
template<> void JSONReader::read(const Emulation::Wavetable::ProcessModel& proc)
{
  obj["Schema"] = 4;
  obj["Backend"] = QStringLiteral("emu2212+libvgm-huc6280");
  obj["Model"] = QString::fromLatin1(Emulation::Wavetable::modelTag(proc.settings().model));
  readPorts(*this, proc.m_inlets, proc.m_outlets);
}
template<> void JSONWriter::write(Emulation::Wavetable::ProcessModel& proc)
{
  const auto schema = obj.tryGet("Schema"), backend = obj.tryGet("Backend"), model = obj.tryGet("Model");
  if(!schema || schema->toInt() != 4 || !backend || backend->toString() != QStringLiteral("emu2212+libvgm-huc6280") || !model)
    throw std::runtime_error("Unsupported Chip Wavetable state schema or backend.");
  writePorts(*this, components.interfaces<Process::PortFactoryList>(), proc.m_inlets, proc.m_outlets, &proc);
  proc.validatePorts();
  if(model->toString() != QString::fromLatin1(Emulation::Wavetable::modelTag(proc.settings().model)))
    throw std::runtime_error("Chip Wavetable model tag does not match semantic controls.");
}
