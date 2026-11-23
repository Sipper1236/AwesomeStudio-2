#pragma once

#include "Project.h"

class WavExporter
{
public:
    static bool exportProject(const ProjectData& project, const QString& path, QString* error = nullptr,
                              double startBeat = 0.0, double endBeat = -1.0);
    static bool exportStems(const ProjectData& project, const QString& directoryPath,
                            QString* error = nullptr, double startBeat = 0.0,
                            double endBeat = -1.0);
};
