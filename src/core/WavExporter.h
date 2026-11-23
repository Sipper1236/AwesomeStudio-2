#pragma once

#include "Project.h"

class WavExporter
{
public:
    static bool exportProject(const ProjectData& project, const QString& path, QString* error = nullptr);
    static bool exportStems(const ProjectData& project, const QString& directoryPath,
                            QString* error = nullptr);
};
