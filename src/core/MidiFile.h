#pragma once

#include "Project.h"

enum class MidiExportFormat
{
    Format0,
    Format1
};

class MidiFile
{
public:
    static bool load(const QString& path, ProjectData& project, QString* error = nullptr);
    static bool save(const QString& path, const ProjectData& project, QString* error = nullptr,
                     MidiExportFormat format = MidiExportFormat::Format1);
};
