// SPDX-License-Identifier: GPL-3.0-or-later
// What score shows of the add-on: the third-party components listed in the
// About dialog's Licenses tab, and the instruments in the process library.
#include <Process/Dataflow/Port.hpp>
#include <Process/Process.hpp>
#include <Process/ProcessList.hpp>
#include <QApplication>
#include <score_test/Document.hpp>
#include <core/document/Document.hpp>
#include <memory>
#include <core/presenter/Licenses.hpp>
#include <catch2/catch_test_macros.hpp>
#include <score_test/App.hpp>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTextStream>
#include <algorithm>
#include <vector>

namespace
{
struct Component
{
  QString name, instrument, aboutEntry, spdx, textPath, registeredAs;
};

QString addonFile(const QString& relative)
{
  return QStringLiteral(EMULATION_SOURCE_DIR "/") + relative;
}

QString cell(const QString& raw)
{
  QString s = raw.trimmed();
  s.remove(QLatin1Char('`'));
  s.remove(QStringLiteral("**"));
  return s.trimmed();
}

// The rows of the DEPENDENCIES.md license table, between its markers.
std::vector<Component> documentedComponents()
{
  QFile file{addonFile(QStringLiteral("DEPENDENCIES.md"))};
  REQUIRE(file.open(QIODevice::ReadOnly));
  QTextStream in{&file};
  in.setEncoding(QStringConverter::Utf8);
  std::vector<Component> res;
  bool inside = false;
  QStringList header;
  while(!in.atEnd())
  {
    const QString line = in.readLine();
    if(line.contains(QStringLiteral("<!-- about-licenses:begin -->"))) { inside = true; continue; }
    if(line.contains(QStringLiteral("<!-- about-licenses:end -->"))) break;
    if(!inside || !line.startsWith(QLatin1Char('|'))) continue;
    QStringList cells = line.mid(1, line.size() - (line.endsWith(QLatin1Char('|')) ? 2 : 1))
                            .split(QLatin1Char('|'));
    for(auto& c : cells) c = cell(c);
    if(header.isEmpty()) { header = cells; continue; }
    if(cells.value(0).startsWith(QStringLiteral("---"))) continue;
    auto at = [&](const char* column) {
      const int i = header.indexOf(QString::fromLatin1(column));
      REQUIRE(i >= 0);
      return cells.value(i);
    };
    res.push_back({at("Component"), at("Instrument"), at("About entry"), at("SPDX"),
                   at("License text"), at("Registered as")});
  }
  return res;
}

// The line naming the copyright holder in a source notice, without comment markers.
QByteArray copyrightLine(const QString& path)
{
  QFile file{addonFile(path)};
  REQUIRE(file.open(QIODevice::ReadOnly));
  static const QRegularExpression markers{QStringLiteral("^[\\s/*]+|[\\s*]+$")};
  while(!file.atEnd())
  {
    QString line = QString::fromUtf8(file.readLine());
    if(line.contains(QStringLiteral("Copyright"), Qt::CaseInsensitive))
      return line.remove(markers).toUtf8();
  }
  FAIL("no copyright line in " << path.toStdString());
  return {};
}

bool laBuilt()
{
#if EMULATION_HAS_LA
  return true;
#else
  return false;
#endif
}
}

TEST_CASE("The About dialog lists every third-party component with its license", "[emulation][licenses]")
{
  const auto components = documentedComponents();
  REQUIRE(components.size() >= 18);

  const auto licenses = score::thirdPartyLicenses();
  for(const auto& c : components)
  {
    CAPTURE(c.name.toStdString(), c.aboutEntry.toStdString());
    const auto it = std::find_if(licenses.begin(), licenses.end(),
        [&](const score::LicenseInfo& l) { return l.name == c.aboutEntry; });
    if(c.instrument == QStringLiteral("LAdrenaline") && !laBuilt())
    {
      // A component that is not built is not shipped, so it is not listed.
      CHECK(it == licenses.end());
      continue;
    }
    REQUIRE(it != licenses.end());
    CHECK(it->header.contains(c.spdx));
    CHECK(it->url.startsWith(QStringLiteral("https://github.com/jcelerier/")));
    REQUIRE_FALSE(it->license.isEmpty());

    if(c.registeredAs == QStringLiteral("file"))
    {
      QFile text{addonFile(c.textPath)};
      REQUIRE(text.open(QIODevice::ReadOnly));
      // The license list keeps a text's lines, not its carriage returns.
      CHECK(it->license.contains(text.readAll().replace('\r', QByteArray{})));
    }
    else
    {
      REQUIRE(c.registeredAs == QStringLiteral("notice"));
      CHECK(it->license.contains(copyrightLine(c.textPath)));
    }
  }
}

TEST_CASE("The process library offers the enabled emulation instruments only", "[emulation][registry]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    QSet<QString> names;
    for(const Process::ProcessModelFactory& factory : ctx.interfaces<Process::ProcessFactoryList>())
      names.insert(factory.prettyName());
    for(const char* name : {"Opale", "AYstral", "SIDérant", "Wurlie", "SiCC", "Consolated"})
    {
      CAPTURE(name);
      CHECK(names.contains(QString::fromUtf8(name)));
    }
    CHECK(names.contains(QStringLiteral("LAdrenaline")) == laBuilt());
  });
}

TEST_CASE("Each instrument says which kinds of port it has, as it makes them", "[emulation][registry]")
{
  // The process picker offers, from a port, the processes that say they can
  // be cabled to it
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto doc = score::test::new_document(ctx);
    REQUIRE(doc);
    int checked = 0;
    for(Process::ProcessModelFactory& factory : ctx.interfaces<Process::ProcessFactoryList>())
    {
      if(factory.category() != QStringLiteral("Synths/Emulation"))
        continue;
      CAPTURE(factory.prettyName().toStdString());
      const auto d = factory.descriptor(QString{});
      REQUIRE(d.inlets);
      REQUIRE(d.outlets);
      std::unique_ptr<Process::ProcessModel> proc{factory.make(
          TimeVal::fromMsecs(1000), QString{}, Id<Process::ProcessModel>{1},
          doc->context(), nullptr)};
      REQUIRE(proc);
      const auto kinds = [](const auto& ports) {
        QSet<int> s;
        for(const Process::Port* p : ports)
          s.insert(int(p->type()));
        return s;
      };
      const auto listed = [](const std::vector<Process::PortType>& v) {
        QSet<int> s;
        for(auto t : v)
          s.insert(int(t));
        return s;
      };
      CHECK(kinds(proc->inlets()) == listed(*d.inlets));
      CHECK(kinds(proc->outlets()) == listed(*d.outlets));
      checked++;
    }
    CHECK(checked >= 6);
  });
}
