// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProcessModel.hpp"
#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <score/application/ApplicationComponents.hpp>
#include <score/serialization/DataStreamVisitor.hpp>
#include <score/serialization/JSONVisitor.hpp>
#include <score/serialization/VisitorCommon.hpp>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>
template<> void DataStreamReader::read(const Emulation::Apu::ProcessModel& p) {
  m_stream << quint32{1} << p.stateData();readPorts(*this,p.m_inlets,p.m_outlets);insertDelimiter();
}
template<> void DataStreamWriter::write(Emulation::Apu::ProcessModel& p) {
  quint32 version{};QByteArray data;m_stream>>version>>data;
  if(version!=1)throw std::runtime_error("Unsupported APU binary schema.");
  writePorts(*this,components.interfaces<Process::PortFactoryList>(),p.m_inlets,p.m_outlets,&p);checkDelimiter();
  p.restoreState(data);
}
template<> void JSONReader::read(const Emulation::Apu::ProcessModel& p) {
  const auto data=p.stateData();stream.Key("State");stream.RawValue(data.constData(),data.size(),rapidjson::kObjectType);
  readPorts(*this,p.m_inlets,p.m_outlets);
}
template<> void JSONWriter::write(Emulation::Apu::ProcessModel& p) {
  const auto state=obj.tryGet("State");if(!state||!state->obj.IsObject())throw std::runtime_error("Missing semantic APU state.");
  rapidjson::StringBuffer buffer;rapidjson::Writer<rapidjson::StringBuffer> writer{buffer};state->obj.Accept(writer);
  writePorts(*this,components.interfaces<Process::PortFactoryList>(),p.m_inlets,p.m_outlets,&p);
  p.restoreState(QByteArray{buffer.GetString(),qsizetype(buffer.GetSize())});
}
