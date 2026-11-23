#pragma once

#include "Project.h"

class ProjectFile
{
public:
    static bool save(const QString& path, const ProjectData& project, QString* error = nullptr);
    static bool load(const QString& path, ProjectData& project, QString* error = nullptr);
};
