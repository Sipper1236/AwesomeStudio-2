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
    std::atomic_store_explicit(&projectSnapshot,
                               std::make_shared<const ProjectData>(std::move(safeProject)),
                               std::memory_order_release);
}

void AudioEngine::startPlayback()
{
    if (!sink)
        return;
    resetPosition.store(true, std::memory_order_release);
    playing.store(true, std::memory_order_release);
}

void AudioEngine::stopPlayback()
{
    playing.store(false, std::memory_order_release);
    resetPosition.store(true, std::memory_order_release);
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

void AudioEngine::triggerPreview(int midiPitch, int velocity, SynthWaveform waveform, float pan,
                                 bool percussion)
{
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

    if (resetPosition.exchange(false, std::memory_order_acq_rel))
        playbackFrame = 0;

    bool playingState = playing.load(std::memory_order_acquire);
    const qint64 patternFrames = std::max<qint64>(1, static_cast<qint64>(std::llround(
        project->songLengthBeats() * outputSampleRate * 60.0 / project->tempoBpm)));
    const qint64 releaseTailFrames = static_cast<qint64>(outputSampleRate * 0.08);
    const qint64 clockStart = audioClockFrames.load(std::memory_order_relaxed);
    const auto sampleFormat = outputFormat->sampleFormat();

    for (int frameIndex = 0; frameIndex < frames; ++frameIndex)
    {
        if (playingState && !project->loopEnabled && playbackFrame >= patternFrames + releaseTailFrames)
        {
            playingState = false;
            playing.store(false, std::memory_order_release);
        }
        qint64 currentFrame = playbackFrame;
        if (project->loopEnabled)
            currentFrame %= patternFrames;

        StereoSample mixed = playingState ? renderer.renderSample(currentFrame) : StereoSample{};
        const qint64 clockFrame = clockStart + frameIndex;
        const int preview = previewPitch.load(std::memory_order_acquire);
        const qint64 previewStart = previewStartFrame.load(std::memory_order_acquire);
        const qint64 previewEnd = previewEndFrame.load(std::memory_order_acquire);
        if (preview >= 0 && clockFrame >= previewStart && clockFrame < previewEnd)
        {
            const qint64 age = clockFrame - previewStart;
            const bool percussion = previewPercussion.load(std::memory_order_relaxed);
            const float velocity = static_cast<float>(previewVelocity.load(std::memory_order_relaxed)) / 127.0f;
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

        const float gain = masterGain.load(std::memory_order_relaxed);
        mixed.left = std::tanh(mixed.left * gain);
        mixed.right = std::tanh(mixed.right * gain);
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

    audioClockFrames.store(clockStart + frames, std::memory_order_release);
    const qint64 publishedPosition = playingState
        ? (project->loopEnabled ? playbackFrame % patternFrames : std::min(playbackFrame, patternFrames))
        : 0;
    publishedPositionFrames.store(publishedPosition, std::memory_order_release);
    return static_cast<qint64>(frames) * bytesPerFrame;
}
