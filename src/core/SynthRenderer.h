#pragma once

#include "Project.h"

#include <array>
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
    void resetTransport();
    void beginMixerFrame();
    void addLiveBusFrameSample(size_t busIndex, StereoSample input);
    StereoSample routeLiveTrackSample(size_t trackIndex, StereoSample input, int* busIndex = nullptr,
                                      StereoSample* sendSample = nullptr, int* sendBusIndex = nullptr) const;
    void setLiveTrackMixOverride(size_t trackIndex, int parameter, float value);
    void resetTrackPeakLevels();
    void resetBusPeakLevels();
    float trackPeakLevel(size_t trackIndex) const;
    float busPeakLevel(size_t busIndex) const;
    StereoSample renderSample(qint64 projectFrame);

private:
    struct VoiceNote
    {
        qint64 startFrame = 0;
        qint64 endFrame = 0;
        qint64 attackFrames = 1;
        qint64 decayFrames = 0;
        qint64 releaseFrames = 1;
        size_t trackIndex = 0;
        double frequency = 440.0;
        float amplitude = 0.0f;
        float sustainLevel = 1.0f;
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
        size_t trackIndex = 0;
        float sourceGain = 1.0f;
        std::shared_ptr<const AudioAsset> source;
    };

    struct TrackMixAutomation
    {
        float baseVolume = 1.0f;
        float basePan = 0.0f;
        QVector<AutomationPoint> volume;
        QVector<AutomationPoint> pan;
    };

    struct TrackMixValue
    {
        qint64 frame = -1;
        float leftGain = 0.70710678f;
        float rightGain = 0.70710678f;
    };

    struct LiveTrackMixOverride
    {
        float volume = -1.0f;
        float pan = 2.0f;
    };

    struct TrackEqualizerState
    {
        float lowLeft = 0.0f;
        float lowRight = 0.0f;
        float upperLeft = 0.0f;
        float upperRight = 0.0f;
        bool tailActive = false;
    };

    struct BusMixState
    {
        float leftGain = 1.0f;
        float rightGain = 1.0f;
    };

    const TrackMixValue& trackMixAt(size_t trackIndex, qint64 frame, double beat);
    void addTrackSample(size_t trackIndex, float left, float right);
    void addBusFrameSample(size_t busIndex, StereoSample input);
    StereoSample applyTrackEqualizer(size_t trackIndex, StereoSample input);
    void resetTrackEqualizers();

    std::shared_ptr<const ProjectData> project;
    std::vector<TrackMixAutomation> trackMixAutomations;
    std::vector<TrackMixValue> trackMixValues;
    std::vector<LiveTrackMixOverride> liveTrackMixOverrides;
    std::vector<std::array<float, 3>> trackEqualizerGains;
    std::vector<TrackEqualizerState> trackEqualizerStates;
    std::vector<size_t> activeEqualizerTailTracks;
    std::vector<int> trackBusIndices;
    std::vector<int> trackSendBusIndices;
    std::vector<float> trackSendGains;
    std::vector<unsigned char> trackAudible;
    std::vector<BusMixState> busMixStates;
    std::vector<unsigned char> busAudible;
    std::vector<StereoSample> busFrameSamples;
    std::vector<StereoSample> lastBusOutputSamples;
    std::vector<unsigned char> busFrameTouched;
    std::vector<size_t> touchedBusIndices;
    std::vector<float> busPeakLevels;
    std::vector<StereoSample> trackFrameSamples;
    std::vector<unsigned char> trackFrameTouched;
    std::vector<size_t> touchedTrackIndices;
    std::vector<float> trackPeakLevels;
    std::vector<VoiceNote> notes;
    std::vector<size_t> activeNotes;
    std::vector<AudioRegion> audioRegions;
    std::vector<size_t> activeAudioRegions;
    size_t nextStartIndex = 0;
    size_t nextAudioStartIndex = 0;
    qint64 lastFrame = -1;
    double sampleRate = 48000.0;
    float lowBandCoefficient = 0.0f;
    float highBandCoefficient = 0.0f;
    bool hasBusSolo = false;
};
