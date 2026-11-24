#include "AudioEngine.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>
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

float envelopeLevel(qint64 ageFrames, qint64 attackFrames, qint64 decayFrames, float sustainLevel)
{
    if (ageFrames < 0)
        return 0.0f;
    if (attackFrames > 0 && ageFrames < attackFrames)
        return static_cast<float>(ageFrames) / static_cast<float>(attackFrames);
    const qint64 decayAge = ageFrames - attackFrames;
    if (decayFrames > 0 && decayAge < decayFrames)
    {
        const float progress = static_cast<float>(decayAge) / static_cast<float>(decayFrames);
        return 1.0f - progress * (1.0f - sustainLevel);
    }
    return sustainLevel;
}

struct BiquadCoefficients
{
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
};

BiquadCoefficients synthFilterCoefficients(SynthFilterType type, double cutoff,
                                           double resonance, double sampleRate)
{
    cutoff = std::clamp(cutoff, 20.0, std::max(20.0, sampleRate * 0.49));
    resonance = std::clamp(resonance, 0.1, 10.0);
    const double omega = twoPi * cutoff / sampleRate;
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) / (2.0 * resonance);
    const double a0 = 1.0 + alpha;
    BiquadCoefficients coefficients;
    switch (type)
    {
        case SynthFilterType::HighPass:
            coefficients.b0 = static_cast<float>(((1.0 + cosine) * 0.5) / a0);
            coefficients.b1 = static_cast<float>(-(1.0 + cosine) / a0);
            coefficients.b2 = coefficients.b0;
            break;
        case SynthFilterType::BandPass:
            coefficients.b0 = static_cast<float>(alpha / a0);
            coefficients.b1 = 0.0f;
            coefficients.b2 = -coefficients.b0;
            break;
        case SynthFilterType::Notch:
            coefficients.b0 = static_cast<float>(1.0 / a0);
            coefficients.b1 = static_cast<float>((-2.0 * cosine) / a0);
            coefficients.b2 = coefficients.b0;
            break;
        case SynthFilterType::LowPass:
        default:
            coefficients.b0 = static_cast<float>(((1.0 - cosine) * 0.5) / a0);
            coefficients.b1 = static_cast<float>((1.0 - cosine) / a0);
            coefficients.b2 = coefficients.b0;
            break;
    }
    coefficients.a1 = static_cast<float>((-2.0 * cosine) / a0);
    coefficients.a2 = static_cast<float>((1.0 - alpha) / a0);
    return coefficients;
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
    liveMidiChannelPitchBendValues.fill(8192);
    for (size_t index = 0; index < maximumTrackCount; ++index)
    {
        liveTrackVolumes[index].store(-1.0f, std::memory_order_relaxed);
        liveTrackPans[index].store(2.0f, std::memory_order_relaxed);
        for (auto& gain : liveTrackEqGainDb[index])
            gain.store(1000.0f, std::memory_order_relaxed);
        for (auto& crossover : liveTrackEqCrossoverHz[index])
            crossover.store(-1.0f, std::memory_order_relaxed);
        for (auto& band : liveTrackParametricEqValues[index])
        {
            band[0].store(-1.0f, std::memory_order_relaxed);
            band[1].store(1000.0f, std::memory_order_relaxed);
            band[2].store(-1.0f, std::memory_order_relaxed);
        }
        liveSynthFilterCutoffOverrides[index].store(-1.0f, std::memory_order_relaxed);
        liveSynthFilterResonanceOverrides[index].store(-1.0f, std::memory_order_relaxed);
        trackPeakValues[index].store(0.0f, std::memory_order_relaxed);
    }
    for (auto& peak : busPeakValues)
        peak.store(0.0f, std::memory_order_relaxed);
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

    QAudioDevice device;
    if (selectedOutputDeviceId.isEmpty())
    {
        device = QMediaDevices::defaultAudioOutput();
    }
    else
    {
        for (const QAudioDevice& candidate : QMediaDevices::audioOutputs())
            if (candidate.id() == selectedOutputDeviceId)
            {
                device = candidate;
                break;
            }
    }
    if (device.isNull())
    {
        initializationError = selectedOutputDeviceId.isEmpty()
            ? QStringLiteral("No audio output device is available.")
            : QStringLiteral("The selected audio output device is not available.");
        return false;
    }

    const QAudioFormat format = device.preferredFormat();
    if (!format.isValid() || format.bytesPerFrame() <= 0)
    {
        initializationError = QStringLiteral("The audio device returned an unsupported format.");
        return false;
    }

    outputDeviceName = device.description();
    activeOutputDeviceId = device.id();
    selectedOutputDeviceId = activeOutputDeviceId;
    outputSampleRate = format.sampleRate();
    outputChannels = format.channelCount();
    bytesPerFrame = format.bytesPerFrame();
    outputFormat = std::make_unique<QAudioFormat>(format);
    const auto preparedState = std::atomic_load_explicit(&preparedRenderState,
                                                          std::memory_order_acquire);
    if (preparedState != nullptr && preparedState->project != nullptr)
        setProject(*preparedState->project);
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

QByteArray AudioEngine::audioOutputDeviceId() const
{
    return activeOutputDeviceId.isEmpty() ? selectedOutputDeviceId : activeOutputDeviceId;
}

bool AudioEngine::setAudioOutputDevice(const QByteArray& deviceId)
{
    if (playing.load(std::memory_order_acquire))
    {
        initializationError = QStringLiteral("Stop playback before changing the audio output device.");
        return false;
    }
    QAudioDevice requestedDevice;
    for (const QAudioDevice& candidate : QMediaDevices::audioOutputs())
        if (candidate.id() == deviceId)
        {
            requestedDevice = candidate;
            break;
        }
    if (requestedDevice.isNull())
    {
        initializationError = QStringLiteral("The selected audio output device is not available.");
        return false;
    }
    if (deviceId == selectedOutputDeviceId &&
        (sink == nullptr || deviceId == activeOutputDeviceId))
        return true;

    const QByteArray previousDeviceId = selectedOutputDeviceId;
    if (sink != nullptr)
    {
        pluginStateCaptureInProgress.store(true, std::memory_order_release);
        sink->stop();
        while (activeRenderCallbacks.load(std::memory_order_acquire) != 0)
            std::this_thread::yield();
        sink.reset();
        stream.reset();
        outputFormat.reset();
        activeOutputDeviceId.clear();
        bytesPerFrame = 0;
        pluginStateCaptureInProgress.store(false, std::memory_order_release);
    }

    selectedOutputDeviceId = deviceId;
    if (initialize())
    {
        initializationError.clear();
        return true;
    }

    const QString requestedDeviceError = initializationError;
    selectedOutputDeviceId = previousDeviceId;
    if (!previousDeviceId.isEmpty())
        initialize();
    initializationError = requestedDeviceError;
    return false;
}

void AudioEngine::handleAudioOutputsChanged()
{
    if (playing.load(std::memory_order_acquire))
    {
        stopPlayback();
        initializationError = QStringLiteral(
            "The audio output device changed while playing. Playback was stopped; select an available output and restart.");
    }

    if (activeOutputDeviceId.isEmpty())
    {
        if (sink == nullptr)
            initialize();
        return;
    }

    const QList<QAudioDevice> devices = QMediaDevices::audioOutputs();
    const bool activeDeviceAvailable = std::any_of(devices.cbegin(), devices.cend(),
        [this](const QAudioDevice& device) { return device.id() == activeOutputDeviceId; });
    if (activeDeviceAvailable)
        return;

    if (sink != nullptr)
    {
        pluginStateCaptureInProgress.store(true, std::memory_order_release);
        sink->stop();
        while (activeRenderCallbacks.load(std::memory_order_acquire) != 0)
            std::this_thread::yield();
        sink.reset();
        stream.reset();
        outputFormat.reset();
        activeOutputDeviceId.clear();
        bytesPerFrame = 0;
        pluginStateCaptureInProgress.store(false, std::memory_order_release);
    }

    selectedOutputDeviceId.clear();
    if (initialize())
    {
        initializationError.clear();
        return;
    }

    initializationError = devices.isEmpty()
        ? QStringLiteral("The audio output device was disconnected. Connect an output device to resume audio.")
        : initializationError;
}

QString AudioEngine::errorMessage() const
{
    return initializationError;
}

void AudioEngine::setProject(const ProjectData& project)
{
    ProjectData safeProject = project;
    safeProject.normalize();
    liveMidiPitchBendRangeSemitones.store(
        static_cast<float>(safeProject.pitchBendRangeSemitones), std::memory_order_release);
    for (size_t index = 0; index < maximumTrackCount; ++index)
    {
        liveTrackVolumes[index].store(-1.0f, std::memory_order_release);
        liveTrackPans[index].store(2.0f, std::memory_order_release);
        for (auto& gain : liveTrackEqGainDb[index])
            gain.store(1000.0f, std::memory_order_release);
        for (auto& crossover : liveTrackEqCrossoverHz[index])
            crossover.store(-1.0f, std::memory_order_release);
        for (auto& band : liveTrackParametricEqValues[index])
        {
            band[0].store(-1.0f, std::memory_order_release);
            band[1].store(1000.0f, std::memory_order_release);
            band[2].store(-1.0f, std::memory_order_release);
        }
        liveSynthFilterCutoffOverrides[index].store(-1.0f, std::memory_order_release);
        liveSynthFilterResonanceOverrides[index].store(-1.0f, std::memory_order_release);
        trackPeakValues[index].store(0.0f, std::memory_order_release);
    }
    for (auto& peak : busPeakValues)
        peak.store(0.0f, std::memory_order_release);
    masterPeakValue.store(0.0f, std::memory_order_release);
    auto nextState = std::make_shared<PreparedRenderState>();
    nextState->project = std::make_shared<const ProjectData>(std::move(safeProject));
    nextState->renderer = std::make_shared<SynthRenderer>();
    nextState->renderer->setProject(nextState->project, outputSampleRate);
    nextState->generation = nextProjectGeneration.fetch_add(1, std::memory_order_relaxed);
    std::shared_ptr<const PreparedRenderState> immutableState = std::move(nextState);
    auto previousState = std::atomic_exchange_explicit(&preparedRenderState,
        std::move(immutableState), std::memory_order_acq_rel);
    if (previousState != nullptr)
        retiredRenderStates.push_back(std::move(previousState));
    for (auto state = retiredRenderStates.begin(); state != retiredRenderStates.end();)
    {
        if (state->use_count() == 1)
            state = retiredRenderStates.erase(state);
        else
            ++state;
    }
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
    const auto state = std::atomic_load_explicit(&preparedRenderState, std::memory_order_acquire);
    const auto project = state == nullptr ? std::shared_ptr<const ProjectData>{} : state->project;
    if (!project || !std::isfinite(beat))
        return;
    const double maximumBeat = std::max(0.0, project->songLengthBeats() - 1.0 / 64.0);
    const double clampedBeat = std::clamp(beat, 0.0, maximumBeat);
    requestedPositionFrames.store(project->frameAtBeat(clampedBeat, outputSampleRate),
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
    const auto state = std::atomic_load_explicit(&preparedRenderState, std::memory_order_acquire);
    const auto project = state == nullptr ? std::shared_ptr<const ProjectData>{} : state->project;
    if (!project)
        return 0.0;

    qint64 outputFrame = -1;
    if (sink != nullptr && sink->state() != QAudio::StoppedState && outputSampleRate > 0.0)
    {
        const qint64 processedMicroseconds = sink->processedUSecs();
        if (processedMicroseconds >= 0)
        {
            const long double processedFrames = static_cast<long double>(processedMicroseconds) *
                static_cast<long double>(outputSampleRate) / 1000000.0L;
            if (processedFrames <= static_cast<long double>(std::numeric_limits<qint64>::max()))
                outputFrame = static_cast<qint64>(std::floor(processedFrames));
        }
    }

    if (outputFrame >= 0 && state != nullptr)
    {
        const quint64 latestSegment = transportOutputHistorySize.load(std::memory_order_acquire);
        const quint64 oldestSegment = latestSegment > transportOutputHistoryCapacity
            ? latestSegment - transportOutputHistoryCapacity + 1 : 1;
        for (quint64 serial = latestSegment; serial >= oldestSegment && serial != 0; --serial)
        {
            const TransportOutputSegment& segment =
                transportOutputHistory[(serial - 1) % transportOutputHistoryCapacity];
            if (segment.serial.load(std::memory_order_acquire) != serial)
                continue;

            const quint64 generation = segment.projectGeneration.load(std::memory_order_relaxed);
            const qint64 startFrame = segment.outputStartFrame.load(std::memory_order_relaxed);
            const qint64 endFrame = segment.outputEndFrame.load(std::memory_order_relaxed);
            const qint64 projectStartFrame = segment.projectStartFrame.load(std::memory_order_relaxed);
            const qint64 advancingFrames = segment.advancingFrames.load(std::memory_order_relaxed);
            const qint64 patternFrameCount = segment.patternFrames.load(std::memory_order_relaxed);
            const bool loopEnabled = segment.loopEnabled.load(std::memory_order_relaxed);
            const bool advancesProjectFrame =
                segment.advancesProjectFrame.load(std::memory_order_relaxed);
            if (segment.serial.load(std::memory_order_acquire) != serial ||
                generation != state->generation || outputFrame < startFrame || outputFrame >= endFrame)
                continue;

            qint64 projectFrame = projectStartFrame;
            const qint64 offset = outputFrame - startFrame;
            if (advancesProjectFrame)
            {
                if (offset >= advancingFrames)
                    projectFrame = 0;
                else
                    projectFrame += offset;
                if (loopEnabled && projectFrame >= 0 && patternFrameCount > 0)
                    projectFrame %= patternFrameCount;
                else if (!loopEnabled)
                    projectFrame = std::min(projectFrame, patternFrameCount);
            }
            return project->beatAtFrame(projectFrame, outputSampleRate);
        }
    }

    return project->beatAtFrame(
        publishedPositionFrames.load(std::memory_order_acquire), outputSampleRate);
}

void AudioEngine::publishTransportOutputSegment(quint64 projectGeneration,
                                                qint64 outputStartFrame,
                                                qint64 projectStartFrame,
                                                qint64 frameCount,
                                                qint64 advancingFrames,
                                                qint64 patternFrames,
                                                bool loopEnabled,
                                                bool advancesProjectFrame)
{
    if (frameCount <= 0)
        return;

    const quint64 serial = transportOutputHistorySize.load(std::memory_order_relaxed) + 1;
    TransportOutputSegment& segment =
        transportOutputHistory[(serial - 1) % transportOutputHistoryCapacity];
    segment.serial.store(0, std::memory_order_release);
    segment.projectGeneration.store(projectGeneration, std::memory_order_relaxed);
    segment.outputStartFrame.store(outputStartFrame, std::memory_order_relaxed);
    segment.outputEndFrame.store(outputStartFrame + frameCount, std::memory_order_relaxed);
    segment.projectStartFrame.store(projectStartFrame, std::memory_order_relaxed);
    segment.advancingFrames.store(std::clamp<qint64>(advancingFrames, 0, frameCount),
                                  std::memory_order_relaxed);
    segment.patternFrames.store(std::max<qint64>(1, patternFrames), std::memory_order_relaxed);
    segment.loopEnabled.store(loopEnabled, std::memory_order_relaxed);
    segment.advancesProjectFrame.store(advancesProjectFrame, std::memory_order_relaxed);
    segment.serial.store(serial, std::memory_order_release);
    transportOutputHistorySize.store(serial, std::memory_order_release);
}

float AudioEngine::trackPeakLevel(quint64 trackId) const
{
    const auto state = std::atomic_load_explicit(&preparedRenderState, std::memory_order_acquire);
    const auto project = state == nullptr ? std::shared_ptr<const ProjectData>{} : state->project;
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

float AudioEngine::busPeakLevel(size_t busIndex) const
{
    return busIndex < maximumBusCount ? busPeakValues[busIndex].load(std::memory_order_acquire) : 0.0f;
}

float AudioEngine::masterPeakLevel() const
{
    return masterPeakValue.load(std::memory_order_acquire);
}

void AudioEngine::setLiveTrackMixOverride(quint64 trackId, int parameter, float value)
{
    const bool eqGain = parameter >= trackEqLowGainAutomationTarget &&
                        parameter <= trackEqHighGainAutomationTarget;
    const bool eqLowCrossover = parameter == trackEqLowCrossoverAutomationTarget;
    const bool eqHighCrossover = parameter == trackEqHighCrossoverAutomationTarget;
    const int parametricEncoded = parameter - trackEqParametricAutomationTargetBase;
    const bool parametricEq = parametricEncoded >= 0 &&
        parametricEncoded < parametricEqBandCount * 3;
    if (parameter != 0 && parameter != 1 && !eqGain &&
        !eqLowCrossover && !eqHighCrossover && !parametricEq)
        return;
    const auto state = std::atomic_load_explicit(&preparedRenderState, std::memory_order_acquire);
    const auto project = state == nullptr ? std::shared_ptr<const ProjectData>{} : state->project;
    if (!project)
        return;
    for (size_t index = 0; index < static_cast<size_t>(project->tracks.size()) &&
                            index < maximumTrackCount; ++index)
    {
        if (project->tracks[static_cast<qsizetype>(index)].id != trackId)
            continue;
        if (parameter == 0)
            liveTrackVolumes[index].store(std::clamp(value, 0.0f, 1.0f), std::memory_order_release);
        else if (parameter == 1)
            liveTrackPans[index].store(std::clamp(value, -1.0f, 1.0f), std::memory_order_release);
        else if (eqGain)
            liveTrackEqGainDb[index][static_cast<size_t>(
                parameter - trackEqLowGainAutomationTarget)].store(
                    std::clamp(value, -18.0f, 18.0f), std::memory_order_release);
        else if (eqLowCrossover)
            liveTrackEqCrossoverHz[index][0].store(std::clamp(value, 20.0f, 2000.0f),
                                                   std::memory_order_release);
        else if (eqHighCrossover)
            liveTrackEqCrossoverHz[index][1].store(std::clamp(value, 500.0f, 20000.0f),
                                                   std::memory_order_release);
        else if (parametricEq)
        {
            const size_t band = static_cast<size_t>(parametricEncoded / 3);
            const size_t eqParameter = static_cast<size_t>(parametricEncoded % 3);
            const std::array<float, 3> minimum = {20.0f, -18.0f, 0.1f};
            const std::array<float, 3> maximum = {20000.0f, 18.0f, 10.0f};
            liveTrackParametricEqValues[index][band][eqParameter].store(
                std::clamp(value, minimum[eqParameter], maximum[eqParameter]),
                std::memory_order_release);
        }
        return;
    }
}

void AudioEngine::clearLiveTrackMixOverride(quint64 trackId, int parameter)
{
    const bool eqGain = parameter >= trackEqLowGainAutomationTarget &&
                        parameter <= trackEqHighGainAutomationTarget;
    const bool eqLowCrossover = parameter == trackEqLowCrossoverAutomationTarget;
    const bool eqHighCrossover = parameter == trackEqHighCrossoverAutomationTarget;
    const int parametricEncoded = parameter - trackEqParametricAutomationTargetBase;
    const bool parametricEq = parametricEncoded >= 0 &&
        parametricEncoded < parametricEqBandCount * 3;
    if (parameter != 0 && parameter != 1 && !eqGain &&
        !eqLowCrossover && !eqHighCrossover && !parametricEq)
        return;
    const auto state = std::atomic_load_explicit(&preparedRenderState, std::memory_order_acquire);
    const auto project = state == nullptr ? std::shared_ptr<const ProjectData>{} : state->project;
    if (!project)
        return;
    for (size_t index = 0; index < static_cast<size_t>(project->tracks.size()) &&
                            index < maximumTrackCount; ++index)
    {
        if (project->tracks[static_cast<qsizetype>(index)].id != trackId)
            continue;
        if (parameter == 0)
            liveTrackVolumes[index].store(-1.0f, std::memory_order_release);
        else if (parameter == 1)
            liveTrackPans[index].store(2.0f, std::memory_order_release);
        else if (eqGain)
            liveTrackEqGainDb[index][static_cast<size_t>(
                parameter - trackEqLowGainAutomationTarget)].store(
                    1000.0f, std::memory_order_release);
        else if (eqLowCrossover)
            liveTrackEqCrossoverHz[index][0].store(-1.0f, std::memory_order_release);
        else if (eqHighCrossover)
            liveTrackEqCrossoverHz[index][1].store(-1.0f, std::memory_order_release);
        else if (parametricEq)
        {
            const size_t band = static_cast<size_t>(parametricEncoded / 3);
            const size_t eqParameter = static_cast<size_t>(parametricEncoded % 3);
            const float defaultValue = eqParameter == 1 ? 1000.0f : -1.0f;
            liveTrackParametricEqValues[index][band][eqParameter].store(
                defaultValue, std::memory_order_release);
        }
        return;
    }
}

void AudioEngine::setLiveSynthFilterOverride(quint64 trackId, int parameter, float value)
{
    if ((parameter != 0 && parameter != 1) || !std::isfinite(value))
        return;
    const auto state = std::atomic_load_explicit(&preparedRenderState, std::memory_order_acquire);
    const auto project = state == nullptr ? std::shared_ptr<const ProjectData>{} : state->project;
    if (!project)
        return;
    for (size_t index = 0; index < static_cast<size_t>(project->tracks.size()) &&
                            index < maximumTrackCount; ++index)
    {
        if (project->tracks[static_cast<qsizetype>(index)].id == trackId)
        {
            if (parameter == 0)
                liveSynthFilterCutoffOverrides[index].store(
                    std::clamp(value, 20.0f, 20000.0f), std::memory_order_release);
            else
                liveSynthFilterResonanceOverrides[index].store(
                    std::clamp(value, 0.1f, 10.0f), std::memory_order_release);
            return;
        }
    }
}

void AudioEngine::clearLiveSynthFilterOverride(quint64 trackId, int parameter)
{
    if (parameter != 0 && parameter != 1)
        return;
    const auto state = std::atomic_load_explicit(&preparedRenderState, std::memory_order_acquire);
    const auto project = state == nullptr ? std::shared_ptr<const ProjectData>{} : state->project;
    if (!project)
        return;
    for (size_t index = 0; index < static_cast<size_t>(project->tracks.size()) &&
                            index < maximumTrackCount; ++index)
    {
        if (project->tracks[static_cast<qsizetype>(index)].id == trackId)
        {
            if (parameter == 0)
                liveSynthFilterCutoffOverrides[index].store(-1.0f, std::memory_order_release);
            else
                liveSynthFilterResonanceOverrides[index].store(-1.0f, std::memory_order_release);
            return;
        }
    }
}

void AudioEngine::setLivePluginParameterOverride(quint64 trackId, qsizetype effectIndex,
                                                 qsizetype parameterIndex, float value)
{
    if (trackId == 0 || effectIndex < -1 || effectIndex >= 8 ||
        parameterIndex < 0 || parameterIndex >= 256 || !std::isfinite(value))
        return;
    const bool sameTarget = livePluginTrackId.load(std::memory_order_acquire) == trackId &&
        livePluginEffectIndex.load(std::memory_order_relaxed) == effectIndex &&
        livePluginParameterIndex.load(std::memory_order_relaxed) == parameterIndex;
    if (!sameTarget)
    {
        livePluginParameterOverrideActive.store(false, std::memory_order_release);
        livePluginTrackId.store(trackId, std::memory_order_relaxed);
        livePluginEffectIndex.store(effectIndex, std::memory_order_relaxed);
        livePluginParameterIndex.store(parameterIndex, std::memory_order_relaxed);
    }
    livePluginParameterValue.store(value, std::memory_order_release);
    livePluginParameterOverrideActive.store(true, std::memory_order_release);
}

bool AudioEngine::setLiveVst3InstrumentParameterOverride(quint64 trackId,
                                                         const QString& pluginId,
                                                         qsizetype parameterIndex,
                                                         float value)
{
    if (trackId == 0 || pluginId.isEmpty() || parameterIndex < 0 ||
        parameterIndex >= 256 || !std::isfinite(value))
        return false;

    const size_t writeIndex = liveVst3ParameterEditWriteIndex.load(std::memory_order_relaxed);
    const size_t readIndex = liveVst3ParameterEditReadIndex.load(std::memory_order_acquire);
    if (writeIndex - readIndex >= liveVst3ParameterEditQueueCapacity)
        return false;

    liveVst3ParameterEdits[writeIndex % liveVst3ParameterEditQueueCapacity] =
        {trackId, pluginId, parameterIndex, value};
    liveVst3ParameterEditWriteIndex.store(writeIndex + 1, std::memory_order_release);
    return true;
}

void AudioEngine::clearLivePluginParameterOverride(quint64 trackId, qsizetype effectIndex,
                                                   qsizetype parameterIndex)
{
    if (livePluginTrackId.load(std::memory_order_acquire) == trackId &&
        livePluginEffectIndex.load(std::memory_order_relaxed) == effectIndex &&
        livePluginParameterIndex.load(std::memory_order_relaxed) == parameterIndex)
        livePluginParameterOverrideActive.store(false, std::memory_order_release);
}

void AudioEngine::capturePluginStates(ProjectData& project)
{
    const bool resumeSink = sink != nullptr && sink->state() == QAudio::ActiveState;
    if (resumeSink)
        sink->suspend();

    pluginStateCaptureInProgress.store(true, std::memory_order_release);
    struct StateCaptureExit
    {
        std::atomic<bool>& active;
        QAudioSink* sink = nullptr;
        bool resumeSink = false;

        void finish()
        {
            active.store(false, std::memory_order_release);
            if (resumeSink && sink != nullptr)
                sink->resume();
            resumeSink = false;
        }

        ~StateCaptureExit() { finish(); }
    } captureExit{pluginStateCaptureInProgress, sink.get(), resumeSink};
    while (activeRenderCallbacks.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();

    const auto renderState = std::atomic_load_explicit(&preparedRenderState,
                                                        std::memory_order_acquire);
    if (renderState != nullptr && renderState->renderer != nullptr)
        renderState->renderer->capturePluginStates(project);
    captureExit.finish();
}

void AudioEngine::servicePluginCallbacks()
{
    const auto renderState = std::atomic_load_explicit(&preparedRenderState,
                                                        std::memory_order_acquire);
    if (renderState == nullptr || renderState->renderer == nullptr)
        return;

    const std::shared_ptr<SynthRenderer>& renderer = renderState->renderer;
    renderer->servicePluginCallbacks();
    if (!renderer->hasPendingLv2LatencyChange() &&
        !renderer->hasPendingClapRestart() &&
        !renderer->hasPendingVst3LatencyRestart())
        return;

    if (pluginStateCaptureInProgress.exchange(true, std::memory_order_acq_rel))
        return;
    const bool resumeSink = sink != nullptr && sink->state() == QAudio::ActiveState;
    struct PluginRestartExit
    {
        std::atomic<bool>& active;
        QAudioSink* sink = nullptr;
        bool resumeSink = false;

        ~PluginRestartExit()
        {
            active.store(false, std::memory_order_release);
            if (resumeSink && sink != nullptr)
                sink->resume();
        }
    } restartExit{pluginStateCaptureInProgress, sink.get(), resumeSink};
    if (resumeSink)
        sink->suspend();
    while (activeRenderCallbacks.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
    renderer->serviceLv2LatencyChanges();
    renderer->serviceClapRestarts();
    renderer->serviceVst3LatencyRestarts();
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
    const auto state = std::atomic_load_explicit(&preparedRenderState, std::memory_order_acquire);
    const auto project = state == nullptr ? std::shared_ptr<const ProjectData>{} : state->project;
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
    event.pluginInstrument = track.instrumentType == InstrumentType::ClapInstrument ||
                             track.instrumentType == InstrumentType::Vst3Instrument;
    event.waveform = track.waveform;
    event.secondOscillatorWaveform = track.secondOscillatorWaveform;
    event.secondOscillatorDetuneCents = track.secondOscillatorDetuneCents;
    event.secondOscillatorMix = track.secondOscillatorMix;
    event.secondOscillatorEnabled = track.instrumentType == InstrumentType::Synth &&
                                    track.secondOscillatorEnabled;
    event.filterEnvelopeEnabled = track.instrumentType == InstrumentType::Synth &&
                                  track.synthFilterEnabled && track.synthFilterEnvelopeEnabled;
    event.filterEnvelopeAmountSemitones = std::clamp(
        std::isfinite(track.synthFilterEnvelopeAmountSemitones)
            ? track.synthFilterEnvelopeAmountSemitones : 24.0f, -48.0f, 48.0f);
    event.filterEnvelopeAttackSeconds = std::clamp(
        std::isfinite(track.synthFilterEnvelopeAttackSeconds)
            ? track.synthFilterEnvelopeAttackSeconds : 0.008f, 0.0f, 5.0f);
    event.filterEnvelopeDecaySeconds = std::clamp(
        std::isfinite(track.synthFilterEnvelopeDecaySeconds)
            ? track.synthFilterEnvelopeDecaySeconds : 0.2f, 0.0f, 5.0f);
    event.filterEnvelopeSustainLevel = std::clamp(
        std::isfinite(track.synthFilterEnvelopeSustainLevel)
            ? track.synthFilterEnvelopeSustainLevel : 0.0f, 0.0f, 1.0f);
    event.filterEnvelopeReleaseSeconds = std::clamp(
        std::isfinite(track.synthFilterEnvelopeReleaseSeconds)
            ? track.synthFilterEnvelopeReleaseSeconds : 0.2f, 0.0f, 5.0f);
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
        {
            const DrumPadSample& sample = track.drumSamples[static_cast<size_t>(pad)];
            event.attackSeconds = sample.attackSeconds;
            event.decaySeconds = sample.decaySeconds;
            event.sustainLevel = sample.sustainLevel;
            event.releaseSeconds = sample.releaseSeconds;
            const int layerIndex = sample.sourceLayerIndexForVelocity(velocity);
            const DrumLayerSettings settings = sample.layerSettings(layerIndex);
            event.sample = sample.sourceForVelocity(velocity);
            if (event.sample != nullptr)
            {
                event.attackSeconds = settings.attackSeconds;
                event.decaySeconds = settings.decaySeconds;
                event.sustainLevel = settings.sustainLevel;
                event.releaseSeconds = settings.releaseSeconds;
                event.sampleLoopEnabled = settings.loopEnabled;
            }
            if (event.sample != nullptr && event.sample->frameCount() > 0)
            {
                event.sampleStartFrame = static_cast<qint64>(std::llround(
                    static_cast<double>(event.sample->frameCount() - 1) *
                    std::clamp(static_cast<double>(settings.startPositionPercent), 0.0, 99.0) / 100.0));
                event.sampleEndFrame = static_cast<qint64>(std::llround(
                    static_cast<double>(event.sample->frameCount() - 1) *
                    std::clamp(static_cast<double>(settings.endPositionPercent),
                               static_cast<double>(settings.startPositionPercent) + 0.1,
                               100.0) / 100.0));
                event.sampleEndFrame = std::clamp<qint64>(event.sampleEndFrame,
                    event.sampleStartFrame, event.sample->frameCount() - 1);
                event.samplePlaybackRate = std::pow(2.0,
                    static_cast<double>(settings.tuningSemitones) / 12.0);
            }
        }
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

void AudioEngine::triggerLiveMidiPitchBend(int channel, int value,
                                          const InstrumentTrack* track)
{
    LiveMidiEvent event;
    event.pitchBend = true;
    event.channel = std::clamp(channel, 0, 15);
    event.pitchBendValue = std::clamp(value, 0, 16383);
    if (track != nullptr)
    {
        const auto state = std::atomic_load_explicit(&preparedRenderState, std::memory_order_acquire);
        const auto project = state == nullptr ? std::shared_ptr<const ProjectData>{} : state->project;
        if (project != nullptr)
        {
            for (size_t index = 0; index < static_cast<size_t>(project->tracks.size()) &&
                                    index < maximumTrackCount; ++index)
            {
                if (project->tracks[static_cast<qsizetype>(index)].id == track->id)
                {
                    event.trackIndex = index;
                    break;
                }
            }
        }
    }
    enqueueLiveMidiEvent(event);
}

void AudioEngine::triggerPluginInstrumentPreview(int midiPitch, int velocity,
                                               const InstrumentTrack& track)
{
    if (track.instrumentType != InstrumentType::ClapInstrument &&
        track.instrumentType != InstrumentType::Vst3Instrument)
        return;
    const auto state = std::atomic_load_explicit(&preparedRenderState, std::memory_order_acquire);
    const auto project = state == nullptr ? std::shared_ptr<const ProjectData>{} : state->project;
    if (project == nullptr)
        return;

    size_t trackIndex = maximumTrackCount;
    int instrumentChannel = 0;
    for (size_t index = 0; index < static_cast<size_t>(project->tracks.size()) &&
                            index < maximumTrackCount; ++index)
    {
        const InstrumentTrack& candidate = project->tracks[static_cast<qsizetype>(index)];
        if (candidate.instrumentType == InstrumentType::Synth ||
            candidate.instrumentType == InstrumentType::ClapInstrument ||
            candidate.instrumentType == InstrumentType::Vst3Instrument)
        {
            if (candidate.id == track.id)
            {
                trackIndex = index;
                break;
            }
            ++instrumentChannel;
        }
    }
    if (trackIndex >= maximumTrackCount)
        return;

    if (clapPreviewNoteOffPending.exchange(false, std::memory_order_acq_rel))
    {
        LiveMidiEvent previousOff;
        previousOff.channel = clapPreviewNoteOffChannel.load(std::memory_order_acquire);
        previousOff.pitch = clapPreviewNoteOffPitch.load(std::memory_order_acquire);
        previousOff.noteId = clapPreviewNoteOffId.load(std::memory_order_acquire);
        previousOff.preview = true;
        enqueueLiveMidiEvent(previousOff);
    }

    std::int32_t noteId = nextClapPreviewNoteId.fetch_add(1, std::memory_order_relaxed);
    if (noteId <= 0)
    {
        nextClapPreviewNoteId.store(2, std::memory_order_relaxed);
        noteId = 1;
    }
    LiveMidiEvent event;
    event.noteOn = true;
    const int channelSlot = instrumentChannel % 15;
    event.channel = channelSlot < 9 ? channelSlot : channelSlot + 1;
    event.pitch = std::clamp(midiPitch, 0, 127);
    event.velocity = std::clamp(velocity, 1, 127);
    event.noteId = noteId;
    event.trackIndex = trackIndex;
    event.pluginInstrument = true;
    event.preview = true;
    enqueueLiveMidiEvent(event);

    const qint64 previewFrames = std::max<qint64>(1,
        static_cast<qint64>(std::llround(outputSampleRate * 0.22)));
    clapPreviewNoteOffChannel.store(event.channel, std::memory_order_release);
    clapPreviewNoteOffPitch.store(event.pitch, std::memory_order_release);
    clapPreviewNoteOffId.store(noteId, std::memory_order_release);
    clapPreviewNoteOffFrame.store(
        audioClockFrames.load(std::memory_order_acquire) + previewFrames,
        std::memory_order_release);
    clapPreviewNoteOffPending.store(true, std::memory_order_release);
    previewPitch.store(-1, std::memory_order_release);
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
    liveMidiChannelPitchBends.fill(0.0f);
    liveMidiChannelPitchBendValues.fill(8192);
}

void AudioEngine::applyLiveMidiEvent(const LiveMidiEvent& event, qint64 frame,
                                     SynthRenderer& renderer)
{
    if (event.pitchBend)
    {
        const float bendSemitones = (static_cast<float>(event.pitchBendValue) - 8192.0f) /
                                    8192.0f * liveMidiPitchBendRangeSemitones.load(
                                        std::memory_order_acquire);
        liveMidiChannelPitchBends[static_cast<size_t>(event.channel)] = bendSemitones;
        liveMidiChannelPitchBendValues[static_cast<size_t>(event.channel)] = event.pitchBendValue;
        const double bendRatio = std::pow(2.0, static_cast<double>(bendSemitones) / 12.0);
        if (event.trackIndex < maximumTrackCount)
            renderer.pluginInstrumentPitchBend(event.trackIndex, event.channel,
                                             event.pitchBendValue);
        for (LiveMidiVoice& voice : liveMidiVoices)
        {
            if (event.trackIndex >= maximumTrackCount && voice.active &&
                voice.pluginInstrument && voice.channel == event.channel)
                renderer.pluginInstrumentPitchBend(voice.trackIndex, event.channel,
                                                 event.pitchBendValue);
            else if (voice.active && !voice.percussion && voice.channel == event.channel)
                voice.frequency = voice.baseFrequency * bendRatio;
        }
        return;
    }

    if (!event.noteOn)
    {
        LiveMidiVoice* matchedVoice = nullptr;
        for (LiveMidiVoice& voice : liveMidiVoices)
        {
            if (!voice.active || !voice.held || voice.channel != event.channel ||
                voice.pitch != event.pitch || voice.preview != event.preview ||
                (event.noteId > 0 && voice.pluginNoteId != event.noteId))
                continue;
            if (matchedVoice == nullptr || voice.serial > matchedVoice->serial)
                matchedVoice = &voice;
        }
        if (matchedVoice == nullptr)
            return;
        if (matchedVoice->pluginInstrument)
        {
            renderer.pluginInstrumentNoteOff(matchedVoice->trackIndex, matchedVoice->pluginNoteId,
                                           matchedVoice->channel, matchedVoice->pitch);
            *matchedVoice = LiveMidiVoice{};
            return;
        }
        if (matchedVoice->percussion)
        {
            if (matchedVoice->sample == nullptr || !matchedVoice->sampleLoopEnabled)
                return;
            matchedVoice->held = false;
            matchedVoice->releaseFrame = frame;
            const qint64 age = std::max<qint64>(0, frame - matchedVoice->startFrame);
            matchedVoice->releaseStartLevel = envelopeLevel(age, matchedVoice->attackFrames,
                matchedVoice->decayFrames, matchedVoice->sustainLevel);
            return;
        }

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
        matchedVoice->filterEnvelopeReleaseStartLevel = envelopeLevel(age,
            matchedVoice->filterEnvelopeAttackFrames,
            matchedVoice->filterEnvelopeDecayFrames,
            matchedVoice->filterEnvelopeSustainLevel);
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

    if (target->active && target->pluginInstrument)
        renderer.pluginInstrumentNoteOff(target->trackIndex, target->pluginNoteId,
                                       target->channel, target->pitch);

    const double panAngle = (static_cast<double>(event.pan) + 1.0) * 0.7853981633974483;
    *target = LiveMidiVoice{};
    target->active = true;
    target->held = true;
    target->percussion = event.percussion;
    target->pluginInstrument = event.pluginInstrument;
    target->preview = event.preview;
    target->channel = event.channel;
    target->pitch = event.pitch;
    target->trackIndex = event.trackIndex;
    target->startFrame = frame;
    target->attackFrames = std::max<qint64>(1, static_cast<qint64>(std::llround(
        event.attackSeconds * outputSampleRate)));
    target->decayFrames = std::max<qint64>(0, static_cast<qint64>(std::llround(
        event.decaySeconds * outputSampleRate)));
    target->releaseFrames = std::max<qint64>(0, static_cast<qint64>(std::llround(
        event.releaseSeconds * outputSampleRate)));
    target->filterEnvelopeEnabled = event.filterEnvelopeEnabled;
    target->filterEnvelopeAmountSemitones = event.filterEnvelopeAmountSemitones;
    target->filterEnvelopeAttackFrames = std::max<qint64>(0,
        static_cast<qint64>(std::llround(event.filterEnvelopeAttackSeconds * outputSampleRate)));
    target->filterEnvelopeDecayFrames = std::max<qint64>(0,
        static_cast<qint64>(std::llround(event.filterEnvelopeDecaySeconds * outputSampleRate)));
    target->filterEnvelopeSustainLevel = std::clamp(event.filterEnvelopeSustainLevel, 0.0f, 1.0f);
    target->filterEnvelopeReleaseFrames = std::max<qint64>(0,
        static_cast<qint64>(std::llround(event.filterEnvelopeReleaseSeconds * outputSampleRate)));
    target->serial = nextLiveMidiSerial++;
    if (target->pluginInstrument)
    {
        target->pluginNoteId = event.noteId > 0 ? event.noteId :
            static_cast<std::int32_t>(target->serial & 0x7FFFFFFFULL);
        if (target->pluginNoteId == 0)
            target->pluginNoteId = 1;
        renderer.pluginInstrumentNoteOn(target->trackIndex, target->pluginNoteId,
            target->channel, target->pitch, static_cast<float>(event.velocity) / 127.0f);
    }
    target->baseFrequency = 440.0 * std::pow(2.0, (event.pitch - 69) / 12.0);
    target->frequency = target->baseFrequency * std::pow(2.0,
        static_cast<double>(liveMidiChannelPitchBends[static_cast<size_t>(event.channel)]) / 12.0);
    target->amplitude = (event.percussion ? 0.48f : 0.22f) *
                        static_cast<float>(event.velocity) / 127.0f * event.gain;
    target->sustainLevel = event.sustainLevel;
    target->leftGain = static_cast<float>(std::cos(panAngle));
    target->rightGain = static_cast<float>(std::sin(panAngle));
    target->waveform = event.waveform;
    target->secondOscillatorWaveform = event.secondOscillatorWaveform;
    target->secondOscillatorDetuneCents = event.secondOscillatorDetuneCents;
    target->secondOscillatorMix = event.secondOscillatorMix;
    target->secondOscillatorEnabled = event.secondOscillatorEnabled && !event.percussion;
    target->sample = event.sample;
    target->sampleStartFrame = event.sampleStartFrame;
    target->sampleEndFrame = event.sampleEndFrame;
    target->samplePlaybackRate = event.samplePlaybackRate;
    target->sampleLoopEnabled = event.sampleLoopEnabled;
    if (target->sample != nullptr && !target->sampleLoopEnabled &&
        target->sample->sampleRate > 0 && target->samplePlaybackRate > 0.0)
    {
        const double sourceDuration = static_cast<double>(target->sampleEndFrame -
            target->sampleStartFrame + 1) * outputSampleRate /
            (target->sample->sampleRate * target->samplePlaybackRate);
        target->sampleDurationFrames = std::max<qint64>(1,
            static_cast<qint64>(std::ceil(sourceDuration)));
    }
}

void AudioEngine::renderLiveMidiVoices(qint64 frame, SynthRenderer& renderer)
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
        if (voice.pluginInstrument)
            continue;

        const qint64 age = frame - voice.startFrame;
        if (age < 0)
            continue;

        if (voice.sample != nullptr)
        {
            double sourcePosition = voice.sampleStartFrame + static_cast<double>(age) *
                voice.sample->sampleRate * voice.samplePlaybackRate / outputSampleRate;
            const qint64 loopStartFrame = std::clamp<qint64>(voice.sampleStartFrame, 0,
                                                              voice.sample->frameCount() - 1);
            const qint64 sampleEndFrame = std::clamp<qint64>(voice.sampleEndFrame,
                loopStartFrame, voice.sample->frameCount() - 1);
            if (voice.sampleLoopEnabled)
            {
                const double loopLengthFrames = std::max<qint64>(1,
                    sampleEndFrame - loopStartFrame + 1);
                sourcePosition = loopStartFrame + std::fmod(
                    std::max(0.0, sourcePosition - loopStartFrame), loopLengthFrames);
            }
            const qint64 firstFrame = static_cast<qint64>(sourcePosition);
            if (firstFrame > sampleEndFrame && !voice.sampleLoopEnabled)
            {
                voice = LiveMidiVoice{};
                continue;
            }
            if (firstFrame < 0 || voice.sample->channelCount < 1 || voice.sample->channelCount > 2)
                continue;
            const qint64 secondFrame = firstFrame + 1 <= sampleEndFrame
                ? firstFrame + 1
                : voice.sampleLoopEnabled ? loopStartFrame : sampleEndFrame;
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
            float envelope = envelopeLevel(age, voice.attackFrames, voice.decayFrames,
                                           voice.sustainLevel);
            if (voice.sampleLoopEnabled && voice.releaseFrame >= 0 && frame >= voice.releaseFrame)
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
            else if (!voice.sampleLoopEnabled && voice.releaseFrames > 0)
            {
                const qint64 releaseStartAge = std::max<qint64>(0,
                    voice.sampleDurationFrames - voice.releaseFrames);
                if (age >= releaseStartAge)
                {
                    const qint64 releaseAge = age - releaseStartAge;
                    const float releaseStartLevel = envelopeLevel(releaseStartAge,
                        voice.attackFrames, voice.decayFrames, voice.sustainLevel);
                    envelope = releaseStartLevel * std::max(0.0f, 1.0f -
                        static_cast<float>(releaseAge) / static_cast<float>(voice.releaseFrames));
                }
            }
            const float leftContribution = left * voice.amplitude * voice.leftGain * envelope;
            const float rightContribution = right * voice.amplitude * voice.rightGain * envelope;
            addTrackFrameSample(voice.trackIndex, leftContribution, rightContribution);
            continue;
        }

        float envelope = 1.0f;
        if (voice.percussion)
        {
            const double duration = voice.pitch == 46 ? 0.72 :
                (voice.pitch == 42 || voice.pitch == 44 ? 0.22 : 0.48);
            const qint64 durationFrames = static_cast<qint64>(outputSampleRate * duration);
            if (age >= durationFrames + voice.releaseFrames)
            {
                voice = LiveMidiVoice{};
                continue;
            }
            if (age < durationFrames)
                envelope = envelopeLevel(age, voice.attackFrames, voice.decayFrames,
                                         voice.sustainLevel);
            else
            {
                const qint64 releaseAge = age - durationFrames;
                const float releaseStartLevel = envelopeLevel(durationFrames, voice.attackFrames,
                    voice.decayFrames, voice.sustainLevel);
                envelope = voice.releaseFrames > 0 ? releaseStartLevel * std::max(0.0f, 1.0f -
                    static_cast<float>(releaseAge) / static_cast<float>(voice.releaseFrames)) : 0.0f;
            }
        }
        else if (voice.releaseFrame >= 0 && frame >= voice.releaseFrame)
        {
            const qint64 releaseAge = frame - voice.releaseFrame;
            const qint64 activeReleaseFrames = voice.filterEnvelopeEnabled
                ? std::max(voice.releaseFrames, voice.filterEnvelopeReleaseFrames)
                : voice.releaseFrames;
            if (releaseAge >= activeReleaseFrames)
            {
                voice = LiveMidiVoice{};
                continue;
            }
            envelope = voice.releaseFrames > 0
                ? voice.releaseStartLevel * std::max(0.0f, 1.0f -
                    static_cast<float>(releaseAge) / static_cast<float>(voice.releaseFrames))
                : 0.0f;
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

        if (voice.filterEnvelopeEnabled)
        {
            const float filterEnvelope = voice.releaseFrame >= 0 && frame >= voice.releaseFrame
                ? voice.filterEnvelopeReleaseFrames > 0
                    ? voice.filterEnvelopeReleaseStartLevel * std::max(0.0f, 1.0f -
                        static_cast<float>(frame - voice.releaseFrame) /
                        static_cast<float>(voice.filterEnvelopeReleaseFrames))
                    : 0.0f
                : envelopeLevel(age, voice.filterEnvelopeAttackFrames,
                                voice.filterEnvelopeDecayFrames,
                                voice.filterEnvelopeSustainLevel);
            renderer.setSynthFilterEnvelope(voice.trackIndex,
                voice.filterEnvelopeAmountSemitones * filterEnvelope);
        }

        float voiceSample = 0.0f;
        if (voice.percussion)
        {
            voiceSample = renderPercussionSample(voice.pitch, age, voice.startFrame, outputSampleRate) *
                          voice.amplitude * envelope;
        }
        else
        {
            float oscillator = oscillatorSample(
                voice.waveform, static_cast<double>(age) * voice.frequency / outputSampleRate);
            if (voice.secondOscillatorEnabled)
            {
                const double detuneRatio = std::pow(2.0,
                    static_cast<double>(voice.secondOscillatorDetuneCents) / 1200.0);
                const double secondPhase = static_cast<double>(age) * voice.frequency * detuneRatio /
                                           outputSampleRate;
                const float secondOscillator = oscillatorSample(voice.secondOscillatorWaveform, secondPhase);
                const float mix = std::clamp(voice.secondOscillatorMix, 0.0f, 1.0f);
                oscillator = oscillator * (1.0f - mix) + secondOscillator * mix;
            }
            voiceSample = oscillator * voice.amplitude * envelope;
        }
        const float leftContribution = voiceSample * voice.leftGain;
        const float rightContribution = voiceSample * voice.rightGain;
        addTrackFrameSample(voice.trackIndex, leftContribution, rightContribution);
    }

    for (size_t index = 0; index < liveMidiTouchedTrackCount; ++index)
    {
        const size_t trackIndex = liveMidiTouchedTracks[index];
        const StereoSample& trackSample = liveMidiTrackFrameSamples[trackIndex];
        renderer.addLiveTrackSample(trackIndex, trackSample);
        liveMidiBlockPeaks[trackIndex] = std::max(liveMidiBlockPeaks[trackIndex],
            std::max(std::abs(trackSample.left), std::abs(trackSample.right)));
        liveMidiTrackFrameSamples[trackIndex] = {};
        liveMidiTrackFrameTouched[trackIndex] = false;
    }
}

void AudioEngine::triggerPreview(int midiPitch, int velocity, SynthWaveform waveform, float pan,
                                 bool percussion, bool secondOscillatorEnabled,
                                 SynthWaveform secondOscillatorWaveform,
                                 float secondOscillatorDetuneCents, float secondOscillatorMix,
                                 bool synthFilterEnabled, float synthFilterCutoffHz,
                                 float synthFilterResonance, SynthFilterType synthFilterType,
                                 bool synthFilterEnvelopeEnabled,
                                 float synthFilterEnvelopeAmountSemitones,
                                 float synthFilterEnvelopeAttackSeconds,
                                 float synthFilterEnvelopeDecaySeconds,
                                 float synthFilterEnvelopeSustainLevel,
                                 float synthFilterEnvelopeReleaseSeconds, float attackSeconds,
                                 float decaySeconds, float sustainLevel, float releaseSeconds)
{
    std::atomic_store_explicit(&previewSampleAsset, std::shared_ptr<const AudioAsset>{},
                               std::memory_order_release);
    const int clampedPitch = std::clamp(midiPitch, 0, 127);
    const double panAngle = (std::clamp(static_cast<double>(pan), -1.0, 1.0) + 1.0) * 0.7853981633974483;
    previewVelocity.store(std::clamp(velocity, 1, 127), std::memory_order_relaxed);
    previewFrequency.store(440.0 * std::pow(2.0, (clampedPitch - 69) / 12.0), std::memory_order_relaxed);
    previewWaveform.store(static_cast<int>(waveform), std::memory_order_relaxed);
    previewSecondOscillatorEnabled.store(secondOscillatorEnabled, std::memory_order_relaxed);
    previewSecondOscillatorWaveform.store(static_cast<int>(secondOscillatorWaveform), std::memory_order_relaxed);
    previewSecondOscillatorDetuneCents.store(std::clamp(secondOscillatorDetuneCents, -100.0f, 100.0f),
                                             std::memory_order_relaxed);
    previewSecondOscillatorMix.store(std::clamp(secondOscillatorMix, 0.0f, 1.0f),
                                     std::memory_order_relaxed);
    const bool filterEnabled = synthFilterEnabled && !percussion;
    const double safeCutoff = std::isfinite(synthFilterCutoffHz) ? synthFilterCutoffHz : 20000.0f;
    const double cutoff = std::clamp(safeCutoff, 20.0, std::max(20.0, outputSampleRate * 0.49));
    const double safeResonance = std::isfinite(synthFilterResonance) ? synthFilterResonance : 0.707f;
    const double resonance = std::clamp(safeResonance, 0.1, 10.0);
    const BiquadCoefficients coefficients = synthFilterCoefficients(
        synthFilterType, cutoff, resonance, outputSampleRate);
    previewFilterB0.store(coefficients.b0, std::memory_order_relaxed);
    previewFilterB1.store(coefficients.b1, std::memory_order_relaxed);
    previewFilterB2.store(coefficients.b2, std::memory_order_relaxed);
    previewFilterA1.store(coefficients.a1, std::memory_order_relaxed);
    previewFilterA2.store(coefficients.a2, std::memory_order_relaxed);
    previewSynthFilterEnabled.store(filterEnabled, std::memory_order_relaxed);
    previewFilterType.store(static_cast<int>(synthFilterType), std::memory_order_relaxed);
    previewFilterCutoffHz.store(static_cast<float>(cutoff), std::memory_order_relaxed);
    previewFilterResonance.store(static_cast<float>(resonance), std::memory_order_relaxed);
    const bool filterEnvelopeEnabled = filterEnabled && synthFilterEnvelopeEnabled;
    previewFilterEnvelopeEnabled.store(filterEnvelopeEnabled, std::memory_order_relaxed);
    previewFilterEnvelopeAmountSemitones.store(std::clamp(
        std::isfinite(synthFilterEnvelopeAmountSemitones)
            ? synthFilterEnvelopeAmountSemitones : 24.0f, -48.0f, 48.0f),
        std::memory_order_relaxed);
    previewFilterEnvelopeAttackFrames.store(std::max<qint64>(0,
        static_cast<qint64>(std::llround(std::clamp(
            std::isfinite(synthFilterEnvelopeAttackSeconds)
                ? synthFilterEnvelopeAttackSeconds : 0.008f, 0.0f, 5.0f) * outputSampleRate))),
        std::memory_order_relaxed);
    previewFilterEnvelopeDecayFrames.store(std::max<qint64>(0,
        static_cast<qint64>(std::llround(std::clamp(
            std::isfinite(synthFilterEnvelopeDecaySeconds)
                ? synthFilterEnvelopeDecaySeconds : 0.2f, 0.0f, 5.0f) * outputSampleRate))),
        std::memory_order_relaxed);
    previewFilterEnvelopeSustainLevel.store(std::clamp(
        std::isfinite(synthFilterEnvelopeSustainLevel)
            ? synthFilterEnvelopeSustainLevel : 0.0f, 0.0f, 1.0f),
        std::memory_order_relaxed);
    previewFilterEnvelopeReleaseFrames.store(std::max<qint64>(0,
        static_cast<qint64>(std::llround(std::clamp(
            std::isfinite(synthFilterEnvelopeReleaseSeconds)
                ? synthFilterEnvelopeReleaseSeconds : 0.2f, 0.0f, 5.0f) * outputSampleRate))),
        std::memory_order_relaxed);
    previewFilterResetSerial.fetch_add(1, std::memory_order_release);
    previewPercussion.store(percussion, std::memory_order_relaxed);
    previewAttackFrames.store(std::max<qint64>(0, static_cast<qint64>(std::llround(
        std::clamp(attackSeconds, 0.0f, 5.0f) * outputSampleRate))), std::memory_order_relaxed);
    previewDecayFrames.store(std::max<qint64>(0, static_cast<qint64>(std::llround(
        std::clamp(decaySeconds, 0.0f, 5.0f) * outputSampleRate))), std::memory_order_relaxed);
    previewSustainLevel.store(std::clamp(sustainLevel, 0.0f, 1.0f), std::memory_order_relaxed);
    previewReleaseFrames.store(std::max<qint64>(0, static_cast<qint64>(std::llround(
        std::clamp(releaseSeconds, 0.0f, 5.0f) * outputSampleRate))), std::memory_order_relaxed);
    previewLeftGain.store(static_cast<float>(std::cos(panAngle)), std::memory_order_relaxed);
    previewRightGain.store(static_cast<float>(std::sin(panAngle)), std::memory_order_relaxed);
    previewPitch.store(clampedPitch, std::memory_order_release);
    const qint64 start = audioClockFrames.load(std::memory_order_acquire);
    previewStartFrame.store(start, std::memory_order_release);
    const double previewSeconds = percussion && clampedPitch == 46 ? 0.72 :
                                  percussion && (clampedPitch == 42 || clampedPitch == 44) ? 0.22 : 0.48;
    const qint64 noteEnd = start + static_cast<qint64>(outputSampleRate * previewSeconds);
    previewNoteEndFrame.store(noteEnd, std::memory_order_release);
    const double filterTailSeconds = filterEnabled
        ? std::min(4.0, 7.0 * resonance / (3.14159265358979323846 * cutoff)) : 0.0;
    const qint64 percussionTailFrames = percussion ? previewReleaseFrames.load(std::memory_order_relaxed) : 0;
    const qint64 amplitudeReleaseFrames = percussion ? percussionTailFrames :
        previewReleaseFrames.load(std::memory_order_relaxed);
    const qint64 filterReleaseFrames = filterEnvelopeEnabled
        ? previewFilterEnvelopeReleaseFrames.load(std::memory_order_relaxed) : 0;
    const qint64 synthReleaseFrames = std::max(amplitudeReleaseFrames, filterReleaseFrames);
    previewEndFrame.store(noteEnd + synthReleaseFrames +
                          static_cast<qint64>(outputSampleRate * filterTailSeconds),
                          std::memory_order_release);
}

void AudioEngine::triggerSamplePreview(std::shared_ptr<const AudioAsset> sample, int velocity,
                                       float pan, float gain, float startPositionPercent,
                                       float endPositionPercent, float tuningSemitones,
                                       bool loopEnabled,
                                       float attackSeconds, float decaySeconds,
                                       float sustainLevel, float releaseSeconds)
{
    if (sample == nullptr || sample->sampleRate <= 0 || sample->channelCount < 1 ||
        sample->channelCount > 2 || sample->frameCount() <= 0)
        return;

    const double panAngle = (std::clamp(static_cast<double>(pan), -1.0, 1.0) + 1.0) *
                            0.7853981633974483;
    previewVelocity.store(std::clamp(velocity, 1, 127), std::memory_order_relaxed);
    previewSynthFilterEnabled.store(false, std::memory_order_relaxed);
    previewFilterResetSerial.fetch_add(1, std::memory_order_release);
    previewSampleGain.store(std::clamp(gain, 0.0f, 1.0f), std::memory_order_relaxed);
    const qint64 lastFrame = sample->frameCount() - 1;
    const double startPercent = std::clamp(static_cast<double>(startPositionPercent), 0.0, 99.0);
    const double endPercent = std::clamp(static_cast<double>(endPositionPercent),
                                          startPercent + 0.1, 100.0);
    const qint64 startFrame = static_cast<qint64>(std::llround(lastFrame * startPercent / 100.0));
    const qint64 endFrame = std::clamp<qint64>(
        static_cast<qint64>(std::llround(lastFrame * endPercent / 100.0)),
        startFrame, lastFrame);
    const double playbackRate = std::pow(2.0,
        std::clamp(static_cast<double>(tuningSemitones), -24.0, 24.0) / 12.0);
    previewSampleStartFrame.store(static_cast<double>(startFrame), std::memory_order_relaxed);
    previewSampleEndFrame.store(endFrame, std::memory_order_relaxed);
    previewSamplePlaybackRate.store(playbackRate, std::memory_order_relaxed);
    previewSampleLoopEnabled.store(loopEnabled, std::memory_order_relaxed);
    previewAttackFrames.store(std::max<qint64>(0, static_cast<qint64>(std::llround(
        std::clamp(attackSeconds, 0.0f, 5.0f) * outputSampleRate))), std::memory_order_relaxed);
    previewDecayFrames.store(std::max<qint64>(0, static_cast<qint64>(std::llround(
        std::clamp(decaySeconds, 0.0f, 5.0f) * outputSampleRate))), std::memory_order_relaxed);
    previewSustainLevel.store(std::clamp(sustainLevel, 0.0f, 1.0f), std::memory_order_relaxed);
    previewReleaseFrames.store(std::max<qint64>(0, static_cast<qint64>(std::llround(
        std::clamp(releaseSeconds, 0.0f, 5.0f) * outputSampleRate))), std::memory_order_relaxed);
    previewLeftGain.store(static_cast<float>(std::cos(panAngle)), std::memory_order_relaxed);
    previewRightGain.store(static_cast<float>(std::sin(panAngle)), std::memory_order_relaxed);
    const qint64 start = audioClockFrames.load(std::memory_order_acquire);
    const qint64 duration = loopEnabled
        ? static_cast<qint64>(outputSampleRate)
        : static_cast<qint64>(std::ceil(
            (endFrame - startFrame + 1) * outputSampleRate /
            (sample->sampleRate * playbackRate)));
    previewStartFrame.store(start, std::memory_order_release);
    const qint64 end = start + std::max<qint64>(1, duration);
    previewNoteEndFrame.store(end, std::memory_order_release);
    previewEndFrame.store(end + (loopEnabled
        ? previewReleaseFrames.load(std::memory_order_relaxed) : 0), std::memory_order_release);
    std::atomic_store_explicit(&previewSampleAsset, std::move(sample), std::memory_order_release);
    previewPitch.store(0, std::memory_order_release);
}

void AudioEngine::setMasterGain(float gain)
{
    masterGain.store(std::clamp(gain, 0.0f, 1.0f), std::memory_order_release);
}

bool AudioEngine::setAudioInputMonitoring(bool enabled)
{
    if (enabled && sink == nullptr)
        return false;
    audioMonitorGeneration.fetch_add(1, std::memory_order_acq_rel);
    audioMonitorHasPreviousSample = false;
    audioMonitorSourceFrameIndex = 0;
    audioMonitorNextSourceFrame = 0.0;
    audioInputMonitoring.store(enabled, std::memory_order_release);
    return true;
}

void AudioEngine::setAudioInputMonitorTrack(quint64 trackId)
{
    if (audioMonitorTrackId.exchange(trackId, std::memory_order_acq_rel) != trackId)
        audioMonitorGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void AudioEngine::pushAudioInputMonitorFrames(const float* interleavedSamples, qsizetype frameCount,
                                             int channelCount, int sampleRate)
{
    if (!audioInputMonitoring.load(std::memory_order_acquire) || interleavedSamples == nullptr ||
        frameCount <= 0 || channelCount < 1 || channelCount > 2 || sampleRate < 8000 ||
        sampleRate > 192000 || outputSampleRate <= 0.0)
        return;

    const double sourceFramesPerOutputFrame = sampleRate / outputSampleRate;
    const quint64 generation = audioMonitorGeneration.load(std::memory_order_acquire);
    const auto enqueue = [this, generation](StereoSample sample)
    {
        const quint64 writeIndex = audioMonitorWriteIndex.load(std::memory_order_relaxed);
        const quint64 readIndex = audioMonitorReadIndex.load(std::memory_order_acquire);
        if (writeIndex - readIndex >= audioMonitorBufferCapacity)
            return;
        audioMonitorBuffer[writeIndex % audioMonitorBufferCapacity] = {sample, generation};
        audioMonitorWriteIndex.store(writeIndex + 1, std::memory_order_release);
    };

    for (qsizetype frame = 0; frame < frameCount; ++frame)
    {
        const float rawLeft = interleavedSamples[frame * channelCount];
        const float left = std::isfinite(rawLeft) ? std::clamp(rawLeft, -1.0f, 1.0f) : 0.0f;
        const float rawRight = channelCount == 1 ? rawLeft :
            interleavedSamples[frame * channelCount + 1];
        const float right = channelCount == 1 ? left :
            std::isfinite(rawRight) ? std::clamp(rawRight, -1.0f, 1.0f) : 0.0f;
        const StereoSample current{left, right};
        if (!audioMonitorHasPreviousSample)
        {
            audioMonitorPreviousSample = current;
            audioMonitorHasPreviousSample = true;
            audioMonitorSourceFrameIndex = 0;
            enqueue(current);
            audioMonitorNextSourceFrame = sourceFramesPerOutputFrame;
            continue;
        }

        ++audioMonitorSourceFrameIndex;
        while (audioMonitorNextSourceFrame <= static_cast<double>(audioMonitorSourceFrameIndex))
        {
            const double amount = std::clamp(audioMonitorNextSourceFrame -
                static_cast<double>(audioMonitorSourceFrameIndex - 1), 0.0, 1.0);
            enqueue({audioMonitorPreviousSample.left +
                         (current.left - audioMonitorPreviousSample.left) * static_cast<float>(amount),
                     audioMonitorPreviousSample.right +
                         (current.right - audioMonitorPreviousSample.right) * static_cast<float>(amount)});
            audioMonitorNextSourceFrame += sourceFramesPerOutputFrame;
        }
        audioMonitorPreviousSample = current;
    }
}

qint64 AudioEngine::renderAudio(char* destination, qint64 maxBytes)
{
    activeRenderCallbacks.fetch_add(1, std::memory_order_acq_rel);
    struct RenderCallbackExit
    {
        std::atomic<size_t>& count;
        ~RenderCallbackExit() { count.fetch_sub(1, std::memory_order_release); }
    } callbackExit{activeRenderCallbacks};

    if (pluginStateCaptureInProgress.load(std::memory_order_acquire))
    {
        if (destination != nullptr && maxBytes > 0)
        {
            if (outputFormat != nullptr &&
                outputFormat->sampleFormat() == QAudioFormat::UInt8)
                std::memset(destination, 0x80, static_cast<size_t>(maxBytes));
            else
                std::memset(destination, 0, static_cast<size_t>(maxBytes));

            if (bytesPerFrame > 0)
            {
                const qint64 frames = maxBytes / bytesPerFrame;
                const auto renderState = std::atomic_load_explicit(
                    &preparedRenderState, std::memory_order_acquire);
                if (frames > 0 && renderState != nullptr && renderState->project != nullptr)
                {
                    const qint64 clockStart = audioClockFrames.load(std::memory_order_relaxed);
                    publishTransportOutputSegment(
                        renderState->generation, clockStart,
                        publishedPositionFrames.load(std::memory_order_acquire),
                        frames, 0, 1, false, false);
                    audioClockFrames.store(clockStart + frames, std::memory_order_release);
                }
            }
        }
        return destination == nullptr ? 0 : maxBytes;
    }
    if (destination == nullptr || !outputFormat || bytesPerFrame <= 0)
        return 0;

    const int frames = static_cast<int>(maxBytes / bytesPerFrame);
    if (frames <= 0)
        return 0;

    // The UI publishes immutable snapshots; this callback takes one snapshot per block.
    const auto renderState = std::atomic_load_explicit(&preparedRenderState, std::memory_order_acquire);
    if (renderState == nullptr || renderState->project == nullptr || renderState->renderer == nullptr)
        return 0;
    const auto& project = renderState->project;
    SynthRenderer& renderer = *renderState->renderer;
    size_t audioMonitorTrackIndex = maximumTrackCount;
    const quint64 monitorTrackId = audioMonitorTrackId.load(std::memory_order_acquire);
    if (monitorTrackId != 0)
    {
        for (size_t trackIndex = 0; trackIndex < static_cast<size_t>(project->tracks.size()); ++trackIndex)
        {
            const InstrumentTrack& track = project->tracks[static_cast<qsizetype>(trackIndex)];
            if (track.id == monitorTrackId && track.instrumentType == InstrumentType::Audio)
            {
                audioMonitorTrackIndex = trackIndex;
                break;
            }
        }
    }
    struct PluginProcessingExit
    {
        SynthRenderer& renderer;
        ~PluginProcessingExit() { renderer.stopPluginProcessing(); }
    } pluginProcessingExit{renderer};
    renderer.resetTrackPeakLevels();
    renderer.resetBusPeakLevels();
    liveMidiBlockPeaks.fill(0.0f);
    for (size_t index = 0; index < static_cast<size_t>(project->tracks.size()) &&
                            index < maximumTrackCount; ++index)
    {
        renderer.setLiveTrackMixOverride(index, 0,
            liveTrackVolumes[index].load(std::memory_order_acquire));
        renderer.setLiveTrackMixOverride(index, 1,
            liveTrackPans[index].load(std::memory_order_acquire));
        for (size_t band = 0; band < liveTrackEqGainDb[index].size(); ++band)
            renderer.setLiveTrackMixOverride(index,
                trackEqLowGainAutomationTarget + static_cast<int>(band),
                liveTrackEqGainDb[index][band].load(std::memory_order_acquire));
        renderer.setLiveTrackMixOverride(index, trackEqLowCrossoverAutomationTarget,
            liveTrackEqCrossoverHz[index][0].load(std::memory_order_acquire));
        renderer.setLiveTrackMixOverride(index, trackEqHighCrossoverAutomationTarget,
            liveTrackEqCrossoverHz[index][1].load(std::memory_order_acquire));
        for (size_t band = 0; band < liveTrackParametricEqValues[index].size(); ++band)
            for (size_t parameter = 0;
                 parameter < liveTrackParametricEqValues[index][band].size(); ++parameter)
                renderer.setLiveTrackMixOverride(index,
                    trackEqParametricAutomationTarget(static_cast<int>(band),
                                                      static_cast<int>(parameter)),
                    liveTrackParametricEqValues[index][band][parameter].load(
                        std::memory_order_acquire));
        renderer.setLiveSynthFilterOverride(index, 0,
            liveSynthFilterCutoffOverrides[index].load(std::memory_order_acquire));
        renderer.setLiveSynthFilterOverride(index, 1,
            liveSynthFilterResonanceOverrides[index].load(std::memory_order_acquire));
    }
    size_t parameterEditRead =
        liveVst3ParameterEditReadIndex.load(std::memory_order_relaxed);
    const size_t parameterEditWrite =
        liveVst3ParameterEditWriteIndex.load(std::memory_order_acquire);
    while (parameterEditRead < parameterEditWrite)
    {
        const LiveVst3ParameterEdit& edit = liveVst3ParameterEdits[
            parameterEditRead % liveVst3ParameterEditQueueCapacity];
        for (size_t trackIndex = 0;
             trackIndex < static_cast<size_t>(project->tracks.size()) &&
                 trackIndex < maximumTrackCount; ++trackIndex)
        {
            const InstrumentTrack& track = project->tracks[static_cast<qsizetype>(trackIndex)];
            if (track.id == edit.trackId &&
                track.instrumentType == InstrumentType::Vst3Instrument &&
                track.vst3PluginId == edit.pluginId)
            {
                renderer.setLiveVst3InstrumentParameterOverride(
                    trackIndex, edit.parameterIndex, edit.value);
                break;
            }
        }
        ++parameterEditRead;
    }
    liveVst3ParameterEditReadIndex.store(parameterEditRead, std::memory_order_release);

    const quint64 livePluginTrackId = this->livePluginTrackId.load(std::memory_order_acquire);
    const qsizetype livePluginEffectIndex = this->livePluginEffectIndex.load(std::memory_order_relaxed);
    const qsizetype livePluginParameterIndex = this->livePluginParameterIndex.load(std::memory_order_relaxed);
    if (livePluginTrackId != 0 && livePluginEffectIndex >= -1 && livePluginParameterIndex >= 0)
    {
        for (size_t trackIndex = 0;
             trackIndex < static_cast<size_t>(project->tracks.size()) &&
                 trackIndex < maximumTrackCount; ++trackIndex)
        {
            if (project->tracks[static_cast<qsizetype>(trackIndex)].id != livePluginTrackId)
                continue;
            const float liveValue = livePluginParameterOverrideActive.load(std::memory_order_acquire)
                ? this->livePluginParameterValue.load(std::memory_order_acquire)
                : std::numeric_limits<float>::quiet_NaN();
            renderer.setLivePluginParameterOverride(
                trackIndex, livePluginEffectIndex, livePluginParameterIndex, liveValue);
            break;
        }
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
            outputSampleRate * 60.0 / project->tempoAtBeat(0.0) * 4.0 / project->beatUnit)));
        const qint64 countInFrames = static_cast<qint64>(project->countInBars) *
                                     project->beatsPerBar * beatUnitFrames;
        playbackFrame = -countInFrames;
    }

    bool playingState = playing.load(std::memory_order_acquire);
    const qint64 patternFrames = std::max<qint64>(1,
        project->frameAtBeat(project->songLengthBeats(), outputSampleRate));
    const qint64 releaseTailFrames = static_cast<qint64>(outputSampleRate * 0.08);
    const qint64 clockStart = audioClockFrames.load(std::memory_order_relaxed);
    const qint64 blockProjectStartFrame = playbackFrame;
    const bool blockStartedPlaying = playingState;
    qint64 blockAdvancingFrames = 0;
    if (blockStartedPlaying)
    {
        blockAdvancingFrames = project->loopEnabled
            ? frames
            : std::clamp<qint64>(patternFrames + releaseTailFrames - playbackFrame,
                                 0, frames);
    }
    const auto sampleFormat = outputFormat->sampleFormat();
    qint64 initialProjectFrame = playbackFrame;
    if (project->loopEnabled && initialProjectFrame >= 0)
        initialProjectFrame %= patternFrames;
    renderer.setTransportContext(initialProjectFrame, playingState,
                                 playingState && project->loopEnabled);

    if (liveMidiPanic.exchange(false, std::memory_order_acq_rel))
    {
        renderer.pluginInstrumentsAllNotesOff();
        clearLiveMidiVoices();
        clapPreviewNoteOffPending.store(false, std::memory_order_release);
        const size_t writeIndex = liveMidiWriteIndex.load(std::memory_order_acquire);
        liveMidiReadIndex.store(writeIndex, std::memory_order_release);
    }
    const float bendRange = liveMidiPitchBendRangeSemitones.load(std::memory_order_acquire);
    if (std::abs(bendRange - liveMidiAppliedPitchBendRangeSemitones) > 1.0e-6f)
    {
        liveMidiAppliedPitchBendRangeSemitones = bendRange;
        for (int channel = 0; channel < 16; ++channel)
        {
            const float bendSemitones =
                (static_cast<float>(liveMidiChannelPitchBendValues[static_cast<size_t>(channel)]) - 8192.0f) /
                8192.0f * bendRange;
            liveMidiChannelPitchBends[static_cast<size_t>(channel)] = bendSemitones;
            const double bendRatio = std::pow(2.0, static_cast<double>(bendSemitones) / 12.0);
            for (LiveMidiVoice& voice : liveMidiVoices)
            {
                if (voice.active && !voice.percussion && voice.channel == channel)
                    voice.frequency = voice.baseFrequency * bendRatio;
            }
        }
    }
    size_t midiReadIndex = liveMidiReadIndex.load(std::memory_order_relaxed);
    size_t midiWriteIndex = liveMidiWriteIndex.load(std::memory_order_acquire);
    while (midiReadIndex != midiWriteIndex)
    {
        const LiveMidiEvent event = liveMidiEvents[midiReadIndex];
        midiReadIndex = (midiReadIndex + 1) % liveMidiQueueCapacity;
        liveMidiReadIndex.store(midiReadIndex, std::memory_order_release);
        applyLiveMidiEvent(event, clockStart, renderer);
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

        renderer.setTransportContext(currentFrame, playingState,
                                     playingState && project->loopEnabled);
        renderer.beginMixerFrame();
        StereoSample mixed = playingState ? renderer.renderSample(currentFrame) : StereoSample{};
        const qint64 clockFrame = clockStart + frameIndex;
        if (clapPreviewNoteOffPending.load(std::memory_order_acquire) &&
            clockFrame >= clapPreviewNoteOffFrame.load(std::memory_order_acquire) &&
            clapPreviewNoteOffPending.exchange(false, std::memory_order_acq_rel))
        {
            LiveMidiEvent noteOff;
            noteOff.channel = clapPreviewNoteOffChannel.load(std::memory_order_acquire);
            noteOff.pitch = clapPreviewNoteOffPitch.load(std::memory_order_acquire);
            noteOff.noteId = clapPreviewNoteOffId.load(std::memory_order_acquire);
            noteOff.preview = true;
            applyLiveMidiEvent(noteOff, clockFrame, renderer);
        }
        renderLiveMidiVoices(clockFrame, renderer);
        StereoSample monitorSample;
        bool hasMonitorSample = false;
        if (audioInputMonitoring.load(std::memory_order_acquire))
        {
            quint64 readIndex = audioMonitorReadIndex.load(std::memory_order_relaxed);
            const quint64 writeIndex = audioMonitorWriteIndex.load(std::memory_order_acquire);
            const quint64 generation = audioMonitorGeneration.load(std::memory_order_acquire);
            while (readIndex < writeIndex)
            {
                const AudioMonitorBufferEntry entry =
                    audioMonitorBuffer[readIndex % audioMonitorBufferCapacity];
                ++readIndex;
                audioMonitorReadIndex.store(readIndex, std::memory_order_release);
                if (entry.generation != generation)
                    continue;
                monitorSample = entry.sample;
                hasMonitorSample = true;
                break;
            }
            if (hasMonitorSample && audioMonitorTrackIndex < static_cast<size_t>(project->tracks.size()))
            {
                monitorSample.left *= 0.7f;
                monitorSample.right *= 0.7f;
                renderer.addLiveTrackSample(audioMonitorTrackIndex, monitorSample);
            }
        }
        else
        {
            audioMonitorReadIndex.store(audioMonitorWriteIndex.load(std::memory_order_acquire),
                                        std::memory_order_release);
        }
        const StereoSample mixerOutput = renderer.finishMixerFrame();
        mixed.left += mixerOutput.left;
        mixed.right += mixerOutput.right;
        if (hasMonitorSample && audioMonitorTrackIndex >= static_cast<size_t>(project->tracks.size()))
        {
            mixed.left += monitorSample.left * 0.7f;
            mixed.right += monitorSample.right * 0.7f;
        }
        const int preview = previewPitch.load(std::memory_order_acquire);
        const qint64 previewStart = previewStartFrame.load(std::memory_order_acquire);
        const qint64 previewEnd = previewEndFrame.load(std::memory_order_acquire);
        if (preview >= 0 && clockFrame >= previewStart && clockFrame < previewEnd)
        {
            const qint64 age = clockFrame - previewStart;
            const float velocity = static_cast<float>(previewVelocity.load(std::memory_order_relaxed)) / 127.0f;
            if (previewSample != nullptr)
            {
                double sourcePosition = previewSampleStartFrame.load(std::memory_order_relaxed) +
                    static_cast<double>(age) * previewSample->sampleRate *
                    previewSamplePlaybackRate.load(std::memory_order_relaxed) / outputSampleRate;
                const bool loopEnabled = previewSampleLoopEnabled.load(std::memory_order_relaxed);
            const qint64 loopStartFrame = std::clamp<qint64>(static_cast<qint64>(std::llround(
                previewSampleStartFrame.load(std::memory_order_relaxed))), 0,
                previewSample->frameCount() - 1);
            const qint64 sampleEndFrame = std::clamp<qint64>(
                previewSampleEndFrame.load(std::memory_order_relaxed), loopStartFrame,
                previewSample->frameCount() - 1);
                if (loopEnabled)
                {
                    const double loopLengthFrames = std::max<qint64>(1,
                    sampleEndFrame - loopStartFrame + 1);
                    sourcePosition = loopStartFrame + std::fmod(
                        std::max(0.0, sourcePosition - loopStartFrame), loopLengthFrames);
                }
                const qint64 firstFrame = static_cast<qint64>(sourcePosition);
            if (firstFrame >= 0 && firstFrame <= sampleEndFrame)
                {
                const qint64 secondFrame = firstFrame + 1 <= sampleEndFrame
                        ? firstFrame + 1
                    : loopEnabled ? loopStartFrame : sampleEndFrame;
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
                    const qint64 attackFrames = previewAttackFrames.load(std::memory_order_relaxed);
                    const qint64 decayFrames = previewDecayFrames.load(std::memory_order_relaxed);
                    const qint64 releaseFrames = previewReleaseFrames.load(std::memory_order_relaxed);
                    const float sustainLevel = previewSustainLevel.load(std::memory_order_relaxed);
                    float sampleEnvelope = envelopeLevel(age, attackFrames, decayFrames, sustainLevel);
                    qint64 releaseStartAge = std::max<qint64>(0,
                        previewNoteEndFrame.load(std::memory_order_relaxed) - previewStart);
                    if (!loopEnabled && releaseFrames > 0)
                        releaseStartAge = std::max<qint64>(0,
                            releaseStartAge - releaseFrames);
                    if (releaseFrames > 0 && age >= releaseStartAge)
                    {
                        const qint64 releaseAge = age - releaseStartAge;
                        const float releaseStartLevel = envelopeLevel(releaseStartAge,
                            attackFrames, decayFrames, sustainLevel);
                        sampleEnvelope = releaseStartLevel * std::max(0.0f, 1.0f -
                            static_cast<float>(releaseAge) / static_cast<float>(releaseFrames));
                    }
                    const float sampleGain = 0.48f * velocity * sampleEnvelope *
                                             previewSampleGain.load(std::memory_order_relaxed);
                    mixed.left += left * sampleGain * previewLeftGain.load(std::memory_order_relaxed);
                    mixed.right += right * sampleGain * previewRightGain.load(std::memory_order_relaxed);
                }
            }
            else
            {
                const bool percussion = previewPercussion.load(std::memory_order_relaxed);
                float preview = 0.0f;
                if (percussion)
                {
                    const qint64 gateEndAge = std::max<qint64>(0,
                        previewNoteEndFrame.load(std::memory_order_relaxed) - previewStart);
                    const qint64 attackFrames = previewAttackFrames.load(std::memory_order_relaxed);
                    const qint64 decayFrames = previewDecayFrames.load(std::memory_order_relaxed);
                    const qint64 releaseFrames = previewReleaseFrames.load(std::memory_order_relaxed);
                    const float sustainLevel = previewSustainLevel.load(std::memory_order_relaxed);
                    const float envelope = age < gateEndAge
                        ? envelopeLevel(age, attackFrames, decayFrames, sustainLevel)
                        : releaseFrames > 0
                            ? envelopeLevel(gateEndAge, attackFrames, decayFrames, sustainLevel) *
                                std::max(0.0f, 1.0f - static_cast<float>(age - gateEndAge) /
                                    static_cast<float>(releaseFrames))
                            : 0.0f;
                    preview = renderPercussionSample(previewPitch.load(std::memory_order_relaxed), age,
                                                     previewStart, outputSampleRate) *
                              (0.48f * velocity) * envelope;
                }
                else
                {
                    const quint64 filterResetSerial =
                        previewFilterResetSerial.load(std::memory_order_acquire);
                    if (filterResetSerial != previewFilterSeenSerial)
                    {
                        previewFilterSeenSerial = filterResetSerial;
                        previewFilterX1 = 0.0f;
                        previewFilterX2 = 0.0f;
                        previewFilterY1 = 0.0f;
                        previewFilterY2 = 0.0f;
                        previewFilterActive = previewSynthFilterEnabled.load(std::memory_order_relaxed);
                        previewFilterCachedB0 = previewFilterB0.load(std::memory_order_relaxed);
                        previewFilterCachedB1 = previewFilterB1.load(std::memory_order_relaxed);
                        previewFilterCachedB2 = previewFilterB2.load(std::memory_order_relaxed);
                        previewFilterCachedA1 = previewFilterA1.load(std::memory_order_relaxed);
                        previewFilterCachedA2 = previewFilterA2.load(std::memory_order_relaxed);
                    }
                    const qint64 noteEnd = previewNoteEndFrame.load(std::memory_order_relaxed);
                    const qint64 gateEndAge = std::max<qint64>(0, noteEnd - previewStart);
                    const qint64 amplitudeReleaseFrames =
                        previewReleaseFrames.load(std::memory_order_relaxed);
                    const qint64 amplitudeAttackFrames =
                        previewAttackFrames.load(std::memory_order_relaxed);
                    const qint64 amplitudeDecayFrames =
                        previewDecayFrames.load(std::memory_order_relaxed);
                    const float amplitudeSustainLevel =
                        previewSustainLevel.load(std::memory_order_relaxed);
                    if (clockFrame < noteEnd + amplitudeReleaseFrames)
                    {
                        const double frequency = previewFrequency.load(std::memory_order_relaxed);
                        const double phase = static_cast<double>(age) * frequency / outputSampleRate;
                        float oscillator = oscillatorSample(
                            static_cast<SynthWaveform>(previewWaveform.load(std::memory_order_relaxed)), phase);
                        if (previewSecondOscillatorEnabled.load(std::memory_order_relaxed))
                        {
                            const double detuneRatio = std::pow(2.0,
                                static_cast<double>(previewSecondOscillatorDetuneCents.load(
                                    std::memory_order_relaxed)) / 1200.0);
                            const float secondOscillator = oscillatorSample(
                                static_cast<SynthWaveform>(previewSecondOscillatorWaveform.load(
                                    std::memory_order_relaxed)), phase * detuneRatio);
                            const float mix = previewSecondOscillatorMix.load(std::memory_order_relaxed);
                            oscillator = oscillator * (1.0f - mix) + secondOscillator * mix;
                        }
                        const float amplitudeEnvelope = age < gateEndAge
                            ? envelopeLevel(age, amplitudeAttackFrames, amplitudeDecayFrames,
                                            amplitudeSustainLevel)
                            : amplitudeReleaseFrames > 0
                                ? envelopeLevel(gateEndAge, amplitudeAttackFrames,
                                    amplitudeDecayFrames, amplitudeSustainLevel) * std::max(0.0f, 1.0f -
                                    static_cast<float>(age - gateEndAge) /
                                    static_cast<float>(amplitudeReleaseFrames))
                                : 0.0f;
                        preview = oscillator * (0.2f * velocity) * amplitudeEnvelope;
                    }
                    if (previewFilterActive &&
                        previewFilterEnvelopeEnabled.load(std::memory_order_relaxed) &&
                        (clockFrame & 0x0f) == 0)
                    {
                        const qint64 filterAttackFrames =
                            previewFilterEnvelopeAttackFrames.load(std::memory_order_relaxed);
                        const qint64 filterDecayFrames =
                            previewFilterEnvelopeDecayFrames.load(std::memory_order_relaxed);
                        const qint64 filterReleaseFrames =
                            previewFilterEnvelopeReleaseFrames.load(std::memory_order_relaxed);
                        const float filterSustainLevel =
                            previewFilterEnvelopeSustainLevel.load(std::memory_order_relaxed);
                        const float envelope = age < gateEndAge
                            ? envelopeLevel(age, filterAttackFrames, filterDecayFrames,
                                            filterSustainLevel)
                            : filterReleaseFrames > 0
                                ? envelopeLevel(gateEndAge, filterAttackFrames,
                                    filterDecayFrames, filterSustainLevel) * std::max(0.0f, 1.0f -
                                    static_cast<float>(age - gateEndAge) /
                                    static_cast<float>(filterReleaseFrames))
                                : 0.0f;
                        const double modulatedCutoff =
                            previewFilterCutoffHz.load(std::memory_order_relaxed) * std::exp2(
                                static_cast<double>(previewFilterEnvelopeAmountSemitones.load(
                                    std::memory_order_relaxed)) * envelope / 12.0);
                        const BiquadCoefficients coefficients = synthFilterCoefficients(
                            static_cast<SynthFilterType>(previewFilterType.load(
                                std::memory_order_relaxed)), modulatedCutoff,
                            previewFilterResonance.load(std::memory_order_relaxed), outputSampleRate);
                        previewFilterCachedB0 = coefficients.b0;
                        previewFilterCachedB1 = coefficients.b1;
                        previewFilterCachedB2 = coefficients.b2;
                        previewFilterCachedA1 = coefficients.a1;
                        previewFilterCachedA2 = coefficients.a2;
                    }
                    if (previewFilterActive)
                    {
                        const float inputSample = preview;
                        preview = previewFilterCachedB0 * inputSample +
                                  previewFilterCachedB1 * previewFilterX1 +
                                  previewFilterCachedB2 * previewFilterX2 -
                                  previewFilterCachedA1 * previewFilterY1 -
                                  previewFilterCachedA2 * previewFilterY2;
                        previewFilterX2 = previewFilterX1;
                        previewFilterX1 = inputSample;
                        previewFilterY2 = previewFilterY1;
                        previewFilterY1 = preview;
                    }
                }
                mixed.left += preview * previewLeftGain.load(std::memory_order_relaxed);
                mixed.right += preview * previewRightGain.load(std::memory_order_relaxed);
            }
        }

        mixed = renderer.processMasterEffects(mixed);
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
    const size_t meterBusCount = std::min(static_cast<size_t>(project->buses.size()), maximumBusCount);
    for (size_t index = 0; index < meterBusCount; ++index)
    {
        const float blockPeak = renderer.busPeakLevel(index);
        const float decayedPeak = busPeakValues[index].load(std::memory_order_relaxed) * 0.82f;
        busPeakValues[index].store(std::max(blockPeak, decayedPeak), std::memory_order_release);
    }
    const float decayedMasterPeak = masterPeakValue.load(std::memory_order_relaxed) * 0.82f;
    masterPeakValue.store(std::max(blockMasterPeak, decayedMasterPeak), std::memory_order_release);

    audioClockFrames.store(clockStart + frames, std::memory_order_release);
    publishTransportOutputSegment(
        renderState->generation, clockStart,
        blockStartedPlaying ? blockProjectStartFrame : 0,
        frames, blockAdvancingFrames, patternFrames, project->loopEnabled, true);
    const qint64 publishedPosition = playingState
        ? (project->loopEnabled ? playbackFrame % patternFrames : std::min(playbackFrame, patternFrames))
        : 0;
    publishedPositionFrames.store(publishedPosition, std::memory_order_release);
    return static_cast<qint64>(frames) * bytesPerFrame;
}
