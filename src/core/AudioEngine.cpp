#include "AudioEngine.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

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

int drumPadForPitch(int pitch)
{
    if (pitch == 35 || pitch == 36) return 0;
    if (pitch == 38 || pitch == 40) return 1;
    if (pitch == 42 || pitch == 44) return 2;
    if (pitch == 46) return 3;
    return -1;
}
}

class AudioEngine::AudioStream final : public QIODevice
{
public:
    explicit AudioStream(AudioEngine& owner) : owner(owner) {}

    qint64 readData(char* destination, qint64 maxBytes) override
    {
        return owner.renderAudio(destination, maxBytes);
    }

    qint64 writeData(const char*, qint64) override
    {
        return -1;
    }

    qint64 bytesAvailable() const override
    {
        return QIODevice::bytesAvailable() + 8192;
    }

private:
    AudioEngine& owner;
};

AudioEngine::AudioEngine()
{
    for (size_t index = 0; index < maximumTrackCount; ++index)
    {
        liveTrackVolumes[index].store(-1.0f, std::memory_order_relaxed);
        liveTrackPans[index].store(2.0f, std::memory_order_relaxed);
        trackPeakValues[index].store(0.0f, std::memory_order_relaxed);
    }
    setProject(ProjectData{});
}

AudioEngine::~AudioEngine()
{
    if (sink)
        sink->stop();
}

bool AudioEngine::initialize()
{
    if (sink)
        return true;

    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull())
    {
        initializationError = QStringLiteral("No audio output device is available.");
        return false;
    }

    const QAudioFormat format = device.preferredFormat();
    if (!format.isValid() || format.bytesPerFrame() <= 0)
    {
        initializationError = QStringLiteral("The audio device returned an unsupported format.");
        return false;
    }

    outputDeviceName = device.description();
    outputSampleRate = format.sampleRate();
    outputChannels = format.channelCount();
    bytesPerFrame = format.bytesPerFrame();
    outputFormat = std::make_unique<QAudioFormat>(format);
    stream = std::make_unique<AudioStream>(*this);
    stream->open(QIODevice::ReadOnly);
    sink = std::make_unique<QAudioSink>(device, format);
    sink->setBufferSize(bytesPerFrame * 2048);
    sink->start(stream.get());

    if (sink->state() == QAudio::StoppedState && sink->error() != QAudio::NoError)
    {
        initializationError = QStringLiteral("Could not start audio output (error %1).")
                                  .arg(static_cast<int>(sink->error()));
        sink.reset();
        stream.reset();
        outputFormat.reset();
        return false;
    }

    initializationError.clear();
    return true;
}

bool AudioEngine::isAudioAvailable() const
{
    return sink != nullptr;
}

QString AudioEngine::deviceName() const
{
    return outputDeviceName;
}

QString AudioEngine::errorMessage() const
{
    return initializationError;
}

void AudioEngine::setProject(const ProjectData& project)
{
    ProjectData safeProject = project;
    safeProject.normalize();
    for (size_t index = 0; index < maximumTrackCount; ++index)
    {
        liveTrackVolumes[index].store(-1.0f, std::memory_order_release);
        liveTrackPans[index].store(2.0f, std::memory_order_release);
        trackPeakValues[index].store(0.0f, std::memory_order_release);
    }
    masterPeakValue.store(0.0f, std::memory_order_release);
    std::atomic_store_explicit(&projectSnapshot,
                               std::make_shared<const ProjectData>(std::move(safeProject)),
                               std::memory_order_release);
}

void AudioEngine::startPlayback()
{
    if (!sink)
        return;
    if (!seekPosition.load(std::memory_order_acquire))
        resetPosition.store(true, std::memory_order_release);
    playing.store(true, std::memory_order_release);
}

void AudioEngine::stopPlayback()
{
    playing.store(false, std::memory_order_release);
    resetPosition.store(true, std::memory_order_release);
    seekPosition.store(false, std::memory_order_release);
}

void AudioEngine::seekToBeat(double beat)
{
    const auto project = std::atomic_load_explicit(&projectSnapshot, std::memory_order_acquire);
    if (!project || !std::isfinite(beat))
        return;
    const double maximumBeat = std::max(0.0, project->songLengthBeats() - 1.0 / 64.0);
    const double clampedBeat = std::clamp(beat, 0.0, maximumBeat);
    const double framesPerBeat = outputSampleRate * 60.0 / project->tempoBpm;
    requestedPositionFrames.store(static_cast<qint64>(std::llround(clampedBeat * framesPerBeat)),
                                  std::memory_order_relaxed);
    resetPosition.store(false, std::memory_order_release);
    seekPosition.store(true, std::memory_order_release);
}

bool AudioEngine::isPlaying() const
{
    return playing.load(std::memory_order_acquire);
}

double AudioEngine::positionBeats() const
{
    const auto project = std::atomic_load_explicit(&projectSnapshot, std::memory_order_acquire);
    if (!project)
        return 0.0;

    const double framesPerBeat = outputSampleRate * 60.0 / project->tempoBpm;
    return framesPerBeat > 0.0
        ? static_cast<double>(publishedPositionFrames.load(std::memory_order_acquire)) / framesPerBeat
        : 0.0;
}

float AudioEngine::trackPeakLevel(quint64 trackId) const
{
    const auto project = std::atomic_load_explicit(&projectSnapshot, std::memory_order_acquire);
    if (!project)
        return 0.0f;
    for (size_t index = 0; index < static_cast<size_t>(project->tracks.size()) &&
                            index < maximumTrackCount; ++index)
    {
        if (project->tracks[static_cast<qsizetype>(index)].id == trackId)
            return trackPeakValues[index].load(std::memory_order_acquire);
    }
    return 0.0f;
}

float AudioEngine::masterPeakLevel() const
{
    return masterPeakValue.load(std::memory_order_acquire);
}

void AudioEngine::setLiveTrackMixOverride(quint64 trackId, int parameter, float value)
{
    if (parameter != 0 && parameter != 1)
        return;
    const auto project = std::atomic_load_explicit(&projectSnapshot, std::memory_order_acquire);
    if (!project)
        return;
    for (size_t index = 0; index < static_cast<size_t>(project->tracks.size()) &&
                            index < maximumTrackCount; ++index)
    {
        if (project->tracks[static_cast<qsizetype>(index)].id != trackId)
            continue;
        if (parameter == 0)
            liveTrackVolumes[index].store(std::clamp(value, 0.0f, 1.0f), std::memory_order_release);
        else
            liveTrackPans[index].store(std::clamp(value, -1.0f, 1.0f), std::memory_order_release);
        return;
    }
}

void AudioEngine::clearLiveTrackMixOverride(quint64 trackId, int parameter)
{
    if (parameter != 0 && parameter != 1)
        return;
    const auto project = std::atomic_load_explicit(&projectSnapshot, std::memory_order_acquire);
    if (!project)
        return;
    for (size_t index = 0; index < static_cast<size_t>(project->tracks.size()) &&
                            index < maximumTrackCount; ++index)
    {
        if (project->tracks[static_cast<qsizetype>(index)].id != trackId)
            continue;
        if (parameter == 0)
            liveTrackVolumes[index].store(-1.0f, std::memory_order_release);
        else
            liveTrackPans[index].store(2.0f, std::memory_order_release);
        return;
    }
}

void AudioEngine::triggerLiveMidiNoteOn(int channel, int midiPitch, int velocity,
                                        const InstrumentTrack& track)
{
    if (track.instrumentType == InstrumentType::Audio)
        return;

    LiveMidiEvent event;
    event.noteOn = true;
    event.channel = std::clamp(channel, 0, 15);
    event.pitch = std::clamp(midiPitch, 0, 127);
    event.velocity = std::clamp(velocity, 1, 127);
    const auto project = std::atomic_load_explicit(&projectSnapshot, std::memory_order_acquire);
    bool hasSolo = false;
    if (project != nullptr)
    {
        for (size_t index = 0; index < static_cast<size_t>(project->tracks.size()) &&
                                index < maximumTrackCount; ++index)
        {
            hasSolo = hasSolo || project->tracks[static_cast<qsizetype>(index)].solo;
            if (project->tracks[static_cast<qsizetype>(index)].id == track.id)
                event.trackIndex = index;
        }
    }
    event.percussion = track.instrumentType == InstrumentType::DrumRack;
    event.waveform = track.waveform;
    const bool suppressedBySolo = hasSolo && !track.solo;
    event.gain = track.muted || suppressedBySolo ? 0.0f : std::clamp(track.gain, 0.0f, 1.0f);
    event.pan = std::clamp(track.pan, -1.0f, 1.0f);
    event.attackSeconds = std::clamp(track.attackSeconds, 0.0f, 5.0f);
    event.decaySeconds = std::clamp(track.decaySeconds, 0.0f, 5.0f);
    event.sustainLevel = std::clamp(track.sustainLevel, 0.0f, 1.0f);
    event.releaseSeconds = std::clamp(track.releaseSeconds, 0.001f, 5.0f);
    if (event.percussion)
    {
        const int pad = drumPadForPitch(event.pitch);
        if (pad >= 0)
            event.sample = track.drumSamples[static_cast<size_t>(pad)].source;
    }
    enqueueLiveMidiEvent(event);
}

void AudioEngine::triggerLiveMidiNoteOff(int channel, int midiPitch)
{
    LiveMidiEvent event;
    event.channel = std::clamp(channel, 0, 15);
    event.pitch = std::clamp(midiPitch, 0, 127);
    enqueueLiveMidiEvent(event);
}

void AudioEngine::stopLiveMidiNotes()
{
    liveMidiPanic.store(true, std::memory_order_release);
}

bool AudioEngine::enqueueLiveMidiEvent(const LiveMidiEvent& event)
{
    const size_t writeIndex = liveMidiWriteIndex.load(std::memory_order_relaxed);
    const size_t nextWriteIndex = (writeIndex + 1) % liveMidiQueueCapacity;
    if (nextWriteIndex == liveMidiReadIndex.load(std::memory_order_acquire))
    {
        // Discard queued events and release every voice on the audio thread so a lost note-off
        // cannot leave a key hanging indefinitely.
        liveMidiPanic.store(true, std::memory_order_release);
        return false;
    }

    liveMidiEvents[writeIndex] = event;
    liveMidiWriteIndex.store(nextWriteIndex, std::memory_order_release);
    return true;
}

void AudioEngine::clearLiveMidiVoices()
{
    for (LiveMidiVoice& voice : liveMidiVoices)
        voice = LiveMidiVoice{};
}

void AudioEngine::applyLiveMidiEvent(const LiveMidiEvent& event, qint64 frame)
{
    if (!event.noteOn)
    {
        LiveMidiVoice* matchedVoice = nullptr;
        for (LiveMidiVoice& voice : liveMidiVoices)
        {
            if (!voice.active || !voice.held || voice.channel != event.channel || voice.pitch != event.pitch)
                continue;
            if (matchedVoice == nullptr || voice.serial > matchedVoice->serial)
                matchedVoice = &voice;
        }
        if (matchedVoice == nullptr || matchedVoice->percussion)
            return;

        const qint64 minimumGateFrames = std::min(matchedVoice->attackFrames,
            std::max<qint64>(1, static_cast<qint64>(std::llround(outputSampleRate * 0.02))));
        const qint64 releaseFrame = std::max(frame, matchedVoice->startFrame + minimumGateFrames);
        const qint64 age = std::max<qint64>(0, releaseFrame - matchedVoice->startFrame);
        float envelope = 1.0f;
        if (age < matchedVoice->attackFrames)
            envelope = static_cast<float>(age) / static_cast<float>(matchedVoice->attackFrames);
        else if (matchedVoice->decayFrames > 0 &&
                 age - matchedVoice->attackFrames < matchedVoice->decayFrames)
        {
            const float progress = static_cast<float>(age - matchedVoice->attackFrames) /
                                   static_cast<float>(matchedVoice->decayFrames);
            envelope = 1.0f - progress * (1.0f - matchedVoice->sustainLevel);
        }
        else
            envelope = matchedVoice->sustainLevel;
        matchedVoice->held = false;
        matchedVoice->releaseFrame = releaseFrame;
        matchedVoice->releaseStartLevel = envelope;
        return;
    }

    LiveMidiVoice* target = nullptr;
    for (LiveMidiVoice& voice : liveMidiVoices)
    {
        if (!voice.active)
        {
            target = &voice;
            break;
        }
        if (target == nullptr || voice.serial < target->serial)
            target = &voice;
    }
    if (target == nullptr)
        return;

    const double panAngle = (static_cast<double>(event.pan) + 1.0) * 0.7853981633974483;
    *target = LiveMidiVoice{};
    target->active = true;
    target->held = true;
    target->percussion = event.percussion;
    target->channel = event.channel;
    target->pitch = event.pitch;
    target->trackIndex = event.trackIndex;
    target->startFrame = frame;
    target->attackFrames = std::max<qint64>(1, static_cast<qint64>(std::llround(
        event.attackSeconds * outputSampleRate)));
    target->decayFrames = std::max<qint64>(0, static_cast<qint64>(std::llround(
        event.decaySeconds * outputSampleRate)));
    target->releaseFrames = std::max<qint64>(1, static_cast<qint64>(std::llround(
        event.releaseSeconds * outputSampleRate)));
    target->serial = nextLiveMidiSerial++;
    target->frequency = 440.0 * std::pow(2.0, (event.pitch - 69) / 12.0);
    target->amplitude = (event.percussion ? 0.48f : 0.22f) *
                        static_cast<float>(event.velocity) / 127.0f * event.gain;
    target->sustainLevel = event.sustainLevel;
    target->leftGain = static_cast<float>(std::cos(panAngle));
    target->rightGain = static_cast<float>(std::sin(panAngle));
    target->waveform = event.waveform;
    target->sample = event.sample;
}

void AudioEngine::renderLiveMidiVoices(StereoSample& mixed, qint64 frame)
{
    liveMidiTouchedTrackCount = 0;
    const auto addTrackFrameSample = [this](size_t trackIndex, float left, float right)
    {
        if (trackIndex >= maximumTrackCount)
            return;
        if (!liveMidiTrackFrameTouched[trackIndex])
        {
            liveMidiTrackFrameTouched[trackIndex] = true;
            liveMidiTouchedTracks[liveMidiTouchedTrackCount++] = trackIndex;
        }
        liveMidiTrackFrameSamples[trackIndex].left += left;
        liveMidiTrackFrameSamples[trackIndex].right += right;
    };

    for (LiveMidiVoice& voice : liveMidiVoices)
    {
        if (!voice.active)
            continue;

        const qint64 age = frame - voice.startFrame;
        if (age < 0)
            continue;

        if (voice.sample != nullptr)
        {
            const double sourcePosition = static_cast<double>(age) * voice.sample->sampleRate /
                                          outputSampleRate;
            const qint64 firstFrame = static_cast<qint64>(sourcePosition);
            if (firstFrame >= voice.sample->frameCount())
            {
                voice = LiveMidiVoice{};
                continue;
            }
            if (firstFrame < 0 || voice.sample->channelCount < 1 || voice.sample->channelCount > 2)
                continue;
            const qint64 secondFrame = std::min(firstFrame + 1, voice.sample->frameCount() - 1);
            const float fraction = static_cast<float>(sourcePosition - firstFrame);
            const int channels = voice.sample->channelCount;
            const auto sampleAt = [&](qint64 sampleFrame, int channel)
            {
                const int sourceChannel = channels == 1 ? 0 : channel;
                return voice.sample->samples[
                    static_cast<qsizetype>(sampleFrame * channels + sourceChannel)];
            };
            const float left = sampleAt(firstFrame, 0) * (1.0f - fraction) +
                               sampleAt(secondFrame, 0) * fraction;
            const float right = channels == 1 ? left :
                sampleAt(firstFrame, 1) * (1.0f - fraction) + sampleAt(secondFrame, 1) * fraction;
            const float leftContribution = left * voice.amplitude * voice.leftGain;
            const float rightContribution = right * voice.amplitude * voice.rightGain;
            mixed.left += leftContribution;
            mixed.right += rightContribution;
            addTrackFrameSample(voice.trackIndex, leftContribution, rightContribution);
            continue;
        }

        float envelope = 1.0f;
        if (voice.percussion)
        {
            envelope = std::min(1.0f, static_cast<float>(age) /
                                       std::max(1.0f, static_cast<float>(outputSampleRate * 0.008)));
            const double duration = voice.pitch == 46 ? 0.72 :
                (voice.pitch == 42 || voice.pitch == 44 ? 0.22 : 0.48);
            if (age >= static_cast<qint64>(outputSampleRate * duration))
            {
                voice = LiveMidiVoice{};
                continue;
            }
        }
        else if (voice.releaseFrame >= 0 && frame >= voice.releaseFrame)
        {
            const qint64 releaseAge = frame - voice.releaseFrame;
            if (releaseAge >= voice.releaseFrames)
            {
                voice = LiveMidiVoice{};
                continue;
            }
            envelope = voice.releaseStartLevel * std::max(0.0f, 1.0f -
                static_cast<float>(releaseAge) / static_cast<float>(voice.releaseFrames));
        }
        else if (age < voice.attackFrames)
            envelope = static_cast<float>(age) / static_cast<float>(voice.attackFrames);
        else if (voice.decayFrames > 0 && age - voice.attackFrames < voice.decayFrames)
        {
            const float progress = static_cast<float>(age - voice.attackFrames) /
                                   static_cast<float>(voice.decayFrames);
            envelope = 1.0f - progress * (1.0f - voice.sustainLevel);
        }
        else
            envelope = voice.sustainLevel;

        const float voiceSample = voice.percussion
            ? renderPercussionSample(voice.pitch, age, voice.startFrame, outputSampleRate) *
                  voice.amplitude * envelope
            : oscillatorSample(voice.waveform, static_cast<double>(age) * voice.frequency / outputSampleRate) *
                  voice.amplitude * envelope;
        const float leftContribution = voiceSample * voice.leftGain;
        const float rightContribution = voiceSample * voice.rightGain;
        mixed.left += leftContribution;
        mixed.right += rightContribution;
        addTrackFrameSample(voice.trackIndex, leftContribution, rightContribution);
    }

    for (size_t index = 0; index < liveMidiTouchedTrackCount; ++index)
    {
        const size_t trackIndex = liveMidiTouchedTracks[index];
        const StereoSample& trackSample = liveMidiTrackFrameSamples[trackIndex];
        liveMidiBlockPeaks[trackIndex] = std::max(liveMidiBlockPeaks[trackIndex],
            std::max(std::abs(trackSample.left), std::abs(trackSample.right)));
        liveMidiTrackFrameSamples[trackIndex] = {};
        liveMidiTrackFrameTouched[trackIndex] = false;
    }
}

void AudioEngine::triggerPreview(int midiPitch, int velocity, SynthWaveform waveform, float pan,
                                 bool percussion)
{
    std::atomic_store_explicit(&previewSampleAsset, std::shared_ptr<const AudioAsset>{},
                               std::memory_order_release);
    const int clampedPitch = std::clamp(midiPitch, 0, 127);
    const double panAngle = (std::clamp(static_cast<double>(pan), -1.0, 1.0) + 1.0) * 0.7853981633974483;
    previewVelocity.store(std::clamp(velocity, 1, 127), std::memory_order_relaxed);
    previewFrequency.store(440.0 * std::pow(2.0, (clampedPitch - 69) / 12.0), std::memory_order_relaxed);
    previewWaveform.store(static_cast<int>(waveform), std::memory_order_relaxed);
    previewPercussion.store(percussion, std::memory_order_relaxed);
    previewLeftGain.store(static_cast<float>(std::cos(panAngle)), std::memory_order_relaxed);
    previewRightGain.store(static_cast<float>(std::sin(panAngle)), std::memory_order_relaxed);
    previewPitch.store(clampedPitch, std::memory_order_release);
    const qint64 start = audioClockFrames.load(std::memory_order_acquire);
    previewStartFrame.store(start, std::memory_order_release);
    const double previewSeconds = percussion && clampedPitch == 46 ? 0.72 :
                                  percussion && (clampedPitch == 42 || clampedPitch == 44) ? 0.22 : 0.48;
    previewEndFrame.store(start + static_cast<qint64>(outputSampleRate * previewSeconds), std::memory_order_release);
}

void AudioEngine::triggerSamplePreview(std::shared_ptr<const AudioAsset> sample, int velocity,
                                       float pan, float gain)
{
    if (sample == nullptr || sample->sampleRate <= 0 || sample->channelCount < 1 ||
        sample->channelCount > 2 || sample->frameCount() <= 0)
        return;

    const double panAngle = (std::clamp(static_cast<double>(pan), -1.0, 1.0) + 1.0) *
                            0.7853981633974483;
    previewVelocity.store(std::clamp(velocity, 1, 127), std::memory_order_relaxed);
    previewSampleGain.store(std::clamp(gain, 0.0f, 1.0f), std::memory_order_relaxed);
    previewLeftGain.store(static_cast<float>(std::cos(panAngle)), std::memory_order_relaxed);
    previewRightGain.store(static_cast<float>(std::sin(panAngle)), std::memory_order_relaxed);
    const qint64 start = audioClockFrames.load(std::memory_order_acquire);
    const qint64 duration = static_cast<qint64>(std::ceil(
        static_cast<double>(sample->frameCount()) * outputSampleRate / sample->sampleRate));
    previewStartFrame.store(start, std::memory_order_release);
    previewEndFrame.store(start + std::max<qint64>(1, duration), std::memory_order_release);
    std::atomic_store_explicit(&previewSampleAsset, std::move(sample), std::memory_order_release);
    previewPitch.store(0, std::memory_order_release);
}

void AudioEngine::setMasterGain(float gain)
{
    masterGain.store(std::clamp(gain, 0.0f, 1.0f), std::memory_order_release);
}

void AudioEngine::prepareNotes(const std::shared_ptr<const ProjectData>& project)
{
    if (preparedProject.get() == project.get())
        return;
    renderer.setProject(project, outputSampleRate);
    preparedProject = project;
}

qint64 AudioEngine::renderAudio(char* destination, qint64 maxBytes)
{
    if (destination == nullptr || !outputFormat || bytesPerFrame <= 0)
        return 0;

    const int frames = static_cast<int>(maxBytes / bytesPerFrame);
    if (frames <= 0)
        return 0;

    // The UI publishes immutable snapshots; this callback takes one snapshot per block.
    const auto project = std::atomic_load_explicit(&projectSnapshot, std::memory_order_acquire);
    if (!project)
        return 0;
    prepareNotes(project);
    renderer.resetTrackPeakLevels();
    liveMidiBlockPeaks.fill(0.0f);
    for (size_t index = 0; index < static_cast<size_t>(project->tracks.size()) &&
                            index < maximumTrackCount; ++index)
    {
        renderer.setLiveTrackMixOverride(index, 0,
            liveTrackVolumes[index].load(std::memory_order_acquire));
        renderer.setLiveTrackMixOverride(index, 1,
            liveTrackPans[index].load(std::memory_order_acquire));
    }
    const std::shared_ptr<const AudioAsset> previewSample =
        std::atomic_load_explicit(&previewSampleAsset, std::memory_order_acquire);

    if (seekPosition.exchange(false, std::memory_order_acq_rel))
    {
        playbackFrame = requestedPositionFrames.load(std::memory_order_acquire);
        renderer.resetTransport();
        resetPosition.store(false, std::memory_order_release);
    }
    else if (resetPosition.exchange(false, std::memory_order_acq_rel))
    {
        const qint64 beatUnitFrames = std::max<qint64>(1, static_cast<qint64>(std::llround(
            outputSampleRate * 60.0 / project->tempoBpm * 4.0 / project->beatUnit)));
        const qint64 countInFrames = static_cast<qint64>(project->countInBars) *
                                     project->beatsPerBar * beatUnitFrames;
        playbackFrame = -countInFrames;
    }

    bool playingState = playing.load(std::memory_order_acquire);
    const qint64 patternFrames = std::max<qint64>(1, static_cast<qint64>(std::llround(
        project->songLengthBeats() * outputSampleRate * 60.0 / project->tempoBpm)));
    const qint64 releaseTailFrames = static_cast<qint64>(outputSampleRate * 0.08);
    const qint64 clockStart = audioClockFrames.load(std::memory_order_relaxed);
    const auto sampleFormat = outputFormat->sampleFormat();

    if (liveMidiPanic.exchange(false, std::memory_order_acq_rel))
    {
        clearLiveMidiVoices();
        const size_t writeIndex = liveMidiWriteIndex.load(std::memory_order_acquire);
        liveMidiReadIndex.store(writeIndex, std::memory_order_release);
    }
    size_t midiReadIndex = liveMidiReadIndex.load(std::memory_order_relaxed);
    size_t midiWriteIndex = liveMidiWriteIndex.load(std::memory_order_acquire);
    while (midiReadIndex != midiWriteIndex)
    {
        const LiveMidiEvent event = liveMidiEvents[midiReadIndex];
        midiReadIndex = (midiReadIndex + 1) % liveMidiQueueCapacity;
        liveMidiReadIndex.store(midiReadIndex, std::memory_order_release);
        applyLiveMidiEvent(event, clockStart);
        midiWriteIndex = liveMidiWriteIndex.load(std::memory_order_acquire);
    }

    float blockMasterPeak = 0.0f;
    for (int frameIndex = 0; frameIndex < frames; ++frameIndex)
    {
        if (playingState && !project->loopEnabled && playbackFrame >= patternFrames + releaseTailFrames)
        {
            playingState = false;
            playing.store(false, std::memory_order_release);
        }
        qint64 currentFrame = playbackFrame;
        if (project->loopEnabled && playbackFrame >= 0)
            currentFrame %= patternFrames;

        StereoSample mixed = playingState ? renderer.renderSample(currentFrame) : StereoSample{};
        const qint64 clockFrame = clockStart + frameIndex;
        renderLiveMidiVoices(mixed, clockFrame);
        const int preview = previewPitch.load(std::memory_order_acquire);
        const qint64 previewStart = previewStartFrame.load(std::memory_order_acquire);
        const qint64 previewEnd = previewEndFrame.load(std::memory_order_acquire);
        if (preview >= 0 && clockFrame >= previewStart && clockFrame < previewEnd)
        {
            const qint64 age = clockFrame - previewStart;
            const float velocity = static_cast<float>(previewVelocity.load(std::memory_order_relaxed)) / 127.0f;
            if (previewSample != nullptr)
            {
                const double sourcePosition = static_cast<double>(age) * previewSample->sampleRate /
                                              outputSampleRate;
                const qint64 firstFrame = static_cast<qint64>(sourcePosition);
                if (firstFrame >= 0 && firstFrame < previewSample->frameCount())
                {
                    const qint64 secondFrame = std::min(firstFrame + 1, previewSample->frameCount() - 1);
                    const float fraction = static_cast<float>(sourcePosition - firstFrame);
                    const int channels = previewSample->channelCount;
                    const auto sampleAt = [&](qint64 frame, int channel)
                    {
                        const int sourceChannel = channels == 1 ? 0 : channel;
                        return previewSample->samples[
                            static_cast<qsizetype>(frame * channels + sourceChannel)];
                    };
                    const float left = sampleAt(firstFrame, 0) * (1.0f - fraction) +
                                       sampleAt(secondFrame, 0) * fraction;
                    const float right = channels == 1 ? left :
                        sampleAt(firstFrame, 1) * (1.0f - fraction) +
                        sampleAt(secondFrame, 1) * fraction;
                    const float sampleGain = 0.48f * velocity *
                                             previewSampleGain.load(std::memory_order_relaxed);
                    mixed.left += left * sampleGain * previewLeftGain.load(std::memory_order_relaxed);
                    mixed.right += right * sampleGain * previewRightGain.load(std::memory_order_relaxed);
                }
            }
            else
            {
                const bool percussion = previewPercussion.load(std::memory_order_relaxed);
                const float attack = std::min(1.0f, static_cast<float>(age) /
                                                      std::max(1.0f, static_cast<float>(outputSampleRate * 0.008)));
                float preview = 0.0f;
                if (percussion)
                {
                    preview = renderPercussionSample(previewPitch.load(std::memory_order_relaxed), age,
                                                     previewStart, outputSampleRate) *
                              (0.48f * velocity) * attack;
                }
                else
                {
                    const double frequency = previewFrequency.load(std::memory_order_relaxed);
                    const double phase = static_cast<double>(age) * frequency / outputSampleRate;
                    const float release = std::min(1.0f, static_cast<float>(previewEnd - clockFrame) /
                                                           std::max(1.0f, static_cast<float>(outputSampleRate * 0.035)));
                    preview = oscillatorSample(static_cast<SynthWaveform>(previewWaveform.load(std::memory_order_relaxed)), phase) *
                              (0.2f * velocity) * attack * release;
                }
                mixed.left += preview * previewLeftGain.load(std::memory_order_relaxed);
                mixed.right += preview * previewRightGain.load(std::memory_order_relaxed);
            }
        }

        const float gain = masterGain.load(std::memory_order_relaxed);
        mixed.left = std::tanh(mixed.left * gain);
        mixed.right = std::tanh(mixed.right * gain);
        blockMasterPeak = std::max(blockMasterPeak, std::max(std::abs(mixed.left), std::abs(mixed.right)));
        for (int channel = 0; channel < outputChannels; ++channel)
        {
            const int byteOffset = (frameIndex * outputChannels + channel) * outputFormat->bytesPerSample();
            const float sample = outputChannels == 1
                ? (mixed.left + mixed.right) * 0.5f
                : channel == 0 ? mixed.left : channel == 1 ? mixed.right
                : channel == 2 ? (mixed.left + mixed.right) * 0.5f : 0.0f;
            switch (sampleFormat)
            {
                case QAudioFormat::Float:
                    std::memcpy(destination + byteOffset, &sample, sizeof(float));
                    break;
                case QAudioFormat::Int16:
                {
                    const qint16 value = static_cast<qint16>(std::lrint(std::clamp(sample, -1.0f, 1.0f) * 32767.0f));
                    std::memcpy(destination + byteOffset, &value, sizeof(value));
                    break;
                }
                case QAudioFormat::Int32:
                {
                    const qint32 value = static_cast<qint32>(std::llround(std::clamp(sample, -1.0f, 1.0f) * 2147483647.0));
                    std::memcpy(destination + byteOffset, &value, sizeof(value));
                    break;
                }
                case QAudioFormat::UInt8:
                {
                    const quint8 value = static_cast<quint8>(std::lrint((std::clamp(sample, -1.0f, 1.0f) + 1.0f) * 127.5f));
                    std::memcpy(destination + byteOffset, &value, sizeof(value));
                    break;
                }
                case QAudioFormat::Unknown:
                default:
                    std::memset(destination + byteOffset, 0, static_cast<size_t>(outputFormat->bytesPerSample()));
                    break;
            }
        }

        if (playingState)
            ++playbackFrame;
    }

    const size_t meterTrackCount = std::min(static_cast<size_t>(project->tracks.size()), maximumTrackCount);
    for (size_t index = 0; index < meterTrackCount; ++index)
    {
        const float blockPeak = std::max(renderer.trackPeakLevel(index), liveMidiBlockPeaks[index]);
        const float decayedPeak = trackPeakValues[index].load(std::memory_order_relaxed) * 0.82f;
        trackPeakValues[index].store(std::max(blockPeak, decayedPeak), std::memory_order_release);
    }
    const float decayedMasterPeak = masterPeakValue.load(std::memory_order_relaxed) * 0.82f;
    masterPeakValue.store(std::max(blockMasterPeak, decayedMasterPeak), std::memory_order_release);

    audioClockFrames.store(clockStart + frames, std::memory_order_release);
    const qint64 publishedPosition = playingState
        ? (project->loopEnabled ? playbackFrame % patternFrames : std::min(playbackFrame, patternFrames))
        : 0;
    publishedPositionFrames.store(publishedPosition, std::memory_order_release);
    return static_cast<qint64>(frames) * bytesPerFrame;
}
