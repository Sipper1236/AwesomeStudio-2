#pragma once

#include "Project.h"

#include <memory>
#include <vector>

struct StereoSample
{
    float left = 0.0f;
    float right = 0.0f;
};

float renderPercussionSample(int midiPitch, qint64 ageFrames, qint64 startFrame, double sampleRate);

class SynthRenderer
{
public:
    void setProject(std::shared_ptr<const ProjectData> project, double sampleRate);
    StereoSample renderSample(qint64 projectFrame);

private:
    struct VoiceNote
    {
        qint64 startFrame = 0;
        qint64 endFrame = 0;
        double frequency = 440.0;
        float amplitude = 0.0f;
        float leftGain = 0.70710678f;
        float rightGain = 0.70710678f;
        SynthWaveform waveform = SynthWaveform::Sine;
        int midiPitch = 60;
        bool percussion = false;
    };

    struct AudioRegion
    {
        qint64 startFrame = 0;
        qint64 endFrame = 0;
        qint64 sourceStartFrame = 0;
        qint64 fadeInFrames = 0;
        qint64 fadeOutFrames = 0;
        std::shared_ptr<const AudioAsset> source;
        float leftGain = 0.70710678f;
        float rightGain = 0.70710678f;
    };

    std::shared_ptr<const ProjectData> project;
    std::vector<VoiceNote> notes;
    std::vector<size_t> activeNotes;
    std::vector<AudioRegion> audioRegions;
    std::vector<size_t> activeAudioRegions;
    size_t nextStartIndex = 0;
    size_t nextAudioStartIndex = 0;
    qint64 lastFrame = -1;
    double sampleRate = 48000.0;
};
