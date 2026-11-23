#pragma once

#include "Project.h"

enum class WavSampleFormat
{
    Pcm16,
    Pcm24,
    Float32
};

struct WavExportOptions
{
    WavSampleFormat sampleFormat = WavSampleFormat::Pcm16;
    bool dither = false;
};

class WavExporter
{
public:
    static bool exportProject(const ProjectData& project, const QString& path, QString* error = nullptr,
                              double startBeat = 0.0, double endBeat = -1.0,
                              const WavExportOptions& options = {});
    static bool exportStems(const ProjectData& project, const QString& directoryPath,
                            QString* error = nullptr, double startBeat = 0.0,
                            double endBeat = -1.0, const WavExportOptions& options = {});
};
