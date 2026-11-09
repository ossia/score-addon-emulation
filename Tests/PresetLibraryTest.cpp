// SPDX-License-Identifier: GPL-3.0-or-later
// Every preset score-user-library holds for an instrument loads into the
// current process without an error, puts each stored control on an existing
// control port, and survives a save and reload unchanged.
#include <Emulation/Apu/ProcessModel.hpp>
#include <Emulation/Opl/ProcessModel.hpp>
#include <Emulation/Psg/ProcessModel.hpp>
#include <Emulation/Sid/ProcessModel.hpp>
#include <Emulation/Wavetable/ProcessModel.hpp>
#include <Emulation/Wurli/ProcessModel.hpp>
#include <Process/Dataflow/Port.hpp>
#include <Process/PresetHelpers.hpp>
#include <core/document/Document.hpp>
#include <score/serialization/JSONVisitor.hpp>
#include <catch2/catch_test_macros.hpp>
#include <score_test/App.hpp>
#include <score_test/Project.hpp>
#include "PresetLibrary.hpp"
#include <QFileInfo>
#include <QSet>
#include <cmath>

namespace
{
template <typename Model>
void checkFolder(const QString& process, const char* uuid, int count)
{
  const auto folder = Emulation::Test::presetFolder(process);
  score::test::run_in_app([&](const score::GUIApplicationContext& ctx) {
    auto& factories = ctx.interfaces<Process::ProcessFactoryList>();
    auto* doc = score::test::new_document(ctx); REQUIRE(doc);
    const auto files = folder.entryList({QStringLiteral("*.scp")}, QDir::Files, QDir::Name);
    CHECK(files.size() == count);
    for(const auto& file : files)
    {
      const auto name = QFileInfo{file}.completeBaseName();
      CAPTURE(name.toStdString());
      auto preset = Emulation::Test::loadPreset(factories, folder, name);
      REQUIRE(preset);
      CHECK(preset->name == name);
      REQUIRE(preset->key.key == UuidKey<Process::ProcessModel>::fromString(QString::fromLatin1(uuid)));

      auto* loaded = dynamic_cast<Model*>(score::test::add_process(*doc, QString::fromLatin1(uuid), {}));
      REQUIRE(loaded);
      REQUIRE(loaded->presetMatches(*preset));
      loaded->loadPreset(*preset);
      CHECK(loaded->error().toStdString() == "");

      // Stored controls, either the whole data or its "Controls" member.
      const auto json = readJson(preset->data);
      const rapidjson::Value* controls = json.IsArray() ? &json : nullptr;
      if(json.IsObject())
        if(auto it = json.FindMember("Controls"); it != json.MemberEnd() && it->value.IsArray())
          controls = &it->value;
      if(controls)
      {
        QSet<int> seen;
        for(const auto& entry : controls->GetArray())
        {
          REQUIRE(entry.IsArray()); REQUIRE(entry.Size() == 2); REQUIRE(entry[0].IsInt());
          const int id = entry[0].GetInt();
          CAPTURE(id);
          CHECK_FALSE(seen.contains(id)); seen.insert(id);
          Process::ControlInlet* port{};
          for(auto* inlet : loaded->inlets())
            if(inlet->id().val() == id) port = qobject_cast<Process::ControlInlet*>(inlet);
          REQUIRE(port);
          const auto stored = JsonValue{entry[1]}.to<ossia::value>();
          CAPTURE(port->name().toStdString());
          // Loading may retype a stored scalar, as a Boolean that is now a
          // choice, but never change what it means.
          const auto scalar = [](const ossia::value& v) {
            const auto t = v.get_type();
            return t == ossia::val_type::FLOAT || t == ossia::val_type::INT || t == ossia::val_type::BOOL;
          };
          if(scalar(stored))
          {
            REQUIRE(scalar(port->value()));
            CHECK(std::abs(ossia::convert<double>(port->value()) - ossia::convert<double>(stored)) <= 1e-6);
          }
          else
          {
            REQUIRE(port->value().get_type() == stored.get_type());
            if(stored.get_type() == ossia::val_type::STRING)
              CHECK(port->value() == stored);
          }
        }
      }

      auto* restored = dynamic_cast<Model*>(score::test::add_process(*doc, QString::fromLatin1(uuid), {}));
      REQUIRE(restored);
      const auto saved = loaded->savePreset();
      restored->loadPreset(saved);
      CHECK(restored->error().toStdString() == "");
      CHECK(restored->savePreset().data == saved.data);
    }
  });
}
}

TEST_CASE("Opale library presets load on their ports", "[emulation][opl][preset][library]")
{ checkFolder<Emulation::Opl::ProcessModel>(QStringLiteral("Opale"), "2fe73b91-4100-4cbb-b8d8-10e82c9f317a", 22); }
TEST_CASE("AYstral library presets load on their ports", "[emulation][psg][preset][library]")
{ checkFolder<Emulation::Psg::ProcessModel>(QStringLiteral("AYstral"), "5aabbeda-10c7-40ca-985a-90d7863df990", 14); }
TEST_CASE("SIDérant library presets load on their ports", "[emulation][sid][preset][library]")
{ checkFolder<Emulation::Sid::ProcessModel>(QStringLiteral("SIDérant"), "ce87e734-2cf5-4044-bff4-c849e98bce4a", 13); }
TEST_CASE("Wurlie library presets load on their ports", "[emulation][wurli][preset][library]")
{ checkFolder<Emulation::Wurli::ProcessModel>(QStringLiteral("Wurlie"), "270d9ee2-2a2d-48fd-8bac-9e8ba73a5f4d", 8); }
TEST_CASE("SiCC library presets load on their ports", "[emulation][wavetable][preset][library]")
{ checkFolder<Emulation::Wavetable::ProcessModel>(QStringLiteral("SiCC"), "989bc228-c2e7-40a8-a56e-4700b946d864", 13); }
TEST_CASE("Consolated library presets load on their ports", "[emulation][apu][preset][library]")
{ checkFolder<Emulation::Apu::ProcessModel>(QStringLiteral("Consolated"), "28b1e0ab-e245-4a25-8fb6-d05e43bf6f08", 10); }
