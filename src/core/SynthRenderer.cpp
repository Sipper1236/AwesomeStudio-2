#include "SynthRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

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

float noiseSample(qint64 ageFrames, qint64 startFrame, int midiPitch)
{
    std::uint32_t value = static_cast<std::uint32_t>(ageFrames) ^
                          static_cast<std::uint32_t>(startFrame >> 4) ^
                          static_cast<std::uint32_t>(midiPitch * 0x9e3779b9u);
    value += 0x9e3779b9u;
    value = (value ^ (value >> 16)) * 0x21f0aaadu;
    value = (value ^ (value >> 15)) * 0x735a2d97u;
    value ^= value >> 15;
    return static_cast<float>(value & 0x00ffffffu) / 8388607.5f - 1.0f;
}
}

float renderPercussionSample(int midiPitch, qint64 ageFrames, qint64 startFrame, double sampleRate)
{
    if (ageFrames < 0 || sampleRate <= 0.0)
        return 0.0f;
    const double time = static_cast<double>(ageFrames) / sampleRate;
    constexpr double twoPi = 6.28318530717958647692;
    if (midiPitch == 35 || midiPitch == 36)
    {
        const double phase = 42.0 * time + (118.0 / 17.0) * (1.0 - std::exp(-17.0 * time));
        return static_cast<float>(std::sin(phase * twoPi) * std::exp(-time * 9.0));
    }
    if (midiPitch == 38 || midiPitch == 40)
    {
        const float noise = noiseSample(ageFrames, startFrame, midiPitch);
        const float body = static_cast<float>(std::sin(time * twoPi * 185.0) * std::exp(-time * 18.0));
        return noise * static_cast<float>(std::exp(-time * 22.0)) * 0.72f + body * 0.28f;
    }
    if (midiPitch == 42 || midiPitch == 44 || midiPitch == 46)
    {
        const float current = noiseSample(ageFrames, startFrame, midiPitch);
        const float previous = ageFrames == 0 ? 0.0f : noiseSample(ageFrames - 1, startFrame, midiPitch);
        const double decay = midiPitch == 46 ? 7.0 : 34.0;
        return (current - previous) * 0.42f * static_cast<float>(std::exp(-time * decay));
    }

    const double frequency = 70.0 + 12.0 * std::clamp(midiPitch - 48, 0, 24);
    return static_cast<float>(std::sin(time * twoPi * frequency) * std::exp(-time * 12.0));
}

void SynthRenderer::setProject(std::shared_ptr<const ProjectData> newProject, double newSampleRate)
{
    project = std::move(newProject);
    sampleRate = std::max(1.0, newSampleRate);
    notes.clear();
    activeNotes.clear();
    audioRegions.clear();
    activeAudioRegions.clear();
    nextStartIndex = 0;
    nextAudioStartIndex = 0;
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
            if (clip.audioSource != nullptr && clip.audioSource->sampleRate > 0 &&
                clip.audioSource->channelCount > 0 && clip.audioSource->frameCount() > clip.audioStartFrame)
            {
                AudioRegion region;
                region.startFrame = static_cast<qint64>(std::llround(clip.startBeat * sampleRate *
                                                                    60.0 / project->tempoBpm));
                region.sourceStartFrame = clip.audioStartFrame;
                region.source = clip.audioSource;
                const qint64 clipDuration = static_cast<qint64>(std::llround(
                    clip.lengthBeats * sampleRate * 60.0 / project->tempoBpm));
                const qint64 sourceFrames = clip.audioSource->frameCount() - clip.audioStartFrame;
                const qint64 sourceDuration = static_cast<qint64>(std::ceil(
                    static_cast<double>(sourceFrames) * sampleRate / clip.audioSource->sampleRate));
                region.endFrame = region.startFrame + std::max<qint64>(1,
                    std::min(clipDuration, sourceDuration));
                region.leftGain = leftGain * track.gain;
                region.rightGain = rightGain * track.gain;
                audioRegions.push_back(std::move(region));
                continue;
            }
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
                voice.amplitude = (track.instrumentType == InstrumentType::DrumRack ? 0.48f : 0.22f) *
                                  static_cast<float>(note.velocity) / 127.0f * track.gain;
                voice.leftGain = leftGain;
                voice.rightGain = rightGain;
                voice.waveform = track.waveform;
                voice.midiPitch = note.pitch;
                voice.percussion = track.instrumentType == InstrumentType::DrumRack;
                if (voice.percussion)
                {
                    const double decaySeconds = note.pitch == 46 ? 0.72 :
                        (note.pitch == 35 || note.pitch == 36 ? 0.48 :
                         (note.pitch == 42 || note.pitch == 44 ? 0.22 : 0.36));
                    voice.endFrame = voice.startFrame + static_cast<qint64>(sampleRate * decaySeconds);
                }
                notes.push_back(voice);
            }
        }
    }
    std::sort(notes.begin(), notes.end(), [](const VoiceNote& left, const VoiceNote& right)
    {
        return left.startFrame < right.startFrame;
    });
    std::sort(audioRegions.begin(), audioRegions.end(), [](const AudioRegion& left, const AudioRegion& right)
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
        activeAudioRegions.clear();
        nextStartIndex = 0;
        nextAudioStartIndex = 0;
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

    while (nextAudioStartIndex < audioRegions.size() &&
           audioRegions[nextAudioStartIndex].startFrame <= projectFrame)
        activeAudioRegions.push_back(nextAudioStartIndex++);
    activeAudioRegions.erase(std::remove_if(activeAudioRegions.begin(), activeAudioRegions.end(),
        [&](size_t index) { return projectFrame >= audioRegions[index].endFrame; }), activeAudioRegions.end());

    for (const size_t index : activeNotes)
    {
        const VoiceNote& note = notes[index];
        if (projectFrame < note.startFrame || projectFrame >= note.endFrame + releaseFrames)
            continue;

        const qint64 age = projectFrame - note.startFrame;
        float envelope = age < attackFrames
            ? static_cast<float>(age) / static_cast<float>(attackFrames)
            : 1.0f;
        if (note.percussion)
        {
            const float voice = renderPercussionSample(note.midiPitch, age, note.startFrame, sampleRate) *
                                note.amplitude * envelope;
            sample.left += voice * note.leftGain;
            sample.right += voice * note.rightGain;
            continue;
        }
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

    for (const size_t index : activeAudioRegions)
    {
        const AudioRegion& region = audioRegions[index];
        const qint64 age = projectFrame - region.startFrame;
        const double sourcePosition = region.sourceStartFrame +
            static_cast<double>(age) * region.source->sampleRate / sampleRate;
        const qint64 firstFrame = static_cast<qint64>(sourcePosition);
        const qint64 secondFrame = std::min(firstFrame + 1, region.source->frameCount() - 1);
        if (firstFrame < 0 || firstFrame >= region.source->frameCount())
            continue;
        const float fraction = static_cast<float>(sourcePosition - firstFrame);
        const int channels = region.source->channelCount;
        const auto sampleAt = [&](qint64 frame, int channel)
        {
            const int sourceChannel = channels == 1 ? 0 : std::min(channel, channels - 1);
            return region.source->samples[static_cast<qsizetype>(frame * channels + sourceChannel)];
        };
        const float left = sampleAt(firstFrame, 0) * (1.0f - fraction) + sampleAt(secondFrame, 0) * fraction;
        const float right = channels == 1 ? left :
            sampleAt(firstFrame, 1) * (1.0f - fraction) + sampleAt(secondFrame, 1) * fraction;
        sample.left += left * region.leftGain;
        sample.right += right * region.rightGain;
    }

    const qint64 beatUnitFrames = std::max<qint64>(1, static_cast<qint64>(std::llround(
        sampleRate * 60.0 / project->tempoBpm * 4.0 / project->beatUnit)));
    const qint64 countInTicks = static_cast<qint64>(project->countInBars) * project->beatsPerBar;
    const qint64 countInFrames = countInTicks * beatUnitFrames;
    const qint64 metronomeFrame = projectFrame + countInFrames;
    if (metronomeFrame >= 0 && (project->metronomeEnabled || projectFrame < 0))
    {
        const qint64 tick = metronomeFrame / beatUnitFrames;
        const qint64 age = metronomeFrame % beatUnitFrames;
        const bool downbeat = tick % project->beatsPerBar == 0;
        const qint64 clickLength = static_cast<qint64>(sampleRate * (downbeat ? 0.045 : 0.032));
        if (age < clickLength)
        {
            const double time = static_cast<double>(age) / sampleRate;
            const double frequency = downbeat ? 1480.0 : 960.0;
            const float click = static_cast<float>(std::sin(time * twoPi * frequency) *
                                                   std::exp(-time * (downbeat ? 78.0 : 96.0)) *
                                                   (downbeat ? 0.20 : 0.13));
            sample.left += click;
            sample.right += click;
        }
    }
    return sample;
}
