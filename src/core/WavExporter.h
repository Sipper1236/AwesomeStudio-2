#pragma once

#include "Project.h"

enum class WavSampleFormat
{
    Pcm16,
    Pcm24,
    Float32
};

enum class AudioExportContainer
{
    Wav,
    Flac,
    OggVorbis
};

struct WavExportOptions
{
    int sampleRate = 44100;
    WavSampleFormat sampleFormat = WavSampleFormat::Pcm16;
    bool dither = false;
    float vorbisQuality = 0.8f;
};

class WavExporter
{
public:
    static bool exportProject(const ProjectData& project, const QString& path, QString* error = nullptr,
                              double startBeat = 0.0, double endBeat = -1.0,
                              const WavExportOptions& options = {},
                              AudioExportContainer container = AudioExportContainer::Wav);
    static bool exportStems(const ProjectData& project, const QString& directoryPath,
                            QString* error = nullptr, double startBeat = 0.0,
                            double endBeat = -1.0, const WavExportOptions& options = {});
};
