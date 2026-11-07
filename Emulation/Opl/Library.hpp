#pragma once
#include <Emulation/Opl/ProcessModel.hpp>
#include <Process/Drop/ProcessDropHandler.hpp>
#include <Library/LibraryInterface.hpp>
#include <Library/LibrarySettings.hpp>
#include <Library/ProcessesItemModel.hpp>
#include <QFile>
#include <QFileInfo>

namespace Emulation::Opl
{
class LibraryHandler final : public QObject, public Library::LibraryInterface
{
  SCORE_CONCRETE("9cae1672-1af6-4bcb-b1ae-fd2fd14f4e8c")
  Library::CategoryPaths categories;
  void setup(Library::ProcessesItemModel&, const score::GUIApplicationContext& ctx) override
  {
    categories.init(Metadata<PrettyName_k, ProcessModel>::get().toStdString(), ctx);
  }
  QSet<QString> acceptedFiles() const noexcept override { return {"wopl"}; }
  std::optional<Library::ProcessEntry> scanPath(std::string_view path) override
  {
    const QString filename = QString::fromUtf8(path.data(), path.size());
    const QFileInfo info{filename};
    if(info.suffix().compare(QStringLiteral("wopl"), Qt::CaseInsensitive) != 0) return std::nullopt;
    QFile file{filename};
    if(!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 16 * 1024 * 1024)
      return std::nullopt;
    const auto bytes = file.read(16 * 1024 * 1024 + 1);
    if(bytes.size() != file.size() || bytes.size() > 16 * 1024 * 1024) return std::nullopt;
    try
    {
      const auto bank = decodeBank({reinterpret_cast<const uint8_t*>(bytes.constData()),
          static_cast<std::size_t>(bytes.size())});
      if(bank.programs.empty()) return std::nullopt;
    }
    catch(const std::exception&) { return std::nullopt; }
    Library::ProcessEntry entry;
    entry.rootKey = Metadata<ConcreteKey_k, ProcessModel>::get();
    entry.categoryPath = {QStringLiteral("WOPL")};
    entry.node.data.key = entry.rootKey;
    entry.node.data.prettyName = info.completeBaseName();
    entry.node.data.customData = info.absoluteFilePath();
    return entry;
  }
};
class DropHandler final : public Process::ProcessDropHandler
{
  SCORE_CONCRETE("d21a798e-6ed8-408c-a08d-25be1b65e345")
  QSet<QString> fileExtensions() const noexcept override { return {"wopl"}; }
  void dropPath(std::vector<ProcessDrop>& vec, const score::FilePath& filename,
      const score::DocumentContext&) const noexcept override
  {
    const QFileInfo info{filename.absolute};
    if(info.suffix().compare(QStringLiteral("wopl"), Qt::CaseInsensitive) != 0) return;
    ProcessDrop p;
    p.creation.key = Metadata<ConcreteKey_k, ProcessModel>::get();
    p.creation.prettyName = info.completeBaseName();
    p.creation.customData = filename.absolute;
    vec.push_back(std::move(p));
  }
};
}
