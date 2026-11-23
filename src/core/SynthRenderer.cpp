#include "SynthRenderer.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double twoPi = 6.28318530717958647692;

float oscillatorSample(SynthWaveform waveform, double phase)
{
    phase -= std::floor(phase);
    switch (waveform)
    {
        case SynthWaveform::Triangle:
            return static_cast<float>(1.0 - 4.0 * std::abs(phase - 0.5));
        case SynthWaveform::Saw:
            return static_cast<float>(2.0 * phase - 1.0);
        case SynthWaveform::Square:
            return phase < 0.5 ? 1.0f : -1.0f;
        case SynthWaveform::Sine:
        default:
            return static_cast<float>(std::sin(phase * twoPi));
    }
}
}

void SynthRenderer::setProject(std::shared_ptr<const ProjectData> newProject, double newSampleRate)
{
    project = std::move(newProject);
    sampleRate = std::max(1.0, newSampleRate);
    notes.clear();
    activeNotes.clear();
    nextStartIndex = 0;
    lastFrame = -1;
    if (!project)
        return;

    bool hasSolo = false;
    for (const InstrumentTrack& track : project->tracks)
        hasSolo = hasSolo || track.solo;

    for (const InstrumentTrack& track : project->tracks)
    {
        if (track.muted || (hasSolo && !track.solo))
            continue;

        const double panAngle = (std::clamp(static_cast<double>(track.pan), -1.0, 1.0) + 1.0) *
                                0.7853981633974483;
        const float leftGain = static_cast<float>(std::cos(panAngle));
        const float rightGain = static_cast<float>(std::sin(panAngle));
        for (const MidiClip& clip : track.clips)
        {
            for (const MidiNote& note : clip.notes)
            {
                VoiceNote voice;
                const double startBeat = clip.startBeat + note.startBeat;
                voice.startFrame = static_cast<qint64>(std::llround(startBeat * sampleRate * 60.0 /
                                                                    project->tempoBpm));
                const qint64 duration = static_cast<qint64>(std::llround(note.lengthBeats * sampleRate * 60.0 /
                                                                          project->tempoBpm));
                voice.endFrame = voice.startFrame + std::max<qint64>(duration, 1);
                voice.frequency = 440.0 * std::pow(2.0, (note.pitch - 69) / 12.0);
                voice.amplitude = 0.22f * static_cast<float>(note.velocity) / 127.0f * track.gain;
                voice.leftGain = leftGain;
                voice.rightGain = rightGain;
                voice.waveform = track.waveform;
                notes.push_back(voice);
            }
        }
    }
    std::sort(notes.begin(), notes.end(), [](const VoiceNote& left, const VoiceNote& right)
    {
        return left.startFrame < right.startFrame;
    });
}

StereoSample SynthRenderer::renderSample(qint64 projectFrame)
{
    if (!project)
        return {};

    // A backwards frame means the transport restarted or wrapped at the loop boundary.
    if (projectFrame < lastFrame)
    {
        activeNotes.clear();
        nextStartIndex = 0;
    }
    lastFrame = projectFrame;

    StereoSample sample;
    const qint64 releaseFrames = static_cast<qint64>(sampleRate * 0.06);
    const qint64 attackFrames = std::max<qint64>(1, static_cast<qint64>(sampleRate * 0.008));
    while (nextStartIndex < notes.size() && notes[nextStartIndex].startFrame <= projectFrame)
        activeNotes.push_back(nextStartIndex++);

    activeNotes.erase(std::remove_if(activeNotes.begin(), activeNotes.end(), [&](size_t index)
    {
        return projectFrame >= notes[index].endFrame + releaseFrames;
    }), activeNotes.end());

    for (const size_t index : activeNotes)
    {
        const VoiceNote& note = notes[index];
        if (projectFrame < note.startFrame || projectFrame >= note.endFrame + releaseFrames)
            continue;

        const qint64 age = projectFrame - note.startFrame;
        float envelope = age < attackFrames
            ? static_cast<float>(age) / static_cast<float>(attackFrames)
            : 1.0f;
        if (projectFrame >= note.endFrame)
        {
            const qint64 releaseAge = projectFrame - note.endFrame;
            envelope *= std::max(0.0f, 1.0f - static_cast<float>(releaseAge) /
                                               static_cast<float>(releaseFrames));
        }

        const double phase = static_cast<double>(age) * note.frequency / sampleRate;
        const float voice = oscillatorSample(note.waveform, phase) * note.amplitude * envelope;
        sample.left += voice * note.leftGain;
        sample.right += voice * note.rightGain;
    }
    return sample;
}
