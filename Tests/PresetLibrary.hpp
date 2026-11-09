// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The instruments' presets are part of score-user-library, one folder per
// process name under Presets/. Tests find a checkout of it, in order, through
// the EMULATION_PRESET_LIBRARY environment variable, the CMake cache variable
// of the same name, then the default library of a score install.
#include <Process/Preset.hpp>
#include <Process/ProcessList.hpp>
#include <catch2/catch_test_macros.hpp>
#include <QDir>
#include <QStandardPaths>
#include <QString>
#include <memory>
#include <string>

namespace Emulation::Test
{
inline QString presetLibrary()
{
  if(const auto path = qEnvironmentVariable("EMULATION_PRESET_LIBRARY"); !path.isEmpty())
    return path;
#if defined(EMULATION_PRESET_LIBRARY_DIR)
  if(const auto path = QString::fromUtf8(EMULATION_PRESET_LIBRARY_DIR); !path.isEmpty())
    return path;
#endif
  // Spelled out: tests run under another application name than score.
  return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
         + QStringLiteral("/ossia/score/packages/default");
}

// Skips the calling test case when the library has no folder for the process.
// Call it before run_in_app: a skip cannot cross the application's event loop.
inline QDir presetFolder(const QString& process)
{
  QDir folder{presetLibrary() + QStringLiteral("/Presets/") + process};
  if(!folder.exists())
  {
    const QString reason = "No " + process + " presets in " + presetLibrary()
                           + ": point EMULATION_PRESET_LIBRARY at a score-user-library checkout.";
    SKIP(reason.toStdString());
  }
  return folder;
}

// Read as the library reads it; null when missing or invalid.
inline std::shared_ptr<Process::Preset> loadPreset(
    const Process::ProcessFactoryList& factories, const QDir& folder, const QString& name)
{
  return Process::Preset::fromFile(factories, folder.filePath(name + QStringLiteral(".scp")));
}
}
