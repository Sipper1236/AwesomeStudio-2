#pragma once

#include "Project.h"

#include <functional>

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
    OggVorbis,
    Mp3
};

struct WavExportOptions
{
    int sampleRate = 44100;
    WavSampleFormat sampleFormat = WavSampleFormat::Pcm16;
    bool dither = false;
    float vorbisQuality = 0.8f;
    int mp3BitrateKbps = 192;
};

class WavExporter
{
public:
    using ProgressCallback = std::function<bool(qint64 completedFrames, qint64 totalFrames)>;

    static bool exportProject(const ProjectData& project, const QString& path, QString* error = nullptr,
                              double startBeat = 0.0, double endBeat = -1.0,
                              const WavExportOptions& options = {},
                              AudioExportContainer container = AudioExportContainer::Wav,
                              const ProgressCallback& progress = {});
    static bool exportStems(const ProjectData& project, const QString& directoryPath,
                            QString* error = nullptr, double startBeat = 0.0,
                            double endBeat = -1.0, const WavExportOptions& options = {});
};
