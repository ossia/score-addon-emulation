// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProcessModel.hpp"
#include "Controls.hpp"
#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <score/application/ApplicationComponents.hpp>
#include <score/serialization/DataStreamVisitor.hpp>
#include <score/serialization/JSONValueVisitor.hpp>
#include <score/serialization/JSONVisitor.hpp>
#include <score/serialization/VisitorCommon.hpp>
#include <stdexcept>

namespace
{
// ComboBox normally persists its alternatives. Serialize detached copies of
// program selectors with only numeric/original-memory labels, so a document
// does not retain a stale ROM-name catalog when those ROMs are unavailable.
struct SerializablePorts
{
  Process::Inlets inlets;
  std::array<std::unique_ptr<Process::ComboBox>, 8> programs;
  explicit SerializablePorts(const Emulation::La::ProcessModel& model) : inlets{model.inlets()}
  {
    for(int part = 0; part < 8; ++part)
    {
      const auto index = Emulation::La::partControl(part, Emulation::La::Program) + 1;
      if(auto* source = qobject_cast<Process::ComboBox*>(inlets[index]))
      {
        const auto data = toValue(score::marshall<JSONObject>(*source));
        JSONObject::Deserializer reader{data};
        auto copy = std::make_unique<Process::ComboBox>(reader, nullptr);
        copy->setAlternatives(Emulation::La::programChoices(model.patch(), part, nullptr));
        inlets[index] = copy.get();
        programs[part] = std::move(copy);
      }
    }
  }
};
}

template<> void DataStreamReader::read(const Emulation::La::ProcessModel& p)
{
  m_stream << quint32{1} << QStringLiteral("munt") << p.patchData();
  const SerializablePorts ports{p};
  readPorts(*this, ports.inlets, p.m_outlets);
  insertDelimiter();
}
template<> void DataStreamWriter::write(Emulation::La::ProcessModel& p)
{
  quint32 schema{};
  QString backend;
  QByteArray data;
  m_stream >> schema >> backend >> data;
  if(schema != 1 || backend != QStringLiteral("munt"))
    throw std::runtime_error("Unsupported LA process schema or backend.");
  p.restorePatch(data);
  writePorts(*this, components.interfaces<Process::PortFactoryList>(), p.m_inlets, p.m_outlets, &p);
  checkDelimiter();
  p.migrateControls();
}
template<> void JSONReader::read(const Emulation::La::ProcessModel& p)
{
  obj["Schema"] = 1;
  obj["Backend"] = QStringLiteral("munt");
  obj["Patch"] = QString::fromLatin1(p.patchData().toBase64());
  const SerializablePorts ports{p};
  readPorts(*this, ports.inlets, p.m_outlets);
}
template<> void JSONWriter::write(Emulation::La::ProcessModel& p)
{
  const auto schema = obj.tryGet("Schema");
  const auto backend = obj.tryGet("Backend");
  const auto patch = obj.tryGet("Patch");
  if(!schema || schema->toInt() != 1 || !backend || backend->toString() != QStringLiteral("munt") || !patch)
    throw std::runtime_error("Unsupported LA process schema or backend.");
  p.restorePatch(QByteArray::fromBase64(patch->toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors));
  writePorts(*this, components.interfaces<Process::PortFactoryList>(), p.m_inlets, p.m_outlets, &p);
  p.migrateControls();
}
