#include "SynthRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace
{
constexpr double twoPi = 6.28318530717958647692;

int drumPadForPitch(int pitch)
{
    if (pitch == 35 || pitch == 36) return 0;
    if (pitch == 38 || pitch == 40) return 1;
    if (pitch == 42 || pitch == 44) return 2;
    if (pitch == 46) return 3;
    return -1;
}

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

bool probabilityPasses(int probabilityPercent, quint64 seed, quint64 cycle)
{
    if (probabilityPercent >= 100)
        return true;
    if (probabilityPercent <= 0)
        return false;

    quint64 value = seed ^ (cycle * 0x9e3779b97f4a7c15ULL);
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return static_cast<int>(value % 100) < probabilityPercent;
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

float automationValue(const QVector<AutomationPoint>& points, double beat, float defaultValue)
{
    if (points.isEmpty())
        return defaultValue;
    const auto next = std::upper_bound(points.cbegin(), points.cend(), beat,
        [](double value, const AutomationPoint& point) { return value < point.beat; });
    if (next == points.cbegin())
        return defaultValue;

    const auto previous = std::prev(next);
    if (next == points.cend())
        return previous->value;

    const double span = next->beat - previous->beat;
    if (span <= 0.0)
        return next->value;
    const float amount = static_cast<float>(automationCurveProgress(
        previous->curveToNext, (beat - previous->beat) / span));
    return previous->value + (next->value - previous->value) * amount;
}
}

SynthRenderer::ParametricEqCoefficients SynthRenderer::makeParametricEqCoefficients(
    const ParametricEqBand& band, double sampleRate)
{
    ParametricEqCoefficients coefficients;
    if (std::abs(band.gainDb) < 1.0e-5f)
        return coefficients;

    const double frequency = std::clamp(static_cast<double>(band.frequencyHz),
                                         20.0, std::max(20.0, sampleRate * 0.49));
    const double q = std::clamp(static_cast<double>(band.q), 0.1, 10.0);
    const double gain = std::pow(10.0, std::clamp(static_cast<double>(band.gainDb),
                                                  -18.0, 18.0) / 40.0);
    const double omega = twoPi * frequency / sampleRate;
    const double sine = std::sin(omega);
    const double alpha = sine / (2.0 * q);
    const double cosine = std::cos(omega);
    const double a0 = 1.0 + alpha / gain;
    coefficients.b0 = static_cast<float>((1.0 + alpha * gain) / a0);
    coefficients.b1 = static_cast<float>((-2.0 * cosine) / a0);
    coefficients.b2 = static_cast<float>((1.0 - alpha * gain) / a0);
    coefficients.a1 = static_cast<float>((-2.0 * cosine) / a0);
    coefficients.a2 = static_cast<float>((1.0 - alpha / gain) / a0);
    coefficients.active = true;
    return coefficients;
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

const SynthRenderer::TrackMixValue& SynthRenderer::trackMixAt(size_t trackIndex, qint64 frame, double beat)
{
    TrackMixValue& rendered = trackMixValues[trackIndex];
    if (rendered.frame != frame)
    {
        const TrackMixAutomation& automation = trackMixAutomations[trackIndex];
        const float volume = std::clamp(automationValue(automation.volume, beat, automation.baseVolume),
                                        0.0f, 1.0f);
        const float pan = std::clamp(automationValue(automation.pan, beat, automation.basePan),
                                     -1.0f, 1.0f);
        const LiveTrackMixOverride& live = liveTrackMixOverrides[trackIndex];
        for (size_t bandIndex = 0; bandIndex < automation.equalizerGain.size(); ++bandIndex)
        {
            float gainDb = automationValue(automation.equalizerGain[bandIndex], beat,
                                           automation.baseEqualizerDb[bandIndex]);
            if (live.equalizerGainDb[bandIndex] >= -18.0f &&
                live.equalizerGainDb[bandIndex] <= 18.0f)
                gainDb = live.equalizerGainDb[bandIndex];
            gainDb = std::clamp(
                gainDb, -18.0f, 18.0f);
            rendered.equalizerGains[bandIndex] = static_cast<float>(
                std::pow(10.0, static_cast<double>(gainDb) / 20.0));
        }
        const double maximumCrossoverHz = std::max(20.0, sampleRate * 0.49);
        const float lowCrossoverMaximum = static_cast<float>(
            std::max(20.0, std::min(2000.0, maximumCrossoverHz)));
        float lowCrossoverHz = automationValue(automation.equalizerCrossover[0], beat,
                                               automation.baseEqualizerCrossoverHz[0]);
        if (live.equalizerCrossoverHz[0] > 0.0f)
            lowCrossoverHz = live.equalizerCrossoverHz[0];
        lowCrossoverHz = std::clamp(lowCrossoverHz, 20.0f, lowCrossoverMaximum);
        const float highCrossoverMinimum = static_cast<float>(std::min(
            maximumCrossoverHz, std::max(500.0, static_cast<double>(lowCrossoverHz) + 1.0)));
        float highCrossoverHz = automationValue(automation.equalizerCrossover[1], beat,
                                                automation.baseEqualizerCrossoverHz[1]);
        if (live.equalizerCrossoverHz[1] > 0.0f)
            highCrossoverHz = live.equalizerCrossoverHz[1];
        highCrossoverHz = std::clamp(highCrossoverHz, highCrossoverMinimum,
                                     static_cast<float>(maximumCrossoverHz));
        rendered.lowBandCoefficient = static_cast<float>(
            1.0 - std::exp(-twoPi * lowCrossoverHz / sampleRate));
        rendered.highBandCoefficient = static_cast<float>(
            1.0 - std::exp(-twoPi * highCrossoverHz / sampleRate));
        for (size_t bandIndex = 0; bandIndex < automation.baseParametricEqBands.size();
             ++bandIndex)
        {
            ParametricEqBand band = automation.baseParametricEqBands[bandIndex];
            if (automation.parametricEqAutomationActive[bandIndex])
            {
                band.frequencyHz = automationValue(
                    automation.parametricEqAutomation[bandIndex][0], beat, band.frequencyHz);
                band.gainDb = automationValue(
                    automation.parametricEqAutomation[bandIndex][1], beat, band.gainDb);
                band.q = automationValue(
                    automation.parametricEqAutomation[bandIndex][2], beat, band.q);
            }
            const auto& liveBand = live.parametricEqValues[bandIndex];
            if (liveBand[0] >= 20.0f && liveBand[0] <= 20000.0f)
                band.frequencyHz = liveBand[0];
            if (liveBand[1] >= -18.0f && liveBand[1] <= 18.0f)
                band.gainDb = liveBand[1];
            if (liveBand[2] >= 0.1f && liveBand[2] <= 10.0f)
                band.q = liveBand[2];
            band.frequencyHz = std::clamp(band.frequencyHz, 20.0f, 20000.0f);
            band.gainDb = std::clamp(band.gainDb, -18.0f, 18.0f);
            band.q = std::clamp(band.q, 0.1f, 10.0f);
            rendered.parametricEqBands[bandIndex] = band;
        }
        const float effectiveVolume = live.volume >= 0.0f ? std::clamp(live.volume, 0.0f, 1.0f) : volume;
        const float effectivePan = live.pan >= -1.0f && live.pan <= 1.0f
            ? std::clamp(live.pan, -1.0f, 1.0f) : pan;
        const double panAngle = (static_cast<double>(effectivePan) + 1.0) * 0.7853981633974483;
        rendered.leftGain = effectiveVolume * static_cast<float>(std::cos(panAngle));
        rendered.rightGain = effectiveVolume * static_cast<float>(std::sin(panAngle));
        rendered.frame = frame;
    }
    return rendered;
}

void SynthRenderer::setProject(std::shared_ptr<const ProjectData> newProject, double newSampleRate)
{
    project = std::move(newProject);
    sampleRate = std::max(1.0, newSampleRate);
    currentBeat = 0.0;
    transportProjectBeat = 0.0;
    currentProjectFrame = 0;
    transportPlaying = true;
    transportCycleActive = false;
    notes.clear();
    pitchBendEvents.clear();
    pluginInstrumentEvents.clear();
    trackPitchBends.clear();
    activeNotes.clear();
    audioRegions.clear();
    activeAudioRegions.clear();
    trackMixAutomations.clear();
    trackMixValues.clear();
    liveTrackMixOverrides.clear();
    trackEqualizerStates.clear();
    trackInstrumentFilterStates.clear();
    trackInstrumentFilterHasTail.clear();
    synthFilterEnvelopeSemitoneOffsets.clear();
    synthFilterEnvelopeAppliedOffsets.clear();
    trackEffectStates.clear();
    clapInstruments.clear();
    vst3Instruments.clear();
    pluginInstrumentLiveParameterOverrides.clear();
    vst3InstrumentManualParameterOverrides.clear();
    trackEffectHasTail.clear();
    activeEqualizerTailTracks.clear();
    trackBusIndices.clear();
    trackMainLatencyDelays.clear();
    trackInstrumentSourceLatencyDelays.clear();
    trackSends.clear();
    trackAudible.clear();
    busMixStates.clear();
    busEffectStates.clear();
    busEffectHasTail.clear();
    busSends.clear();
    busOutputIndices.clear();
    busMainLatencyDelays.clear();
    busInputAudible.clear();
    busOutputAudible.clear();
    busSoloDownstreamAudible.clear();
    busMutedAtBeat.clear();
    busSoloAtBeat.clear();
    busAudibilityInitialized = false;
    busRouteOrder.clear();
    busFrameSamples.clear();
    lastBusOutputSamples.clear();
    busFrameTouched.clear();
    touchedBusIndices.clear();
    busPeakLevels.clear();
    trackFrameSamples.clear();
    trackFramePluginInstrumentSamples.clear();
    trackFrameTouched.clear();
    touchedTrackIndices.clear();
    trackPeakLevels.clear();
    hasBusSolo = false;
    nextStartIndex = 0;
    nextPitchBendIndex = 0;
    nextPluginInstrumentEventIndex = 0;
    nextAudioStartIndex = 0;
    playbackCycle = 0;
    lastFrame = -1;
    mixerFrameNumber = 0;
    nextPluginArrangementNoteId = 1;
    if (!project)
        return;

    trackMixAutomations.reserve(static_cast<size_t>(project->tracks.size()));
    trackMixValues.resize(static_cast<size_t>(project->tracks.size()));
    liveTrackMixOverrides.resize(static_cast<size_t>(project->tracks.size()));
    liveSynthFilterCutoffOverrides.assign(static_cast<size_t>(project->tracks.size()), -1.0f);
    liveSynthFilterResonanceOverrides.assign(static_cast<size_t>(project->tracks.size()), -1.0f);
    trackPitchBends.assign(static_cast<size_t>(project->tracks.size()), {});
    trackEqualizerStates.resize(static_cast<size_t>(project->tracks.size()));
    trackInstrumentFilterStates.resize(static_cast<size_t>(project->tracks.size()));
    trackInstrumentFilterHasTail.assign(static_cast<size_t>(project->tracks.size()), 0);
    synthFilterEnvelopeSemitoneOffsets.assign(static_cast<size_t>(project->tracks.size()), 0.0f);
    synthFilterEnvelopeAppliedOffsets.assign(static_cast<size_t>(project->tracks.size()), 0.0f);
    trackEffectStates.resize(static_cast<size_t>(project->tracks.size()));
    clapInstruments.resize(static_cast<size_t>(project->tracks.size()));
    vst3Instruments.resize(static_cast<size_t>(project->tracks.size()));
    pluginInstrumentLiveParameterOverrides.resize(static_cast<size_t>(project->tracks.size()));
    vst3InstrumentManualParameterOverrides.resize(static_cast<size_t>(project->tracks.size()));
    trackEffectHasTail.assign(static_cast<size_t>(project->tracks.size()), 0);
    trackBusIndices.resize(static_cast<size_t>(project->tracks.size()), -1);
    trackMainLatencyDelays.resize(static_cast<size_t>(project->tracks.size()));
    trackInstrumentSourceLatencyDelays.resize(static_cast<size_t>(project->tracks.size()));
    trackSends.resize(static_cast<size_t>(project->tracks.size()));
    trackAudible.assign(static_cast<size_t>(project->tracks.size()), 0);
    busMixStates.reserve(static_cast<size_t>(project->buses.size()));
    busEffectStates.resize(static_cast<size_t>(project->buses.size()));
    busEffectHasTail.assign(static_cast<size_t>(project->buses.size()), 0);
    busSends.resize(static_cast<size_t>(project->buses.size()));
    busOutputIndices.assign(static_cast<size_t>(project->buses.size()), -1);
    busMainLatencyDelays.resize(static_cast<size_t>(project->buses.size()));
    busInputAudible.assign(static_cast<size_t>(project->buses.size()), 0);
    busOutputAudible.assign(static_cast<size_t>(project->buses.size()), 0);
    busSoloDownstreamAudible.assign(static_cast<size_t>(project->buses.size()), 0);
    busMutedAtBeat.assign(static_cast<size_t>(project->buses.size()), 0);
    busSoloAtBeat.assign(static_cast<size_t>(project->buses.size()), 0);
    busRouteOrder.resize(static_cast<size_t>(project->buses.size()));
    busFrameSamples.resize(static_cast<size_t>(project->buses.size()));
    lastBusOutputSamples.resize(static_cast<size_t>(project->buses.size()));
    busFrameTouched.assign(static_cast<size_t>(project->buses.size()), 0);
    touchedBusIndices.reserve(static_cast<size_t>(project->buses.size()));
    busPeakLevels.assign(static_cast<size_t>(project->buses.size()), 0.0f);
    trackFrameSamples.resize(static_cast<size_t>(project->tracks.size()));
    trackFramePluginInstrumentSamples.resize(static_cast<size_t>(project->tracks.size()));
    trackFrameTouched.assign(static_cast<size_t>(project->tracks.size()), 0);
    touchedTrackIndices.reserve(static_cast<size_t>(project->tracks.size()));
    trackPeakLevels.assign(static_cast<size_t>(project->tracks.size()), 0.0f);
    prepareEffectChain(project->masterEffects, masterEffectStates);
    for (size_t trackIndex = 0; trackIndex < static_cast<size_t>(project->tracks.size()); ++trackIndex)
    {
        const InstrumentTrack& track = project->tracks[static_cast<qsizetype>(trackIndex)];
        TrackMixAutomation mixAutomation;
        mixAutomation.baseVolume = track.gain;
        mixAutomation.basePan = track.pan;
        mixAutomation.baseEqualizerDb = {track.eqLowDb, track.eqMidDb, track.eqHighDb};
        mixAutomation.baseEqualizerCrossoverHz = {track.eqLowCrossoverHz,
                                                  track.eqHighCrossoverHz};
        mixAutomation.baseParametricEqBands = track.eqParametricBands;
        mixAutomation.volume = track.volumeAutomation;
        mixAutomation.pan = track.panAutomation;
        mixAutomation.equalizerGain = {track.eqLowGainAutomation,
                                       track.eqMidGainAutomation,
                                       track.eqHighGainAutomation};
        mixAutomation.equalizerCrossover = {track.eqLowCrossoverAutomation,
                                            track.eqHighCrossoverAutomation};
        mixAutomation.parametricEqAutomation = track.eqParametricAutomation;
        for (size_t bandIndex = 0; bandIndex < mixAutomation.parametricEqAutomation.size();
             ++bandIndex)
            mixAutomation.parametricEqAutomationActive[bandIndex] = std::any_of(
                mixAutomation.parametricEqAutomation[bandIndex].cbegin(),
                mixAutomation.parametricEqAutomation[bandIndex].cend(),
                [](const QVector<AutomationPoint>& points) { return !points.isEmpty(); });
        trackMixAutomations.push_back(std::move(mixAutomation));
        TrackEqualizerState& equalizerState = trackEqualizerStates[trackIndex];
        const double maximumCutoff = std::max(20.0, sampleRate * 0.49);
        const double lowCutoff = std::clamp(static_cast<double>(track.eqLowCrossoverHz),
                                             20.0, maximumCutoff);
        const double highCutoff = std::clamp(static_cast<double>(track.eqHighCrossoverHz),
                                              lowCutoff, maximumCutoff);
        equalizerState.lowBandCoefficient = static_cast<float>(
            1.0 - std::exp(-twoPi * lowCutoff / sampleRate));
        equalizerState.highBandCoefficient = static_cast<float>(
            1.0 - std::exp(-twoPi * highCutoff / sampleRate));
        for (size_t bandIndex = 0; bandIndex < equalizerState.parametricCoefficients.size(); ++bandIndex)
        {
            equalizerState.parametricCoefficients[bandIndex] = makeParametricEqCoefficients(
                track.eqParametricBands[bandIndex], sampleRate);
            equalizerState.parametricBandSettings[bandIndex] = track.eqParametricBands[bandIndex];
        }
        if (track.instrumentType == InstrumentType::Synth && track.synthFilterEnabled)
        {
            MixerEffect instrumentFilter = defaultMixerEffect(MixerEffectType::LowPass);
            instrumentFilter.cutoffHz = track.synthFilterCutoffHz;
            instrumentFilter.resonance = track.synthFilterResonance;
            QVector<MixerEffect> instrumentFilterChain{instrumentFilter};
            prepareEffectChain(instrumentFilterChain, trackInstrumentFilterStates[trackIndex]);
            if (!trackInstrumentFilterStates[trackIndex].empty())
                configureFilter(trackInstrumentFilterStates[trackIndex].front(), track.synthFilterType);
        }
        if (track.instrumentType == InstrumentType::ClapInstrument && !track.clapPluginId.isEmpty())
        {
            clapInstruments[trackIndex] = std::make_unique<ClapAudioPluginInstance>(
                track.clapPluginId, ClapPluginKind::Instrument, sampleRate,
                track.clapParameterValues, track.clapPluginState);
            pluginInstrumentLiveParameterOverrides[trackIndex].assign(
                static_cast<size_t>(clapInstruments[trackIndex]->parameterCount()),
                std::numeric_limits<float>::quiet_NaN());
        }
        else if (track.instrumentType == InstrumentType::Vst3Instrument &&
                 !track.vst3PluginId.isEmpty())
        {
            vst3Instruments[trackIndex] = std::make_unique<Vst3AudioInstrumentInstance>(
                track.vst3PluginId, sampleRate, track.vst3ParameterValues, track.vst3PluginState);
            pluginInstrumentLiveParameterOverrides[trackIndex].assign(
                static_cast<size_t>(vst3Instruments[trackIndex]->parameterCount()),
                std::numeric_limits<float>::quiet_NaN());
            vst3InstrumentManualParameterOverrides[trackIndex].assign(
                static_cast<size_t>(vst3Instruments[trackIndex]->parameterCount()),
                std::numeric_limits<float>::quiet_NaN());
        }
        prepareEffectChain(track.effects, trackEffectStates[trackIndex]);
    }

    std::unordered_map<quint64, int> busIndexById;
    for (size_t busIndex = 0; busIndex < static_cast<size_t>(project->buses.size()); ++busIndex)
    {
        const MixerBus& bus = project->buses[static_cast<qsizetype>(busIndex)];
        busIndexById.emplace(bus.id, static_cast<int>(busIndex));
        const float leftBalance = bus.pan > 0.0f ? 1.0f - bus.pan : 1.0f;
        const float rightBalance = bus.pan < 0.0f ? 1.0f + bus.pan : 1.0f;
        busMixStates.push_back({bus.gain * leftBalance, bus.gain * rightBalance});
        prepareEffectChain(bus.effects, busEffectStates[busIndex]);
    }
    for (size_t busIndex = 0; busIndex < static_cast<size_t>(project->buses.size()); ++busIndex)
    {
        const MixerBus& bus = project->buses[static_cast<qsizetype>(busIndex)];
        const auto parent = busIndexById.find(bus.outputBusId);
        busOutputIndices[busIndex] = parent == busIndexById.end() ? -1 : parent->second;
        for (const MixerSend& send : bus.sends)
        {
            const auto sendBus = busIndexById.find(send.busId);
            if (sendBus != busIndexById.end())
                busSends[busIndex].push_back({sendBus->second, send.gain, send.automation, {}});
        }
    }

    std::vector<size_t> busIndegrees(busOutputIndices.size(), 0);
    for (size_t busIndex = 0; busIndex < busOutputIndices.size(); ++busIndex)
    {
        const int parentIndex = busOutputIndices[busIndex];
        if (parentIndex >= 0)
            ++busIndegrees[static_cast<size_t>(parentIndex)];
        for (const TrackSendMix& send : busSends[busIndex])
            if (send.busIndex >= 0)
                ++busIndegrees[static_cast<size_t>(send.busIndex)];
    }
    std::vector<size_t> readyBuses;
    readyBuses.reserve(busOutputIndices.size());
    for (size_t busIndex = 0; busIndex < busIndegrees.size(); ++busIndex)
        if (busIndegrees[busIndex] == 0)
            readyBuses.push_back(busIndex);
    busRouteOrder.clear();
    busRouteOrder.reserve(busOutputIndices.size());
    for (size_t readyIndex = 0; readyIndex < readyBuses.size(); ++readyIndex)
    {
        const size_t busIndex = readyBuses[readyIndex];
        busRouteOrder.push_back(busIndex);
        const auto releaseDestination = [&](int destinationIndex)
        {
            if (destinationIndex < 0)
                return;
            const size_t destination = static_cast<size_t>(destinationIndex);
            if (--busIndegrees[destination] == 0)
                readyBuses.push_back(destination);
        };
        releaseDestination(busOutputIndices[busIndex]);
        for (const TrackSendMix& send : busSends[busIndex])
            releaseDestination(send.busIndex);
    }
    if (busRouteOrder.size() < busOutputIndices.size())
    {
        for (size_t busIndex = 0; busIndex < busOutputIndices.size(); ++busIndex)
        {
            if (std::find(busRouteOrder.begin(), busRouteOrder.end(), busIndex) == busRouteOrder.end())
                busRouteOrder.push_back(busIndex);
        }
    }

    busAudibilityInitialized = false;
    updateBusAudibility();

    bool hasTrackSolo = false;
    for (const InstrumentTrack& track : project->tracks)
        hasTrackSolo = hasTrackSolo || track.solo;

    int synthChannel = 0;
    for (size_t trackIndex = 0; trackIndex < static_cast<size_t>(project->tracks.size()); ++trackIndex)
    {
        const InstrumentTrack& track = project->tracks[static_cast<qsizetype>(trackIndex)];
        int trackMidiChannel = 9;
        if (track.instrumentType == InstrumentType::Synth ||
            track.instrumentType == InstrumentType::ClapInstrument ||
            track.instrumentType == InstrumentType::Vst3Instrument)
        {
            const int channelSlot = synthChannel++ % 15;
            trackMidiChannel = channelSlot < 9 ? channelSlot : channelSlot + 1;
        }
        const auto busPosition = busIndexById.find(track.outputBusId);
        const int busIndex = busPosition == busIndexById.end() ? -1 : busPosition->second;
        trackBusIndices[trackIndex] = busIndex;
        for (const MixerSend& send : track.sends)
        {
            const auto sendBusPosition = busIndexById.find(send.busId);
            if (sendBusPosition != busIndexById.end())
                trackSends[trackIndex].push_back({sendBusPosition->second, send.gain, send.automation, {}});
        }
        const bool audible = !track.muted && (!hasTrackSolo || track.solo);
        trackAudible[trackIndex] = audible ? 1 : 0;
        if (!audible)
            continue;

        for (const MidiClip& clip : track.clips)
        {
            if (clip.isAudioRegion() && clip.audioTakeGroupId != 0 && !clip.audioTakeActive)
                continue;
            if (clip.audioSource != nullptr && clip.audioSource->sampleRate > 0 &&
                clip.audioSource->channelCount > 0 && clip.audioSource->frameCount() > clip.audioStartFrame)
            {
                AudioRegion region;
                region.startFrame = project->frameAtBeat(clip.startBeat, static_cast<int>(sampleRate));
                region.sourceStartFrame = clip.audioStartFrame;
                region.fadeInFrames = project->framesBetweenBeats(
                    clip.startBeat, clip.startBeat + clip.audioFadeInBeats, static_cast<int>(sampleRate));
                region.fadeOutFrames = project->framesBetweenBeats(
                    clip.startBeat + clip.lengthBeats - clip.audioFadeOutBeats,
                    clip.startBeat + clip.lengthBeats, static_cast<int>(sampleRate));
                region.fadeInCurve = clip.audioFadeInCurve;
                region.fadeOutCurve = clip.audioFadeOutCurve;
                region.fadeInShape = clip.audioFadeInShape;
                region.fadeOutShape = clip.audioFadeOutShape;
                region.fadeInCurvePoints = clip.audioFadeInCurvePoints;
                region.fadeOutCurvePoints = clip.audioFadeOutCurvePoints;
                region.source = clip.audioSource;
                region.trackIndex = trackIndex;
                const qint64 clipDuration = project->framesBetweenBeats(
                    clip.startBeat, clip.startBeat + clip.lengthBeats, static_cast<int>(sampleRate));
                const qint64 sourceFrames = clip.audioSource->frameCount() - clip.audioStartFrame;
                const qint64 sourceDuration = static_cast<qint64>(std::ceil(
                    static_cast<double>(sourceFrames) * sampleRate / clip.audioSource->sampleRate));
                region.endFrame = region.startFrame + std::max<qint64>(1,
                    std::min(clipDuration, sourceDuration));
                const qint64 regionFrames = region.endFrame - region.startFrame;
                region.fadeInFrames = std::min(region.fadeInFrames, regionFrames);
                region.fadeOutFrames = std::min(region.fadeOutFrames, regionFrames);
                audioRegions.push_back(std::move(region));
                continue;
            }
            for (const MidiPitchBend& bend : clip.pitchBends)
            {
                PitchBendEvent event;
                event.frame = project->frameAtBeat(clip.startBeat + bend.beat,
                                                    static_cast<int>(sampleRate));
                event.trackIndex = trackIndex;
                event.channel = std::clamp(bend.channel, 0, 15);
                event.value = std::clamp(bend.value, 0, 16383);
                event.semitones = (static_cast<float>(std::clamp(bend.value, 0, 16383)) - 8192.0f) /
                                  8192.0f * static_cast<float>(project->pitchBendRangeSemitones);
                pitchBendEvents.push_back(event);
            }
            for (const MidiNote& note : clip.notes)
            {
                const quint64 probabilitySeed = track.id * 0x9e3779b97f4a7c15ULL ^
                    clip.id * 0xbf58476d1ce4e5b9ULL ^ note.id * 0x94d049bb133111ebULL;
                if (track.instrumentType == InstrumentType::ClapInstrument ||
                    track.instrumentType == InstrumentType::Vst3Instrument)
                {
                    PluginInstrumentEvent noteOn;
                    noteOn.type = PluginInstrumentEvent::Type::NoteOn;
                    noteOn.frame = project->frameAtBeat(clip.startBeat + note.startBeat,
                                                         static_cast<int>(sampleRate));
                    noteOn.endFrame = std::max(noteOn.frame + 1, project->frameAtBeat(
                        clip.startBeat + note.startBeat + note.lengthBeats,
                        static_cast<int>(sampleRate)));
                    noteOn.trackIndex = trackIndex;
                    noteOn.noteId = nextPluginArrangementNoteId;
                    nextPluginArrangementNoteId = nextPluginArrangementNoteId ==
                        std::numeric_limits<std::int32_t>::max()
                        ? 1 : nextPluginArrangementNoteId + 1;
                    noteOn.channel = note.channel >= 0
                        ? std::clamp(note.channel, 0, 15) : trackMidiChannel;
                    noteOn.key = std::clamp(note.pitch, 0, 127);
                    noteOn.velocity = std::clamp(static_cast<float>(note.velocity) / 127.0f,
                                                  0.0f, 1.0f);
                    noteOn.probabilityPercent = note.probabilityPercent;
                    noteOn.probabilitySeed = probabilitySeed;
                    pluginInstrumentEvents.push_back(noteOn);

                    PluginInstrumentEvent noteOff = noteOn;
                    noteOff.type = PluginInstrumentEvent::Type::NoteOff;
                    noteOff.frame = noteOn.endFrame;
                    noteOff.velocity = 0.0f;
                    pluginInstrumentEvents.push_back(noteOff);
                    continue;
                }
                if (track.instrumentType == InstrumentType::DrumRack)
                {
                    const int pad = drumPadForPitch(note.pitch);
                    if (pad >= 0 && track.drumSamples[static_cast<size_t>(pad)].hasSampleAssignment())
                    {
                        const DrumPadSample& padSample = track.drumSamples[static_cast<size_t>(pad)];
                        const int layerIndex = padSample.sourceLayerIndexForVelocity(note.velocity);
                        const std::shared_ptr<const AudioAsset>& source =
                            padSample.sourceForVelocity(note.velocity);
                        if (source != nullptr && source->sampleRate > 0 &&
                            source->channelCount > 0 && source->frameCount() > 0)
                        {
                            const DrumLayerSettings layerSettings =
                                padSample.layerSettings(layerIndex);
                            AudioRegion region;
                            const double noteStartBeat = clip.startBeat + note.startBeat;
                            region.source = source;
                            region.trackIndex = trackIndex;
                            region.drumSamplePlayback = true;
                            region.drumSampleLoop = layerSettings.loopEnabled;
                            region.drumAttackFrames = std::max<qint64>(0,
                                static_cast<qint64>(std::llround(layerSettings.attackSeconds * sampleRate)));
                            region.drumDecayFrames = std::max<qint64>(0,
                                static_cast<qint64>(std::llround(layerSettings.decaySeconds * sampleRate)));
                            region.drumReleaseFrames = std::max<qint64>(0,
                                static_cast<qint64>(std::llround(layerSettings.releaseSeconds * sampleRate)));
                            region.drumSustainLevel = layerSettings.sustainLevel;
                            region.probabilityPercent = note.probabilityPercent;
                            region.probabilitySeed = probabilitySeed;
                            region.sourceStartFrame = static_cast<qint64>(std::llround(
                                static_cast<double>(source->frameCount() - 1) *
                                layerSettings.startPositionPercent / 100.0));
                            region.sourceEndFrame = static_cast<qint64>(std::llround(
                                static_cast<double>(source->frameCount() - 1) *
                                layerSettings.endPositionPercent / 100.0));
                            region.sourceEndFrame = std::clamp<qint64>(region.sourceEndFrame,
                                region.sourceStartFrame, source->frameCount() - 1);
                            region.playbackRate = std::pow(2.0,
                                static_cast<double>(layerSettings.tuningSemitones) / 12.0);
                            const float velocityGain = 0.48f * static_cast<float>(note.velocity) / 127.0f;
                            region.sourceGain = velocityGain;
                            const int ratchetCount = std::clamp(note.ratchetCount, 1, 8);
                            const qint64 remainingSourceFrames =
                                region.sourceEndFrame - region.sourceStartFrame + 1;
                            const qint64 oneShotDurationFrames = static_cast<qint64>(std::ceil(
                                static_cast<double>(remainingSourceFrames) * sampleRate /
                                (source->sampleRate * region.playbackRate)));
                            for (int hit = 0; hit < ratchetCount; ++hit)
                            {
                                AudioRegion hitRegion = region;
                                const qint64 hitStartFrame = project->frameAtBeat(
                                    noteStartBeat + note.lengthBeats * hit / ratchetCount,
                                    static_cast<int>(sampleRate));
                                const qint64 hitEndFrame = project->frameAtBeat(
                                    noteStartBeat + note.lengthBeats * (hit + 1) / ratchetCount,
                                    static_cast<int>(sampleRate));
                                hitRegion.startFrame = hitStartFrame;
                                const qint64 duration = region.drumSampleLoop
                                    ? std::max<qint64>(1, hitEndFrame - hitStartFrame)
                                    : std::max<qint64>(1, oneShotDurationFrames);
                                if (region.drumSampleLoop)
                                {
                                    hitRegion.drumGateEndFrame = hitRegion.startFrame + duration;
                                    hitRegion.endFrame = hitRegion.drumGateEndFrame +
                                                         hitRegion.drumReleaseFrames;
                                }
                                else
                                {
                                    hitRegion.endFrame = hitRegion.startFrame + duration;
                                    hitRegion.drumReleaseFrames = std::min(
                                        hitRegion.drumReleaseFrames, duration);
                                    hitRegion.drumGateEndFrame = hitRegion.endFrame -
                                        std::min(hitRegion.drumReleaseFrames, duration);
                                }
                                audioRegions.push_back(std::move(hitRegion));
                            }
                        }
                        continue;
                    }
                }

                const double startBeat = clip.startBeat + note.startBeat;
                const int ratchetCount = track.instrumentType == InstrumentType::DrumRack
                    ? std::clamp(note.ratchetCount, 1, 8) : 1;
                for (int hit = 0; hit < ratchetCount; ++hit)
                {
                    VoiceNote voice;
                    voice.startFrame = project->frameAtBeat(
                        startBeat + note.lengthBeats * hit / ratchetCount,
                        static_cast<int>(sampleRate));
                    voice.endFrame = std::max(voice.startFrame + 1, project->frameAtBeat(
                        startBeat + note.lengthBeats * (hit + 1) / ratchetCount,
                        static_cast<int>(sampleRate)));
                    voice.attackFrames = std::max<qint64>(1, static_cast<qint64>(std::llround(
                        track.attackSeconds * sampleRate)));
                    voice.decayFrames = std::max<qint64>(0, static_cast<qint64>(std::llround(
                        track.decaySeconds * sampleRate)));
                    voice.releaseFrames = std::max<qint64>(1, static_cast<qint64>(std::llround(
                        track.releaseSeconds * sampleRate)));
                    voice.trackIndex = trackIndex;
                    voice.sustainLevel = track.sustainLevel;
                    voice.baseFrequency = 440.0 * std::pow(2.0, (note.pitch - 69) / 12.0);
                    voice.frequency = voice.baseFrequency;
                    voice.amplitude = (track.instrumentType == InstrumentType::DrumRack ? 0.48f : 0.22f) *
                                      static_cast<float>(note.velocity) / 127.0f;
                    voice.waveform = track.waveform;
                    voice.secondOscillatorWaveform = track.secondOscillatorWaveform;
                    voice.secondOscillatorDetuneCents = track.secondOscillatorDetuneCents;
                    voice.secondOscillatorMix = track.secondOscillatorMix;
                    voice.secondOscillatorEnabled = track.instrumentType == InstrumentType::Synth &&
                                                    track.secondOscillatorEnabled;
                    voice.filterEnvelopeEnabled = track.instrumentType == InstrumentType::Synth &&
                        track.synthFilterEnabled && track.synthFilterEnvelopeEnabled;
                    voice.filterEnvelopeAttackFrames = std::max<qint64>(0,
                        static_cast<qint64>(std::llround(
                            track.synthFilterEnvelopeAttackSeconds * sampleRate)));
                    voice.filterEnvelopeDecayFrames = std::max<qint64>(0,
                        static_cast<qint64>(std::llround(
                            track.synthFilterEnvelopeDecaySeconds * sampleRate)));
                    voice.filterEnvelopeReleaseFrames = std::max<qint64>(0,
                        static_cast<qint64>(std::llround(
                            track.synthFilterEnvelopeReleaseSeconds * sampleRate)));
                    voice.filterEnvelopeAmountSemitones = track.synthFilterEnvelopeAmountSemitones;
                    voice.filterEnvelopeSustainLevel = track.synthFilterEnvelopeSustainLevel;
                    voice.midiPitch = note.pitch;
                    voice.midiChannel = note.channel >= 0 ? note.channel : trackMidiChannel;
                    voice.probabilityPercent = note.probabilityPercent;
                    voice.probabilitySeed = probabilitySeed;
                    voice.percussion = track.instrumentType == InstrumentType::DrumRack;
                    if (voice.percussion)
                    {
                        const int pad = drumPadForPitch(note.pitch);
                        if (pad >= 0)
                        {
                            const DrumPadSample& padSample = track.drumSamples[static_cast<size_t>(pad)];
                            voice.attackFrames = std::max<qint64>(0, static_cast<qint64>(std::llround(
                                padSample.attackSeconds * sampleRate)));
                            voice.decayFrames = std::max<qint64>(0, static_cast<qint64>(std::llround(
                                padSample.decaySeconds * sampleRate)));
                            voice.releaseFrames = std::max<qint64>(0, static_cast<qint64>(std::llround(
                                padSample.releaseSeconds * sampleRate)));
                            voice.sustainLevel = padSample.sustainLevel;
                        }
                        const double decaySeconds = note.pitch == 46 ? 0.72 :
                            (note.pitch == 35 || note.pitch == 36 ? 0.48 :
                             (note.pitch == 42 || note.pitch == 44 ? 0.22 : 0.36));
                        voice.endFrame = voice.startFrame + static_cast<qint64>(sampleRate * decaySeconds);
                    }
                    notes.push_back(voice);
                }
            }
        }
    }

    recomputeLatencyCompensation();

    std::sort(notes.begin(), notes.end(), [](const VoiceNote& left, const VoiceNote& right)
    {
        return left.startFrame < right.startFrame;
    });
    std::stable_sort(pitchBendEvents.begin(), pitchBendEvents.end(),
        [](const PitchBendEvent& left, const PitchBendEvent& right)
        {
            if (left.frame != right.frame) return left.frame < right.frame;
            if (left.trackIndex != right.trackIndex) return left.trackIndex < right.trackIndex;
            return left.channel < right.channel;
        });
    std::stable_sort(pluginInstrumentEvents.begin(), pluginInstrumentEvents.end(),
        [](const PluginInstrumentEvent& left, const PluginInstrumentEvent& right)
        {
            if (left.frame != right.frame) return left.frame < right.frame;
            if (left.trackIndex != right.trackIndex) return left.trackIndex < right.trackIndex;
            if (left.type != right.type)
                return left.type == PluginInstrumentEvent::Type::NoteOff;
            return left.noteId < right.noteId;
        });
    std::sort(audioRegions.begin(), audioRegions.end(), [](const AudioRegion& left, const AudioRegion& right)
    {
        return left.startFrame < right.startFrame;
    });
}

void SynthRenderer::recomputeLatencyCompensation()
{
    if (!project)
        return;

    const auto effectChainBufferingLatency = [](const std::vector<EffectState>& effects)
    {
        qint64 frames = 0;
        for (const EffectState& effect : effects)
        {
            if (effect.lv2Effect != nullptr)
                frames += effect.lv2Effect->processingLatencyFrames();
            if (effect.clapEffect != nullptr)
                frames += effect.clapEffect->processingLatencyFrames();
            if (effect.vst3Effect != nullptr)
                frames += effect.vst3Effect->processingLatencyFrames();
        }
        return frames;
    };
    std::vector<qint64> trackSignalLatencies(trackEffectStates.size(), 0);
    std::vector<qint64> busInputLatencies(busEffectStates.size(), 0);
    std::vector<qint64> busSignalLatencies(busEffectStates.size(), 0);
    for (size_t trackIndex = 0; trackIndex < trackSignalLatencies.size(); ++trackIndex)
    {
        if (trackIndex < clapInstruments.size() && clapInstruments[trackIndex] != nullptr)
        {
            const qint64 instrumentLatency =
                clapInstruments[trackIndex]->processingLatencyFrames();
            trackInstrumentSourceLatencyDelays[trackIndex].configure(instrumentLatency);
            trackSignalLatencies[trackIndex] += instrumentLatency;
        }
        if (trackIndex < vst3Instruments.size() && vst3Instruments[trackIndex] != nullptr)
        {
            const qint64 instrumentLatency = vst3Instruments[trackIndex]->processingLatencyFrames();
            trackInstrumentSourceLatencyDelays[trackIndex].configure(instrumentLatency);
            trackSignalLatencies[trackIndex] += instrumentLatency;
        }
        trackSignalLatencies[trackIndex] +=
            effectChainBufferingLatency(trackEffectStates[trackIndex]);
    }

    qint64 masterInputLatency = 0;
    for (size_t trackIndex = 0; trackIndex < trackSignalLatencies.size(); ++trackIndex)
    {
        const int outputBus = trackBusIndices[trackIndex];
        if (outputBus >= 0 && static_cast<size_t>(outputBus) < busInputLatencies.size())
            busInputLatencies[static_cast<size_t>(outputBus)] = std::max(
                busInputLatencies[static_cast<size_t>(outputBus)],
                trackSignalLatencies[trackIndex]);
        else
            masterInputLatency = std::max(masterInputLatency, trackSignalLatencies[trackIndex]);

        for (TrackSendMix& send : trackSends[trackIndex])
            if (send.busIndex >= 0 && static_cast<size_t>(send.busIndex) < busInputLatencies.size())
                busInputLatencies[static_cast<size_t>(send.busIndex)] = std::max(
                    busInputLatencies[static_cast<size_t>(send.busIndex)],
                    trackSignalLatencies[trackIndex]);
    }
    for (const size_t busIndex : busRouteOrder)
    {
        busSignalLatencies[busIndex] = busInputLatencies[busIndex] +
            effectChainBufferingLatency(busEffectStates[busIndex]);
        const int parentBus = busOutputIndices[busIndex];
        if (parentBus >= 0 && static_cast<size_t>(parentBus) < busInputLatencies.size())
            busInputLatencies[static_cast<size_t>(parentBus)] = std::max(
                busInputLatencies[static_cast<size_t>(parentBus)], busSignalLatencies[busIndex]);
        else
            masterInputLatency = std::max(masterInputLatency, busSignalLatencies[busIndex]);

        for (TrackSendMix& send : busSends[busIndex])
            if (send.busIndex >= 0 && static_cast<size_t>(send.busIndex) < busInputLatencies.size())
                busInputLatencies[static_cast<size_t>(send.busIndex)] = std::max(
                    busInputLatencies[static_cast<size_t>(send.busIndex)],
                    busSignalLatencies[busIndex]);
    }

    for (size_t trackIndex = 0; trackIndex < trackSignalLatencies.size(); ++trackIndex)
    {
        const int outputBus = trackBusIndices[trackIndex];
        const qint64 destinationLatency = outputBus >= 0 &&
                static_cast<size_t>(outputBus) < busInputLatencies.size()
            ? busInputLatencies[static_cast<size_t>(outputBus)] : masterInputLatency;
        trackMainLatencyDelays[trackIndex].configure(
            std::max<qint64>(0, destinationLatency - trackSignalLatencies[trackIndex]));
        for (TrackSendMix& send : trackSends[trackIndex])
            if (send.busIndex >= 0 && static_cast<size_t>(send.busIndex) < busInputLatencies.size())
                send.latencyDelay.configure(std::max<qint64>(0,
                    busInputLatencies[static_cast<size_t>(send.busIndex)] -
                    trackSignalLatencies[trackIndex]));
    }
    for (size_t busIndex = 0; busIndex < busSignalLatencies.size(); ++busIndex)
    {
        const int parentBus = busOutputIndices[busIndex];
        const qint64 destinationLatency = parentBus >= 0 &&
                static_cast<size_t>(parentBus) < busInputLatencies.size()
            ? busInputLatencies[static_cast<size_t>(parentBus)] : masterInputLatency;
        busMainLatencyDelays[busIndex].configure(
            std::max<qint64>(0, destinationLatency - busSignalLatencies[busIndex]));
        for (TrackSendMix& send : busSends[busIndex])
            if (send.busIndex >= 0 && static_cast<size_t>(send.busIndex) < busInputLatencies.size())
                send.latencyDelay.configure(std::max<qint64>(0,
                    busInputLatencies[static_cast<size_t>(send.busIndex)] -
                    busSignalLatencies[busIndex]));
    }
}

void SynthRenderer::resetTransport()
{
    for (const std::unique_ptr<ClapAudioPluginInstance>& instrument : clapInstruments)
    {
        if (instrument == nullptr)
            continue;
        instrument->resetProcessingState();
        instrument->allNotesOff();
    }
    for (const std::unique_ptr<Vst3AudioInstrumentInstance>& instrument : vst3Instruments)
    {
        if (instrument == nullptr)
            continue;
        instrument->resetProcessingState();
        instrument->allNotesOff();
    }
    activeNotes.clear();
    activeAudioRegions.clear();
    resetTrackEqualizers();
    resetLatencyCompensation();
    for (std::vector<EffectState>& effects : trackEffectStates)
        resetEffectChain(effects);
    for (size_t trackIndex = 0; trackIndex < trackInstrumentFilterStates.size(); ++trackIndex)
    {
        std::vector<EffectState>& effects = trackInstrumentFilterStates[trackIndex];
        resetEffectChain(effects);
        if (!effects.empty() && project != nullptr &&
            trackIndex < static_cast<size_t>(project->tracks.size()))
        {
            const InstrumentTrack& track = project->tracks[static_cast<qsizetype>(trackIndex)];
            effects.front().settings.cutoffHz = track.synthFilterCutoffHz;
            effects.front().settings.resonance = track.synthFilterResonance;
            configureFilter(effects.front(), track.synthFilterType);
        }
    }
    for (std::vector<EffectState>& effects : busEffectStates)
        resetEffectChain(effects);
    resetEffectChain(masterEffectStates);
    std::fill(trackEffectHasTail.begin(), trackEffectHasTail.end(), 0);
    std::fill(trackInstrumentFilterHasTail.begin(), trackInstrumentFilterHasTail.end(), 0);
    std::fill(synthFilterEnvelopeSemitoneOffsets.begin(),
              synthFilterEnvelopeSemitoneOffsets.end(), 0.0f);
    std::fill(synthFilterEnvelopeAppliedOffsets.begin(),
              synthFilterEnvelopeAppliedOffsets.end(), 0.0f);
    std::fill(busEffectHasTail.begin(), busEffectHasTail.end(), 0);
    std::fill(trackFrameSamples.begin(), trackFrameSamples.end(), StereoSample{});
    std::fill(trackFramePluginInstrumentSamples.begin(),
              trackFramePluginInstrumentSamples.end(), StereoSample{});
    std::fill(trackFrameTouched.begin(), trackFrameTouched.end(), 0);
    touchedTrackIndices.clear();
    std::fill(busFrameSamples.begin(), busFrameSamples.end(), StereoSample{});
    std::fill(lastBusOutputSamples.begin(), lastBusOutputSamples.end(), StereoSample{});
    std::fill(busFrameTouched.begin(), busFrameTouched.end(), 0);
    touchedBusIndices.clear();
    nextStartIndex = 0;
    nextPitchBendIndex = 0;
    nextPluginInstrumentEventIndex = 0;
    nextAudioStartIndex = 0;
    playbackCycle = 0;
    for (std::array<float, 16>& bends : trackPitchBends)
        bends.fill(0.0f);
    mixerFrameNumber = 0;
    lastFrame = -1;
}

void SynthRenderer::beginMixerFrame()
{
    std::fill(lastBusOutputSamples.begin(), lastBusOutputSamples.end(), StereoSample{});
    std::fill(synthFilterEnvelopeSemitoneOffsets.begin(),
              synthFilterEnvelopeSemitoneOffsets.end(), 0.0f);
    ++mixerFrameNumber;
}

void SynthRenderer::setTransportContext(qint64 projectFrame, bool playing, bool cycleActive)
{
    currentProjectFrame = projectFrame;
    transportPlaying = playing;
    transportCycleActive = cycleActive && playing;
    transportProjectBeat = project == nullptr ? 0.0 :
        project->beatAtFrame(projectFrame, static_cast<int>(sampleRate));
    if (!std::isfinite(transportProjectBeat))
        transportProjectBeat = 0.0;
    currentBeat = std::clamp(transportProjectBeat, 0.0, 16384.0);
}

void SynthRenderer::updateBusAudibility()
{
    if (project == nullptr)
        return;
    bool changed = !busAudibilityInitialized;
    hasBusSolo = false;
    for (size_t index = 0; index < static_cast<size_t>(project->buses.size()); ++index)
    {
        const MixerBus& bus = project->buses[static_cast<qsizetype>(index)];
        const bool muted = automationValue(bus.muteAutomation, currentBeat, bus.muted ? 1.0f : 0.0f) >= 0.5f;
        const bool solo = automationValue(bus.soloAutomation, currentBeat, bus.solo ? 1.0f : 0.0f) >= 0.5f;
        const unsigned char mutedValue = muted ? 1 : 0;
        const unsigned char soloValue = solo ? 1 : 0;
        changed = changed || busMutedAtBeat[index] != mutedValue || busSoloAtBeat[index] != soloValue;
        busMutedAtBeat[index] = mutedValue;
        busSoloAtBeat[index] = soloValue;
        hasBusSolo = hasBusSolo || solo;
    }
    if (!changed)
        return;

    std::fill(busInputAudible.begin(), busInputAudible.end(), 0);
    std::fill(busSoloDownstreamAudible.begin(), busSoloDownstreamAudible.end(), 0);
    std::fill(busOutputAudible.begin(), busOutputAudible.end(), 0);
    for (size_t busIndex = 0; busIndex < static_cast<size_t>(project->buses.size()); ++busIndex)
    {
        if (!hasBusSolo)
        {
            busInputAudible[busIndex] = busMutedAtBeat[busIndex] == 0 ? 1 : 0;
            busSoloDownstreamAudible[busIndex] = busInputAudible[busIndex];
            continue;
        }
        if (busMutedAtBeat[busIndex] != 0)
            continue;
        std::vector<size_t> pending{busIndex};
        std::vector<unsigned char> visited(busOutputIndices.size(), 0);
        while (!pending.empty())
        {
            const size_t routeIndex = pending.back();
            pending.pop_back();
            if (routeIndex >= visited.size() || visited[routeIndex] != 0)
                continue;
            visited[routeIndex] = 1;
            if (busMutedAtBeat[routeIndex] != 0)
                continue;
            if (busSoloAtBeat[routeIndex] != 0)
            {
                busInputAudible[busIndex] = 1;
                break;
            }
            const int parentIndex = busOutputIndices[routeIndex];
            if (parentIndex >= 0)
                pending.push_back(static_cast<size_t>(parentIndex));
            for (const TrackSendMix& send : busSends[routeIndex])
                if (send.busIndex >= 0)
                    pending.push_back(static_cast<size_t>(send.busIndex));
        }
    }
    if (hasBusSolo)
    {
        for (size_t busIndex = 0; busIndex < static_cast<size_t>(project->buses.size()); ++busIndex)
        {
            if (busSoloAtBeat[busIndex] == 0 || busMutedAtBeat[busIndex] != 0)
                continue;
            std::vector<size_t> pending{busIndex};
            while (!pending.empty())
            {
                const size_t routeIndex = pending.back();
                pending.pop_back();
                if (routeIndex >= busSoloDownstreamAudible.size() ||
                    busSoloDownstreamAudible[routeIndex] != 0 || busMutedAtBeat[routeIndex] != 0)
                    continue;
                busSoloDownstreamAudible[routeIndex] = 1;
                const int parentIndex = busOutputIndices[routeIndex];
                if (parentIndex >= 0)
                    pending.push_back(static_cast<size_t>(parentIndex));
                for (const TrackSendMix& send : busSends[routeIndex])
                    if (send.busIndex >= 0)
                        pending.push_back(static_cast<size_t>(send.busIndex));
            }
        }
    }
    for (size_t busIndex = 0; busIndex < busOutputAudible.size(); ++busIndex)
        busOutputAudible[busIndex] = busInputAudible[busIndex] != 0 ||
            busSoloDownstreamAudible[busIndex] != 0 ? 1 : 0;
    busAudibilityInitialized = true;
}

Vst3TransportContext SynthRenderer::currentVst3TransportContext() const
{
    Vst3TransportContext context;
    context.projectTimeSamples = currentProjectFrame;
    context.projectTimeMusic = transportProjectBeat;
    context.sampleRate = sampleRate;
    context.playing = transportPlaying;
    context.cycleActive = transportCycleActive;
    if (project != nullptr)
    {
        context.tempo = project->tempoAtBeat(transportProjectBeat);
        context.timeSigNumerator = project->beatsPerBar;
        context.timeSigDenominator = project->beatUnit;
        context.cycleEndMusic = project->songLengthBeats();
    }
    return context;
}

StereoSample SynthRenderer::addLiveTrackSample(size_t trackIndex, StereoSample input)
{
    if (trackIndex >= trackAudible.size() || trackAudible[trackIndex] == 0)
        return {};
    addTrackSample(trackIndex, input.left, input.right);
    return {};
}

void SynthRenderer::setSynthFilterEnvelope(size_t trackIndex, float semitoneOffset)
{
    if (trackIndex >= synthFilterEnvelopeSemitoneOffsets.size() || !std::isfinite(semitoneOffset))
        return;
    float& current = synthFilterEnvelopeSemitoneOffsets[trackIndex];
    if (std::abs(semitoneOffset) > std::abs(current))
        current = std::clamp(semitoneOffset, -48.0f, 48.0f);
}

bool SynthRenderer::isPluginInstrumentTrack(size_t trackIndex) const
{
    if (project == nullptr || trackIndex >= static_cast<size_t>(project->tracks.size()))
        return false;
    const InstrumentType type = project->tracks[static_cast<qsizetype>(trackIndex)].instrumentType;
    return type == InstrumentType::ClapInstrument || type == InstrumentType::Vst3Instrument;
}

void SynthRenderer::pluginInstrumentNoteOn(size_t trackIndex, std::int32_t noteId,
                                         int channel, int key, float velocity)
{
    if (trackIndex < clapInstruments.size() && clapInstruments[trackIndex] != nullptr)
        clapInstruments[trackIndex]->noteOn(noteId, channel, key, velocity);
    else if (trackIndex < vst3Instruments.size() && vst3Instruments[trackIndex] != nullptr)
    {
        vst3Instruments[trackIndex]->setTransportContext(currentVst3TransportContext());
        vst3Instruments[trackIndex]->noteOn(noteId, channel, key, velocity);
    }
}

void SynthRenderer::pluginInstrumentNoteOff(size_t trackIndex, std::int32_t noteId,
                                          int channel, int key, float velocity)
{
    if (trackIndex < clapInstruments.size() && clapInstruments[trackIndex] != nullptr)
        clapInstruments[trackIndex]->noteOff(noteId, channel, key, velocity);
    else if (trackIndex < vst3Instruments.size() && vst3Instruments[trackIndex] != nullptr)
    {
        vst3Instruments[trackIndex]->setTransportContext(currentVst3TransportContext());
        vst3Instruments[trackIndex]->noteOff(noteId, channel, key, velocity);
    }
}

void SynthRenderer::pluginInstrumentAllNotesOff(size_t trackIndex)
{
    if (trackIndex < clapInstruments.size() && clapInstruments[trackIndex] != nullptr)
        clapInstruments[trackIndex]->allNotesOff();
    if (trackIndex < vst3Instruments.size() && vst3Instruments[trackIndex] != nullptr)
    {
        vst3Instruments[trackIndex]->setTransportContext(currentVst3TransportContext());
        vst3Instruments[trackIndex]->allNotesOff();
    }
}

void SynthRenderer::pluginInstrumentPitchBend(size_t trackIndex, int channel, int value)
{
    if (trackIndex < clapInstruments.size() && clapInstruments[trackIndex] != nullptr)
        clapInstruments[trackIndex]->pitchBend(channel, value);
    if (trackIndex < vst3Instruments.size() && vst3Instruments[trackIndex] != nullptr)
        vst3Instruments[trackIndex]->pitchBend(channel, value);
}

void SynthRenderer::pluginInstrumentNotePressure(size_t trackIndex, std::int32_t noteId,
                                                 int channel, int key, float value)
{
    if (trackIndex < clapInstruments.size() && clapInstruments[trackIndex] != nullptr)
        clapInstruments[trackIndex]->notePressure(noteId, channel, key, value);
}

void SynthRenderer::pluginInstrumentsAllNotesOff()
{
    for (const std::unique_ptr<ClapAudioPluginInstance>& instrument : clapInstruments)
        if (instrument != nullptr)
            instrument->allNotesOff();
    for (const std::unique_ptr<Vst3AudioInstrumentInstance>& instrument : vst3Instruments)
        if (instrument != nullptr)
        {
            instrument->setTransportContext(currentVst3TransportContext());
            instrument->allNotesOff();
        }
}

StereoSample SynthRenderer::finishMixerFrame()
{
    StereoSample masterSample;

    for (size_t trackIndex = 0; trackIndex < clapInstruments.size(); ++trackIndex)
    {
        ClapAudioPluginInstance* instrument = clapInstruments[trackIndex].get();
        if (instrument == nullptr || !instrument->isValid() || trackIndex >= trackAudible.size() ||
            trackAudible[trackIndex] == 0)
            continue;
        const InstrumentTrack& track = project->tracks[static_cast<qsizetype>(trackIndex)];
        const std::vector<float>& liveOverrides =
            pluginInstrumentLiveParameterOverrides[trackIndex];
        static const QVector<AutomationPoint> empty;
        for (qsizetype parameterIndex = 0; parameterIndex < instrument->parameterCount();
             ++parameterIndex)
        {
            const QVector<AutomationPoint>& points =
                parameterIndex < track.clapParameterAutomation.size()
                    ? track.clapParameterAutomation[parameterIndex] : empty;
            const float liveValue = static_cast<size_t>(parameterIndex) < liveOverrides.size()
                ? liveOverrides[static_cast<size_t>(parameterIndex)]
                : std::numeric_limits<float>::quiet_NaN();
            if (points.isEmpty() && !std::isfinite(liveValue))
                continue;
            const float baseValue = parameterIndex < track.clapParameterValues.size()
                ? track.clapParameterValues[parameterIndex]
                : instrument->parameterValue(parameterIndex);
            float value = automationValue(points, currentBeat, baseValue);
            if (std::isfinite(liveValue))
                value = liveValue;
            instrument->setParameter(parameterIndex, value);
        }
        float left = 0.0f;
        float right = 0.0f;
        if (instrument->process(left, right))
            addTrackSample(trackIndex, left, right, true);
    }

    for (size_t trackIndex = 0; trackIndex < vst3Instruments.size(); ++trackIndex)
    {
        Vst3AudioInstrumentInstance* instrument = vst3Instruments[trackIndex].get();
        if (instrument == nullptr || !instrument->isValid() || trackIndex >= trackAudible.size() ||
            trackAudible[trackIndex] == 0)
            continue;
        const InstrumentTrack& track = project->tracks[static_cast<qsizetype>(trackIndex)];
        const std::vector<float>& liveOverrides =
            pluginInstrumentLiveParameterOverrides[trackIndex];
        const std::vector<float>& manualOverrides =
            vst3InstrumentManualParameterOverrides[trackIndex];
        static const QVector<AutomationPoint> empty;
        for (qsizetype parameterIndex = 0; parameterIndex < instrument->parameterCount();
             ++parameterIndex)
        {
            const QVector<AutomationPoint>& points =
                parameterIndex < track.vst3ParameterAutomation.size()
                    ? track.vst3ParameterAutomation[parameterIndex] : empty;
            const float liveValue = static_cast<size_t>(parameterIndex) < liveOverrides.size()
                ? liveOverrides[static_cast<size_t>(parameterIndex)]
                : std::numeric_limits<float>::quiet_NaN();
            const float manualValue = static_cast<size_t>(parameterIndex) < manualOverrides.size()
                ? manualOverrides[static_cast<size_t>(parameterIndex)]
                : std::numeric_limits<float>::quiet_NaN();
            if (points.isEmpty() && !std::isfinite(liveValue) && !std::isfinite(manualValue))
                continue;
            const float baseValue = parameterIndex < track.vst3ParameterValues.size()
                ? track.vst3ParameterValues[parameterIndex]
                : instrument->parameterValue(parameterIndex);
            float value = automationValue(points, currentBeat, baseValue);
            if (points.isEmpty() && std::isfinite(manualValue))
                value = manualValue;
            if (std::isfinite(liveValue))
                value = liveValue;
            instrument->setParameter(parameterIndex, value);
        }
        float left = 0.0f;
        float right = 0.0f;
        if (instrument->process(left, right, currentVst3TransportContext()))
            addTrackSample(trackIndex, left, right, true);
    }

    for (size_t trackIndex = 0; trackIndex < trackEffectHasTail.size(); ++trackIndex)
    {
        bool hasLatencyTail = trackIndex < trackMainLatencyDelays.size() &&
            trackMainLatencyDelays[trackIndex].hasPending();
        if (trackIndex < trackInstrumentSourceLatencyDelays.size())
            hasLatencyTail = hasLatencyTail ||
                trackInstrumentSourceLatencyDelays[trackIndex].hasPending();
        if (trackIndex < trackSends.size())
            for (const TrackSendMix& send : trackSends[trackIndex])
                hasLatencyTail = hasLatencyTail || send.latencyDelay.hasPending();
        if ((trackEffectHasTail[trackIndex] != 0 || trackInstrumentFilterHasTail[trackIndex] != 0 ||
             hasLatencyTail) && trackIndex < trackFrameTouched.size() &&
            trackFrameTouched[trackIndex] == 0)
            addTrackSample(trackIndex, 0.0f, 0.0f);
    }
    for (const size_t trackIndex : activeEqualizerTailTracks)
    {
        if (trackIndex < trackFrameTouched.size() && !trackFrameTouched[trackIndex])
            addTrackSample(trackIndex, 0.0f, 0.0f);
    }

    for (const size_t trackIndex : touchedTrackIndices)
    {
        StereoSample trackSample = trackFrameSamples[trackIndex];
        if (trackIndex < trackInstrumentSourceLatencyDelays.size())
            trackSample = trackInstrumentSourceLatencyDelays[trackIndex].process(trackSample);
        trackSample.left += trackFramePluginInstrumentSamples[trackIndex].left;
        trackSample.right += trackFramePluginInstrumentSamples[trackIndex].right;
        if (!trackInstrumentFilterStates[trackIndex].empty())
        {
            const bool filterControlTick = (mixerFrameNumber & 0x0fU) == 0U;
            const bool envelopeChange = std::abs(
                synthFilterEnvelopeSemitoneOffsets[trackIndex] -
                synthFilterEnvelopeAppliedOffsets[trackIndex]) >= 0.1f;
            if ((filterControlTick || envelopeChange) &&
                trackIndex < static_cast<size_t>(project->tracks.size()))
            {
                const InstrumentTrack& track = project->tracks[static_cast<qsizetype>(trackIndex)];
                const float liveCutoff = liveSynthFilterCutoffOverrides[trackIndex];
                const float automatedCutoff = std::isfinite(liveCutoff) && liveCutoff >= 20.0f
                    ? liveCutoff
                    : automationValue(track.synthFilterCutoffAutomation, currentBeat,
                                      track.synthFilterCutoffHz);
                const float liveResonance = liveSynthFilterResonanceOverrides[trackIndex];
                const float automatedResonance = std::isfinite(liveResonance) &&
                    liveResonance >= 0.1f
                    ? liveResonance
                    : automationValue(track.synthFilterResonanceAutomation, currentBeat,
                                      track.synthFilterResonance);
                const double cutoff = std::clamp(
                    static_cast<double>(automatedCutoff) * std::exp2(
                        static_cast<double>(synthFilterEnvelopeSemitoneOffsets[trackIndex]) / 12.0),
                    20.0, std::max(20.0, sampleRate * 0.49));
                EffectState& filter = trackInstrumentFilterStates[trackIndex].front();
                if (std::abs(filter.settings.cutoffHz - cutoff) > 0.01 ||
                    std::abs(filter.settings.resonance - automatedResonance) > 0.001f)
                {
                    filter.settings.cutoffHz = static_cast<float>(cutoff);
                    filter.settings.resonance = automatedResonance;
                    configureFilter(filter, track.synthFilterType);
                }
                synthFilterEnvelopeAppliedOffsets[trackIndex] =
                    synthFilterEnvelopeSemitoneOffsets[trackIndex];
            }
            trackSample = processEffectChain(trackInstrumentFilterStates[trackIndex], trackSample);
            trackInstrumentFilterHasTail[trackIndex] =
                effectChainHasTail(trackInstrumentFilterStates[trackIndex]) ? 1 : 0;
        }
        trackSample = applyTrackEqualizer(trackIndex, trackSample);
        TrackEqualizerState& equalizerState = trackEqualizerStates[trackIndex];
        bool equalizerHasTail = std::max({std::abs(equalizerState.lowLeft),
            std::abs(equalizerState.lowRight), std::abs(equalizerState.upperLeft),
            std::abs(equalizerState.upperRight)}) > 1.0e-5f;
        for (const auto& bandDelays : equalizerState.parametricDelays)
            for (const ParametricEqDelay& delay : bandDelays)
                equalizerHasTail = equalizerHasTail ||
                    std::abs(delay.first) > 1.0e-5f || std::abs(delay.second) > 1.0e-5f;
        if (!equalizerState.tailActive && equalizerHasTail)
        {
            equalizerState.tailActive = true;
            activeEqualizerTailTracks.push_back(trackIndex);
        }
        trackSample = processEffectChain(trackEffectStates[trackIndex], trackSample);
        trackEffectHasTail[trackIndex] = effectChainHasTail(trackEffectStates[trackIndex]) ? 1 : 0;
        trackPeakLevels[trackIndex] = std::max(trackPeakLevels[trackIndex],
            std::max(std::abs(trackSample.left), std::abs(trackSample.right)));

        const StereoSample mainRouteSample =
            trackMainLatencyDelays[trackIndex].process(trackSample);
        const int busIndex = trackBusIndices[trackIndex];
        if (busIndex >= 0 && static_cast<size_t>(busIndex) < busFrameSamples.size())
        {
            if (busInputAudible[static_cast<size_t>(busIndex)] != 0)
                addBusFrameSample(static_cast<size_t>(busIndex), mainRouteSample);
        }
        else if (!hasBusSolo)
        {
            masterSample.left += mainRouteSample.left;
            masterSample.right += mainRouteSample.right;
        }
        for (TrackSendMix& send : trackSends[trackIndex])
        {
            const float sendGain = std::clamp(automationValue(send.automation, currentBeat,
                                                               send.gain), 0.0f, 1.0f);
            if (sendGain > 0.0f && send.busIndex >= 0 &&
                static_cast<size_t>(send.busIndex) < busFrameSamples.size() &&
                busInputAudible[static_cast<size_t>(send.busIndex)] != 0)
            {
                const StereoSample sendSample = send.latencyDelay.process(
                    {trackSample.left * sendGain, trackSample.right * sendGain});
                addBusFrameSample(static_cast<size_t>(send.busIndex),
                                  sendSample);
            }
        }
        trackFrameSamples[trackIndex] = {};
        trackFramePluginInstrumentSamples[trackIndex] = {};
        trackFrameTouched[trackIndex] = 0;
    }
    touchedTrackIndices.clear();

    auto activeTail = activeEqualizerTailTracks.begin();
    for (size_t trackIndex : activeEqualizerTailTracks)
    {
        TrackEqualizerState& state = trackEqualizerStates[trackIndex];
        const bool hasTail = std::max({std::abs(state.lowLeft), std::abs(state.lowRight),
                                       std::abs(state.upperLeft), std::abs(state.upperRight)}) > 1.0e-5f;
        state.tailActive = hasTail;
        if (hasTail)
            *activeTail++ = trackIndex;
    }
    activeEqualizerTailTracks.erase(activeTail, activeEqualizerTailTracks.end());

    for (size_t busIndex : busRouteOrder)
    {
        const bool hasInput = busFrameTouched[busIndex] != 0;
        bool hasLatencyTail = busIndex < busMainLatencyDelays.size() &&
            busMainLatencyDelays[busIndex].hasPending();
        for (const TrackSendMix& send : busSends[busIndex])
            hasLatencyTail = hasLatencyTail || send.latencyDelay.hasPending();
        if (busOutputAudible[busIndex] == 0 ||
            (!hasInput && busEffectHasTail[busIndex] == 0 && !hasLatencyTail))
        {
            busFrameSamples[busIndex] = {};
            busFrameTouched[busIndex] = 0;
            continue;
        }
        const MixerBus& bus = project->buses[static_cast<qsizetype>(busIndex)];
        const float gain = std::clamp(automationValue(bus.gainAutomation, currentBeat,
                                                      bus.gain), 0.0f, 2.0f);
        const float pan = std::clamp(automationValue(bus.panAutomation, currentBeat,
                                                      bus.pan), -1.0f, 1.0f);
        const float leftBalance = pan > 0.0f ? 1.0f - pan : 1.0f;
        const float rightBalance = pan < 0.0f ? 1.0f + pan : 1.0f;
        const StereoSample input{busFrameSamples[busIndex].left * gain * leftBalance,
                                 busFrameSamples[busIndex].right * gain * rightBalance};
        StereoSample output = processEffectChain(busEffectStates[busIndex], input);
        busEffectHasTail[busIndex] = effectChainHasTail(busEffectStates[busIndex]) ? 1 : 0;
        lastBusOutputSamples[busIndex] = output;
        busPeakLevels[busIndex] = std::max(busPeakLevels[busIndex],
            std::max(std::abs(output.left), std::abs(output.right)));

        const int parentIndex = busOutputIndices[busIndex];
        const StereoSample mainRouteSample = busMainLatencyDelays[busIndex].process(output);
        if (parentIndex >= 0)
        {
            const size_t destination = static_cast<size_t>(parentIndex);
            const bool routeAudible = !hasBusSolo
                ? busInputAudible[destination] != 0
                : ((busInputAudible[busIndex] != 0 && busInputAudible[destination] != 0) ||
                   (busSoloDownstreamAudible[busIndex] != 0 &&
                    busSoloDownstreamAudible[destination] != 0));
            if (routeAudible)
            {
                addBusFrameSample(destination, mainRouteSample);
            }
        }
        else if (busSoloDownstreamAudible[busIndex] != 0)
        {
            masterSample.left += mainRouteSample.left;
            masterSample.right += mainRouteSample.right;
        }
        for (TrackSendMix& send : busSends[busIndex])
        {
            const float sendGain = std::clamp(automationValue(send.automation, currentBeat,
                                                               send.gain), 0.0f, 1.0f);
            if (send.busIndex < 0 || static_cast<size_t>(send.busIndex) >= busSoloDownstreamAudible.size() ||
                sendGain <= 0.0f)
                continue;
            const size_t destination = static_cast<size_t>(send.busIndex);
            const bool routeAudible = !hasBusSolo
                ? busInputAudible[destination] != 0
                : ((busInputAudible[busIndex] != 0 && busInputAudible[destination] != 0) ||
                   (busSoloDownstreamAudible[busIndex] != 0 &&
                    busSoloDownstreamAudible[destination] != 0));
            if (!routeAudible)
                continue;
            const StereoSample sendSample = send.latencyDelay.process(
                {output.left * sendGain, output.right * sendGain});
            addBusFrameSample(destination, sendSample);
        }
        busFrameSamples[busIndex] = {};
        busFrameTouched[busIndex] = 0;
    }
    touchedBusIndices.clear();
    return masterSample;
}

StereoSample SynthRenderer::applyTrackEqualizer(size_t trackIndex, StereoSample input)
{
    if (trackIndex >= trackMixAutomations.size() || trackIndex >= trackMixValues.size() ||
        trackIndex >= trackEqualizerStates.size())
        return input;
    const TrackMixValue& mix = trackMixAt(trackIndex, currentProjectFrame, currentBeat);
    const std::array<float, 3>& gains = mix.equalizerGains;
    TrackEqualizerState& state = trackEqualizerStates[trackIndex];
    state.lowBandCoefficient = mix.lowBandCoefficient;
    state.highBandCoefficient = mix.highBandCoefficient;
    const TrackMixAutomation& automation = trackMixAutomations[trackIndex];
    if ((mixerFrameNumber & 0x0fU) == 0U)
    {
        for (size_t bandIndex = 0; bandIndex < automation.parametricEqAutomationActive.size();
             ++bandIndex)
        {
            if (!automation.parametricEqAutomationActive[bandIndex])
                continue;
            const ParametricEqBand& requested = mix.parametricEqBands[bandIndex];
            ParametricEqBand& previous = state.parametricBandSettings[bandIndex];
            if (std::abs(requested.frequencyHz - previous.frequencyHz) < 0.01f &&
                std::abs(requested.gainDb - previous.gainDb) < 0.001f &&
                std::abs(requested.q - previous.q) < 0.001f)
                continue;
            state.parametricCoefficients[bandIndex] = makeParametricEqCoefficients(
                requested, sampleRate);
            previous = requested;
        }
    }
    const bool legacyActive = std::abs(gains[0] - 1.0f) >= 1.0e-6f ||
                              std::abs(gains[1] - 1.0f) >= 1.0e-6f ||
                              std::abs(gains[2] - 1.0f) >= 1.0e-6f;
    const bool parametricActive = std::any_of(
        state.parametricCoefficients.cbegin(), state.parametricCoefficients.cend(),
        [](const ParametricEqCoefficients& coefficients) { return coefficients.active; });
    const bool automated = std::any_of(
        trackMixAutomations[trackIndex].equalizerGain.cbegin(),
        trackMixAutomations[trackIndex].equalizerGain.cend(),
        [](const QVector<AutomationPoint>& points) { return !points.isEmpty(); });
    const bool parametricAutomated = std::any_of(
        trackMixAutomations[trackIndex].parametricEqAutomationActive.cbegin(),
        trackMixAutomations[trackIndex].parametricEqAutomationActive.cend(),
        [](bool active) { return active; });
    if (!legacyActive && !parametricActive && !automated && !parametricAutomated)
    {
        const float lowCoefficient = state.lowBandCoefficient;
        const float highCoefficient = state.highBandCoefficient;
        const auto parametricCoefficients = state.parametricCoefficients;
        const auto parametricBandSettings = state.parametricBandSettings;
        state = {};
        state.lowBandCoefficient = lowCoefficient;
        state.highBandCoefficient = highCoefficient;
        state.parametricCoefficients = parametricCoefficients;
        state.parametricBandSettings = parametricBandSettings;
        return input;
    }

    StereoSample output = input;
    if (legacyActive)
    {
        state.lowLeft += state.lowBandCoefficient * (input.left - state.lowLeft);
        state.lowRight += state.lowBandCoefficient * (input.right - state.lowRight);
        state.upperLeft += state.highBandCoefficient * (input.left - state.upperLeft);
        state.upperRight += state.highBandCoefficient * (input.right - state.upperRight);

        const float midLeft = state.upperLeft - state.lowLeft;
        const float midRight = state.upperRight - state.lowRight;
        const float highLeft = input.left - state.upperLeft;
        const float highRight = input.right - state.upperRight;
        output = {
            state.lowLeft * gains[0] + midLeft * gains[1] + highLeft * gains[2],
            state.lowRight * gains[0] + midRight * gains[1] + highRight * gains[2]
        };
    }

    for (size_t bandIndex = 0; bandIndex < state.parametricCoefficients.size(); ++bandIndex)
    {
        const ParametricEqCoefficients& coefficients = state.parametricCoefficients[bandIndex];
        if (!coefficients.active)
            continue;
        const auto process = [&coefficients](float sample, ParametricEqDelay& delay)
        {
            const float filtered = coefficients.b0 * sample + delay.first;
            delay.first = coefficients.b1 * sample - coefficients.a1 * filtered + delay.second;
            delay.second = coefficients.b2 * sample - coefficients.a2 * filtered;
            return filtered;
        };
        output.left = process(output.left, state.parametricDelays[bandIndex][0]);
        output.right = process(output.right, state.parametricDelays[bandIndex][1]);
    }
    return output;
}

void SynthRenderer::resetTrackEqualizers()
{
    for (TrackEqualizerState& state : trackEqualizerStates)
    {
        const float lowCoefficient = state.lowBandCoefficient;
        const float highCoefficient = state.highBandCoefficient;
        const auto parametricCoefficients = state.parametricCoefficients;
        const auto parametricBandSettings = state.parametricBandSettings;
        state = {};
        state.lowBandCoefficient = lowCoefficient;
        state.highBandCoefficient = highCoefficient;
        state.parametricCoefficients = parametricCoefficients;
        state.parametricBandSettings = parametricBandSettings;
    }
    activeEqualizerTailTracks.clear();
}

void SynthRenderer::resetLatencyCompensation()
{
    for (LatencyDelayLine& delay : trackMainLatencyDelays)
        delay.reset();
    for (LatencyDelayLine& delay : trackInstrumentSourceLatencyDelays)
        delay.reset();
    for (std::vector<TrackSendMix>& sends : trackSends)
        for (TrackSendMix& send : sends)
            send.latencyDelay.reset();
    for (LatencyDelayLine& delay : busMainLatencyDelays)
        delay.reset();
    for (std::vector<TrackSendMix>& sends : busSends)
        for (TrackSendMix& send : sends)
            send.latencyDelay.reset();
}

void SynthRenderer::prepareEffectChain(const QVector<MixerEffect>& settings,
                                      std::vector<EffectState>& states)
{
    states.clear();
    states.reserve(static_cast<size_t>(settings.size()));
    constexpr std::array<double, 8> reverbDelaySeconds{
        0.0297, 0.0371, 0.0411, 0.0437, 0.0301, 0.0353, 0.0401, 0.0447
    };
    for (const MixerEffect& effect : settings)
    {
        EffectState state;
        state.settings = effect;
        if (effect.enabled && effect.wet > 0.0f && effect.type == MixerEffectType::Delay)
        {
            const size_t delayFrames = static_cast<size_t>(std::max(1.0,
                std::round(sampleRate * effect.delayMs * 0.001)));
            state.delayLeft.assign(delayFrames + 1, 0.0f);
            state.delayRight.assign(delayFrames + 1, 0.0f);
        }
        else if (effect.enabled && effect.wet > 0.0f && effect.type == MixerEffectType::Reverb)
        {
            for (size_t line = 0; line < state.reverbLines.size(); ++line)
            {
                const size_t lineFrames = static_cast<size_t>(std::max(1.0,
                    std::round(sampleRate * reverbDelaySeconds[line])));
                state.reverbLines[line].assign(lineFrames, 0.0f);
            }
        }
        else if (effect.type == MixerEffectType::LowPass)
            configureFilter(state, SynthFilterType::LowPass);
        else if (effect.enabled && effect.wet > 0.0f &&
                 effect.type == MixerEffectType::Lv2Plugin && !effect.pluginUri.isEmpty())
        {
            state.lv2Effect = std::make_unique<Lv2AudioEffectInstance>(
                effect.pluginUri, sampleRate, effect.pluginParameterValues,
                effect.pluginState);
            if (state.lv2Effect != nullptr)
                state.liveParameterOverrides.assign(
                    static_cast<size_t>(state.lv2Effect->parameterCount()),
                    std::numeric_limits<float>::quiet_NaN());
        }
        else if (effect.enabled && effect.wet > 0.0f &&
                 effect.type == MixerEffectType::ClapPlugin && !effect.pluginUri.isEmpty())
        {
            state.clapEffect = std::make_unique<ClapAudioPluginInstance>(
                effect.pluginUri, ClapPluginKind::AudioEffect, sampleRate,
                effect.pluginParameterValues,
                effect.pluginState);
            if (state.clapEffect != nullptr)
                state.liveParameterOverrides.assign(
                    static_cast<size_t>(state.clapEffect->parameterCount()),
                    std::numeric_limits<float>::quiet_NaN());
        }
        else if (effect.enabled && effect.wet > 0.0f &&
                 effect.type == MixerEffectType::Vst3Plugin && !effect.pluginUri.isEmpty())
        {
            state.vst3Effect = std::make_unique<Vst3AudioEffectInstance>(
                effect.pluginUri, sampleRate, effect.pluginParameterValues, effect.pluginState);
            if (state.vst3Effect != nullptr)
                state.liveParameterOverrides.assign(
                    static_cast<size_t>(state.vst3Effect->parameterCount()),
                    std::numeric_limits<float>::quiet_NaN());
        }
        states.push_back(std::move(state));
    }
}

void SynthRenderer::configureFilter(EffectState& state, SynthFilterType type)
{
    const double cutoff = std::clamp(static_cast<double>(state.settings.cutoffHz), 20.0,
                                     std::max(20.0, sampleRate * 0.49));
    const double omega = twoPi * cutoff / sampleRate;
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) /
        (2.0 * std::max(0.1f, state.settings.resonance));
    const double a0 = 1.0 + alpha;
    switch (type)
    {
        case SynthFilterType::HighPass:
            state.filterB0 = static_cast<float>(((1.0 + cosine) * 0.5) / a0);
            state.filterB1 = static_cast<float>(-(1.0 + cosine) / a0);
            state.filterB2 = state.filterB0;
            break;
        case SynthFilterType::BandPass:
            state.filterB0 = static_cast<float>((alpha) / a0);
            state.filterB1 = 0.0f;
            state.filterB2 = -state.filterB0;
            break;
        case SynthFilterType::Notch:
            state.filterB0 = static_cast<float>(1.0 / a0);
            state.filterB1 = static_cast<float>((-2.0 * cosine) / a0);
            state.filterB2 = state.filterB0;
            break;
        case SynthFilterType::LowPass:
        default:
            state.filterB0 = static_cast<float>(((1.0 - cosine) * 0.5) / a0);
            state.filterB1 = static_cast<float>((1.0 - cosine) / a0);
            state.filterB2 = state.filterB0;
            break;
    }
    state.filterA1 = static_cast<float>((-2.0 * cosine) / a0);
    state.filterA2 = static_cast<float>((1.0 - alpha) / a0);
}

void SynthRenderer::resetEffectChain(std::vector<EffectState>& states)
{
    for (EffectState& state : states)
    {
        if (state.clapEffect != nullptr)
            state.clapEffect->resetProcessingState();
        if (state.vst3Effect != nullptr)
            state.vst3Effect->resetProcessingState();
        std::fill(state.delayLeft.begin(), state.delayLeft.end(), 0.0f);
        std::fill(state.delayRight.begin(), state.delayRight.end(), 0.0f);
        for (std::vector<float>& line : state.reverbLines)
            std::fill(line.begin(), line.end(), 0.0f);
        state.delayIndex = 0;
        state.reverbIndices.fill(0);
        state.reverbDamped.fill(0.0f);
        state.filterX1.fill(0.0f);
        state.filterX2.fill(0.0f);
        state.filterY1.fill(0.0f);
        state.filterY2.fill(0.0f);
        state.compressorEnvelope = 0.0f;
        state.tailFramesRemaining = 0;
    }
}

bool SynthRenderer::effectChainHasTail(const std::vector<EffectState>& states) const
{
    return std::any_of(states.cbegin(), states.cend(), [](const EffectState& state)
    {
        return state.tailFramesRemaining > 0;
    });
}

StereoSample SynthRenderer::processEffectChain(std::vector<EffectState>& states, StereoSample input)
{
    StereoSample output = input;
    const Vst3TransportContext vst3Context = currentVst3TransportContext();
    const auto updateClapParameters = [this](EffectState& state, const MixerEffect& effect)
    {
        if (state.clapEffect == nullptr || !state.clapEffect->isValid())
            return;
        for (qsizetype parameterIndex = 0;
             parameterIndex < effect.pluginParameterAutomation.size(); ++parameterIndex)
        {
            const float defaultValue = parameterIndex < effect.pluginParameterValues.size()
                ? effect.pluginParameterValues[parameterIndex] : 0.0f;
            float value = automationValue(
                effect.pluginParameterAutomation[parameterIndex], currentBeat, defaultValue);
            if (static_cast<size_t>(parameterIndex) < state.liveParameterOverrides.size() &&
                std::isfinite(state.liveParameterOverrides[static_cast<size_t>(parameterIndex)]))
                value = state.liveParameterOverrides[static_cast<size_t>(parameterIndex)];
            state.clapEffect->setParameter(parameterIndex, value);
        }
        for (qsizetype parameterIndex = effect.pluginParameterAutomation.size();
             parameterIndex < static_cast<qsizetype>(state.liveParameterOverrides.size());
             ++parameterIndex)
        {
            const float value = state.liveParameterOverrides[static_cast<size_t>(parameterIndex)];
            if (std::isfinite(value))
                state.clapEffect->setParameter(parameterIndex, value);
        }
    };
    const auto updateVst3Parameters = [this](EffectState& state, const MixerEffect& effect)
    {
        if (state.vst3Effect == nullptr || !state.vst3Effect->isValid())
            return;
        for (qsizetype parameterIndex = 0;
             parameterIndex < effect.pluginParameterAutomation.size(); ++parameterIndex)
        {
            const float defaultValue = parameterIndex < effect.pluginParameterValues.size()
                ? effect.pluginParameterValues[parameterIndex] : 0.0f;
            float value = automationValue(
                effect.pluginParameterAutomation[parameterIndex], currentBeat, defaultValue);
            if (static_cast<size_t>(parameterIndex) < state.liveParameterOverrides.size() &&
                std::isfinite(state.liveParameterOverrides[static_cast<size_t>(parameterIndex)]))
                value = state.liveParameterOverrides[static_cast<size_t>(parameterIndex)];
            state.vst3Effect->setParameter(parameterIndex, value);
        }
        for (qsizetype parameterIndex = effect.pluginParameterAutomation.size();
             parameterIndex < static_cast<qsizetype>(state.liveParameterOverrides.size());
             ++parameterIndex)
        {
            const float value = state.liveParameterOverrides[static_cast<size_t>(parameterIndex)];
            if (std::isfinite(value))
                state.vst3Effect->setParameter(parameterIndex, value);
        }
    };
    for (EffectState& state : states)
    {
        const MixerEffect& effect = state.settings;
        if (effect.type == MixerEffectType::ClapPlugin)
            updateClapParameters(state, effect);
        if (effect.type == MixerEffectType::Vst3Plugin)
            updateVst3Parameters(state, effect);
        if (!effect.enabled || effect.wet <= 0.0f)
        {
            if (effect.type == MixerEffectType::ClapPlugin && state.clapEffect != nullptr &&
                state.clapEffect->isValid())
            {
                StereoSample discardedWet = output;
                float discardedDryLeft = output.left;
                float discardedDryRight = output.right;
                state.clapEffect->process(discardedWet.left, discardedWet.right,
                                          discardedDryLeft, discardedDryRight);
            }
            if (effect.type == MixerEffectType::Vst3Plugin && state.vst3Effect != nullptr &&
                state.vst3Effect->isValid())
            {
                StereoSample discardedWet = output;
                float discardedDryLeft = output.left;
                float discardedDryRight = output.right;
                state.vst3Effect->process(discardedWet.left, discardedWet.right,
                                          discardedDryLeft, discardedDryRight, vst3Context);
            }
            continue;
        }

        const StereoSample dry = output;
        StereoSample wet = dry;
        StereoSample alignedDry = dry;
        float tailLevel = 0.0f;
        if (effect.type == MixerEffectType::Delay && !state.delayLeft.empty() &&
            state.delayLeft.size() == state.delayRight.size())
        {
            const size_t ringSize = state.delayLeft.size();
            const size_t delayFrames = std::clamp(static_cast<size_t>(std::llround(
                sampleRate * effect.delayMs * 0.001)), size_t{1}, ringSize - 1);
            const size_t readIndex = (state.delayIndex + ringSize - delayFrames) % ringSize;
            const float delayedLeft = state.delayLeft[readIndex];
            const float delayedRight = state.delayRight[readIndex];
            state.delayLeft[state.delayIndex] = dry.left + delayedLeft * effect.feedback;
            state.delayRight[state.delayIndex] = dry.right + delayedRight * effect.feedback;
            state.delayIndex = (state.delayIndex + 1) % ringSize;
            wet = {delayedLeft, delayedRight};
            tailLevel = std::max(std::abs(delayedLeft), std::abs(delayedRight));
            if (effect.wet > 0.0f &&
                std::max(std::abs(dry.left), std::abs(dry.right)) > 1.0e-5f)
            {
                const double repeats = effect.feedback > 0.0f
                    ? std::ceil(std::log(0.001) / std::log(effect.feedback)) : 1.0;
                const double seconds = std::min(8.0, effect.delayMs * 0.001 * (repeats + 1.0));
                state.tailFramesRemaining = std::max(state.tailFramesRemaining,
                    static_cast<qint64>(std::ceil(seconds * sampleRate)) + 1);
            }
        }
        else if (effect.type == MixerEffectType::Reverb)
        {
            const float feedback = 0.55f + effect.roomSize * 0.4f;
            const float dampingCoefficient = 1.0f - effect.damping * 0.95f;
            float wetLeft = 0.0f;
            float wetRight = 0.0f;
            for (size_t lineIndex = 0; lineIndex < state.reverbLines.size(); ++lineIndex)
            {
                std::vector<float>& line = state.reverbLines[lineIndex];
                if (line.empty())
                    continue;
                const size_t writeIndex = state.reverbIndices[lineIndex];
                const float delayed = line[writeIndex];
                float& damped = state.reverbDamped[lineIndex];
                damped += dampingCoefficient * (delayed - damped);
                const float channelInput = lineIndex < 4 ? dry.left : dry.right;
                line[writeIndex] = channelInput + damped * feedback;
                state.reverbIndices[lineIndex] = (writeIndex + 1) % line.size();
                if (lineIndex < 4)
                    wetLeft += damped;
                else
                    wetRight += damped;
            }
            wet = {wetLeft * 0.25f, wetRight * 0.25f};
            tailLevel = std::max(std::abs(wet.left), std::abs(wet.right));
            if (effect.wet > 0.0f && std::max(std::abs(dry.left), std::abs(dry.right)) > 1.0e-5f)
            {
                const double seconds = 0.25 + effect.roomSize * 3.75;
                state.tailFramesRemaining = std::max(state.tailFramesRemaining,
                    static_cast<qint64>(std::ceil(seconds * sampleRate)));
            }
        }
        else if (effect.type == MixerEffectType::Compressor)
        {
            const float detector = std::max(std::abs(dry.left), std::abs(dry.right));
            const double attackSeconds = std::max(0.0001, effect.attackMs * 0.001);
            const double releaseSeconds = std::max(0.001, effect.releaseMs * 0.001);
            const float attackCoefficient = static_cast<float>(std::exp(-1.0 / (sampleRate * attackSeconds)));
            const float releaseCoefficient = static_cast<float>(std::exp(-1.0 / (sampleRate * releaseSeconds)));
            const float coefficient = detector > state.compressorEnvelope
                ? attackCoefficient : releaseCoefficient;
            state.compressorEnvelope = coefficient * state.compressorEnvelope +
                                       (1.0f - coefficient) * detector;
            if (detector > 1.0e-5f)
            {
                const double releaseTail = effect.releaseMs * 0.001 * 8.0;
                state.tailFramesRemaining = std::max(state.tailFramesRemaining,
                    static_cast<qint64>(std::ceil(releaseTail * sampleRate)));
            }
            const float threshold = std::pow(10.0f, effect.thresholdDb / 20.0f);
            float gain = 1.0f;
            if (state.compressorEnvelope > threshold && state.compressorEnvelope > 1.0e-8f)
            {
                const float compressed = threshold * std::pow(
                    state.compressorEnvelope / threshold, 1.0f / effect.ratio);
                gain = compressed / state.compressorEnvelope;
            }
            gain *= std::pow(10.0f, effect.makeupDb / 20.0f);
            wet = {dry.left * gain, dry.right * gain};
        }
        else if (effect.type == MixerEffectType::LowPass)
        {
            const std::array<float, 2> inputSamples{dry.left, dry.right};
            std::array<float, 2> filtered{};
            for (size_t channel = 0; channel < inputSamples.size(); ++channel)
            {
                const float inputSample = inputSamples[channel];
                const float outputSample = state.filterB0 * inputSample +
                    state.filterB1 * state.filterX1[channel] +
                    state.filterB2 * state.filterX2[channel] -
                    state.filterA1 * state.filterY1[channel] -
                    state.filterA2 * state.filterY2[channel];
                state.filterX2[channel] = state.filterX1[channel];
                state.filterX1[channel] = inputSample;
                state.filterY2[channel] = state.filterY1[channel];
                state.filterY1[channel] = outputSample;
                filtered[channel] = outputSample;
            }
            wet = {filtered[0], filtered[1]};
            tailLevel = std::max(std::abs(wet.left), std::abs(wet.right));
            if (std::max(std::abs(dry.left), std::abs(dry.right)) > 1.0e-5f)
            {
                const double cutoff = std::max(20.0, std::min<double>(effect.cutoffHz, sampleRate * 0.49));
                const double tailSeconds = std::min(4.0,
                    7.0 * effect.resonance / (3.14159265358979323846 * cutoff));
                state.tailFramesRemaining = std::max(state.tailFramesRemaining,
                    static_cast<qint64>(std::ceil(tailSeconds * sampleRate)));
            }
        }
        else if (effect.type == MixerEffectType::Lv2Plugin &&
                 state.lv2Effect != nullptr && state.lv2Effect->isValid())
        {
            for (qsizetype parameterIndex = 0;
                 parameterIndex < effect.pluginParameterAutomation.size(); ++parameterIndex)
            {
                const float defaultValue = parameterIndex < effect.pluginParameterValues.size()
                    ? effect.pluginParameterValues[parameterIndex] : 0.0f;
                float value = automationValue(
                    effect.pluginParameterAutomation[parameterIndex], currentBeat, defaultValue);
                if (static_cast<size_t>(parameterIndex) < state.liveParameterOverrides.size() &&
                    std::isfinite(state.liveParameterOverrides[static_cast<size_t>(parameterIndex)]))
                    value = state.liveParameterOverrides[static_cast<size_t>(parameterIndex)];
                state.lv2Effect->setParameter(parameterIndex, value);
            }
            for (qsizetype parameterIndex = effect.pluginParameterAutomation.size();
                 parameterIndex < static_cast<qsizetype>(state.liveParameterOverrides.size());
                 ++parameterIndex)
            {
                const float value = state.liveParameterOverrides[static_cast<size_t>(parameterIndex)];
                if (std::isfinite(value))
                    state.lv2Effect->setParameter(parameterIndex, value);
            }
            state.lv2Effect->process(wet.left, wet.right);
            if (!std::isfinite(wet.left) || !std::isfinite(wet.right))
                wet = dry;
            tailLevel = std::max(std::abs(wet.left), std::abs(wet.right));
            if (std::max(std::abs(dry.left), std::abs(dry.right)) > 1.0e-5f ||
                tailLevel > 1.0e-5f)
                state.tailFramesRemaining = std::max(state.tailFramesRemaining,
                    static_cast<qint64>(std::ceil(5.0 * sampleRate)));
        }
        else if (effect.type == MixerEffectType::ClapPlugin &&
                 state.clapEffect != nullptr && state.clapEffect->isValid())
        {
            if (!state.clapEffect->process(wet.left, wet.right,
                                           alignedDry.left, alignedDry.right))
            {
                wet = dry;
                alignedDry = dry;
            }
            if (!std::isfinite(wet.left) || !std::isfinite(wet.right))
                wet = dry;
            tailLevel = std::max(std::abs(wet.left), std::abs(wet.right));
            if (std::max(std::abs(dry.left), std::abs(dry.right)) > 1.0e-5f ||
                tailLevel > 1.0e-5f)
                state.tailFramesRemaining = std::max(state.tailFramesRemaining,
                    static_cast<qint64>(std::ceil(5.0 * sampleRate)));
        }
        else if (effect.type == MixerEffectType::Vst3Plugin &&
                 state.vst3Effect != nullptr && state.vst3Effect->isValid())
        {
            if (!state.vst3Effect->process(wet.left, wet.right,
                                           alignedDry.left, alignedDry.right, vst3Context))
            {
                wet = dry;
                alignedDry = dry;
            }
            if (!std::isfinite(wet.left) || !std::isfinite(wet.right))
                wet = dry;
            tailLevel = std::max(std::abs(wet.left), std::abs(wet.right));
            if (std::max(std::abs(dry.left), std::abs(dry.right)) > 1.0e-5f ||
                tailLevel > 1.0e-5f)
                state.tailFramesRemaining = std::max(state.tailFramesRemaining,
                    static_cast<qint64>(std::ceil(5.0 * sampleRate)));
        }

        if (state.tailFramesRemaining > 0)
            --state.tailFramesRemaining;
        if (effect.type == MixerEffectType::Delay && effect.feedback > 0.0f && tailLevel > 1.0e-5f)
        {
            const qint64 nextEchoFrames = static_cast<qint64>(std::ceil(
                sampleRate * effect.delayMs * 0.001)) + 1;
            state.tailFramesRemaining = std::max(state.tailFramesRemaining,
                nextEchoFrames);
        }
        else if (effect.type == MixerEffectType::Reverb && tailLevel > 1.0e-5f)
        {
            const double seconds = 0.25 + effect.roomSize * 3.75;
            state.tailFramesRemaining = std::max(state.tailFramesRemaining,
                static_cast<qint64>(std::ceil(seconds * sampleRate)));
        }
        else if (effect.type == MixerEffectType::LowPass && tailLevel > 1.0e-5f)
        {
            const double cutoff = std::max(20.0, std::min<double>(effect.cutoffHz, sampleRate * 0.49));
            const double tailSeconds = std::min(4.0,
                7.0 * effect.resonance / (3.14159265358979323846 * cutoff));
            state.tailFramesRemaining = std::max(state.tailFramesRemaining,
                static_cast<qint64>(std::ceil(tailSeconds * sampleRate)));
        }
        else if (effect.type == MixerEffectType::Lv2Plugin && tailLevel > 1.0e-5f)
        {
            state.tailFramesRemaining = std::max(state.tailFramesRemaining,
                static_cast<qint64>(std::ceil(5.0 * sampleRate)));
        }
        output = {alignedDry.left * (1.0f - effect.wet) + wet.left * effect.wet,
                  alignedDry.right * (1.0f - effect.wet) + wet.right * effect.wet};
    }
    return output;
}

StereoSample SynthRenderer::processMasterEffects(StereoSample input)
{
    return processEffectChain(masterEffectStates, input);
}

double SynthRenderer::effectTailSeconds() const
{
    if (!project)
        return 0.0;
    const auto chainTail = [this](const QVector<MixerEffect>& effects,
                                  const std::vector<EffectState>& states)
    {
        double seconds = 0.0;
        for (qsizetype index = 0; index < effects.size(); ++index)
        {
            const MixerEffect& effect = effects[index];
            if (!effect.enabled || effect.wet <= 0.0f)
                continue;
            if (effect.type == MixerEffectType::Delay)
            {
                const double repeats = effect.feedback > 0.0f
                    ? std::ceil(std::log(0.001) / std::log(effect.feedback)) : 1.0;
                seconds += std::min(8.0, effect.delayMs * 0.001 * (repeats + 1.0));
            }
            else if (effect.type == MixerEffectType::Reverb)
                seconds += 0.25 + effect.roomSize * 3.75;
            else if (effect.type == MixerEffectType::LowPass)
            {
                const double cutoff = std::max(20.0, std::min<double>(effect.cutoffHz, sampleRate * 0.49));
                seconds += std::min(4.0,
                    7.0 * effect.resonance / (3.14159265358979323846 * cutoff));
            }
            else if (isPluginEffect(effect.type) &&
                     static_cast<size_t>(index) < states.size() &&
                     ((states[static_cast<size_t>(index)].lv2Effect != nullptr &&
                       states[static_cast<size_t>(index)].lv2Effect->isValid()) ||
                      (states[static_cast<size_t>(index)].clapEffect != nullptr &&
                       states[static_cast<size_t>(index)].clapEffect->isValid())))
                seconds += 5.0;
        }
        return seconds;
    };
    double longestTrackTail = 0.0;
    double renderOverrunSeconds = 0.0;
    const double songEndFrame = project->frameAtBeat(project->songLengthBeats(),
                                                      static_cast<int>(sampleRate));
    for (const AudioRegion& region : audioRegions)
    {
        if (region.drumSamplePlayback)
            renderOverrunSeconds = std::max(renderOverrunSeconds,
                std::max(0.0, region.endFrame - songEndFrame) / sampleRate);
    }
    for (const VoiceNote& note : notes)
        renderOverrunSeconds = std::max(renderOverrunSeconds,
            std::max(0.0, note.endFrame + note.releaseFrames - songEndFrame) / sampleRate);
    for (const PluginInstrumentEvent& event : pluginInstrumentEvents)
        if (event.type == PluginInstrumentEvent::Type::NoteOff)
            renderOverrunSeconds = std::max(renderOverrunSeconds,
                std::max(0.0, event.frame - songEndFrame) / sampleRate);
    for (qsizetype trackIndex = 0; trackIndex < project->tracks.size(); ++trackIndex)
    {
        const InstrumentTrack& track = project->tracks[trackIndex];
        double instrumentFilterTail = 0.0;
        if (track.instrumentType == InstrumentType::Synth && track.synthFilterEnabled)
        {
            const double cutoff = std::max(20.0,
                std::min<double>(track.synthFilterCutoffHz, sampleRate * 0.49));
            instrumentFilterTail = std::min(4.0,
                7.0 * track.synthFilterResonance / (3.14159265358979323846 * cutoff));
        }
        const bool hasPluginNotes = (track.instrumentType == InstrumentType::ClapInstrument ||
                                     track.instrumentType == InstrumentType::Vst3Instrument) &&
            std::any_of(pluginInstrumentEvents.cbegin(), pluginInstrumentEvents.cend(),
                [trackIndex](const PluginInstrumentEvent& event)
                {
                    return event.trackIndex == static_cast<size_t>(trackIndex) &&
                        event.type == PluginInstrumentEvent::Type::NoteOn;
                });
        const size_t pluginTrackIndex = static_cast<size_t>(trackIndex);
        const bool pluginInstrumentValid =
            (pluginTrackIndex < clapInstruments.size() && clapInstruments[pluginTrackIndex] != nullptr &&
             clapInstruments[pluginTrackIndex]->isValid()) ||
            (pluginTrackIndex < vst3Instruments.size() && vst3Instruments[pluginTrackIndex] != nullptr &&
             vst3Instruments[pluginTrackIndex]->isValid());
        const double pluginInstrumentTail = hasPluginNotes && pluginInstrumentValid ? 5.0 : 0.0;
        longestTrackTail = std::max(longestTrackTail,
            instrumentFilterTail + pluginInstrumentTail + chainTail(track.effects,
                trackEffectStates[static_cast<size_t>(trackIndex)]));
    }
    longestTrackTail += renderOverrunSeconds;
    double busTail = 0.0;
    for (qsizetype busIndex = 0; busIndex < project->buses.size(); ++busIndex)
        busTail += chainTail(project->buses[busIndex].effects,
            busEffectStates[static_cast<size_t>(busIndex)]);
    return longestTrackTail + busTail + chainTail(project->masterEffects, masterEffectStates);
}

void SynthRenderer::setLiveTrackMixOverride(size_t trackIndex, int parameter, float value)
{
    if (trackIndex >= liveTrackMixOverrides.size())
        return;
    if (parameter == 0)
        liveTrackMixOverrides[trackIndex].volume = value;
    else if (parameter == 1)
        liveTrackMixOverrides[trackIndex].pan = value;
    else if (parameter >= trackEqLowGainAutomationTarget &&
             parameter <= trackEqHighGainAutomationTarget)
        liveTrackMixOverrides[trackIndex].equalizerGainDb[
            static_cast<size_t>(parameter - trackEqLowGainAutomationTarget)] = value;
    else if (parameter == trackEqLowCrossoverAutomationTarget)
        liveTrackMixOverrides[trackIndex].equalizerCrossoverHz[0] = value;
    else if (parameter == trackEqHighCrossoverAutomationTarget)
        liveTrackMixOverrides[trackIndex].equalizerCrossoverHz[1] = value;
    else
    {
        const int parametricEncoded = parameter - trackEqParametricAutomationTargetBase;
        if (parametricEncoded < 0 || parametricEncoded >= parametricEqBandCount * 3)
            return;
        liveTrackMixOverrides[trackIndex].parametricEqValues[
            static_cast<size_t>(parametricEncoded / 3)]
            [static_cast<size_t>(parametricEncoded % 3)] = value;
    }
}

void SynthRenderer::setLiveSynthFilterOverride(size_t trackIndex, int parameter, float value)
{
    if (parameter == 0 && trackIndex < liveSynthFilterCutoffOverrides.size())
        liveSynthFilterCutoffOverrides[trackIndex] = value;
    else if (parameter == 1 && trackIndex < liveSynthFilterResonanceOverrides.size())
        liveSynthFilterResonanceOverrides[trackIndex] = value;
}

void SynthRenderer::setLivePluginParameterOverride(size_t trackIndex, qsizetype effectIndex,
                                                   qsizetype parameterIndex, float value)
{
    if (trackIndex >= trackEffectStates.size() || parameterIndex < 0)
        return;
    if (effectIndex == -1)
    {
        if (trackIndex >= pluginInstrumentLiveParameterOverrides.size())
            return;
        std::vector<float>& overrides = pluginInstrumentLiveParameterOverrides[trackIndex];
        if (static_cast<size_t>(parameterIndex) >= overrides.size())
            return;
        overrides[static_cast<size_t>(parameterIndex)] = value;
        return;
    }
    if (effectIndex < 0 ||
        effectIndex >= static_cast<qsizetype>(trackEffectStates[trackIndex].size()))
        return;
    std::vector<float>& overrides =
        trackEffectStates[trackIndex][static_cast<size_t>(effectIndex)].liveParameterOverrides;
    if (parameterIndex < 0 || parameterIndex >= static_cast<qsizetype>(overrides.size()))
        return;
    overrides[static_cast<size_t>(parameterIndex)] = value;
}

void SynthRenderer::setLiveVst3InstrumentParameterOverride(size_t trackIndex,
                                                           qsizetype parameterIndex,
                                                           float value)
{
    if (trackIndex >= vst3InstrumentManualParameterOverrides.size() || parameterIndex < 0)
        return;
    std::vector<float>& overrides = vst3InstrumentManualParameterOverrides[trackIndex];
    if (static_cast<size_t>(parameterIndex) >= overrides.size())
        return;
    overrides[static_cast<size_t>(parameterIndex)] = value;
}

void SynthRenderer::captureEffectStates(const std::vector<EffectState>& states,
                                        QVector<MixerEffect>& target) const
{
    const size_t count = std::min(states.size(), static_cast<size_t>(target.size()));
    for (size_t index = 0; index < count; ++index)
    {
        const EffectState& state = states[index];
        MixerEffect& effect = target[static_cast<qsizetype>(index)];
        if (!isPluginEffect(effect.type) || effect.type != state.settings.type ||
            effect.pluginUri != state.settings.pluginUri)
            continue;

        QString savedState;
        const bool saved = state.lv2Effect != nullptr && state.lv2Effect->isValid()
            ? state.lv2Effect->saveState(savedState)
            : state.clapEffect != nullptr && state.clapEffect->isValid()
                ? state.clapEffect->saveState(savedState)
                : state.vst3Effect != nullptr && state.vst3Effect->isValid()
                    ? state.vst3Effect->saveState(savedState) : false;
        if (saved)
            effect.pluginState = std::move(savedState);
    }
}

void SynthRenderer::capturePluginStates(ProjectData& target) const
{
    const size_t trackCount = std::min(trackEffectStates.size(),
                                       static_cast<size_t>(target.tracks.size()));
    for (size_t index = 0; index < trackCount; ++index)
    {
        if (index < clapInstruments.size() && clapInstruments[index] != nullptr &&
            clapInstruments[index]->isValid())
        {
            QString savedState;
            if (clapInstruments[index]->saveState(savedState))
                target.tracks[static_cast<qsizetype>(index)].clapPluginState = std::move(savedState);
        }
        if (index < vst3Instruments.size() && vst3Instruments[index] != nullptr &&
            vst3Instruments[index]->isValid())
        {
            QString savedState;
            if (vst3Instruments[index]->saveState(savedState))
                target.tracks[static_cast<qsizetype>(index)].vst3PluginState = std::move(savedState);
        }
        captureEffectStates(trackEffectStates[index], target.tracks[static_cast<qsizetype>(index)].effects);
    }

    const size_t busCount = std::min(busEffectStates.size(),
                                     static_cast<size_t>(target.buses.size()));
    for (size_t index = 0; index < busCount; ++index)
        captureEffectStates(busEffectStates[index], target.buses[static_cast<qsizetype>(index)].effects);

    captureEffectStates(masterEffectStates, target.masterEffects);
}

void SynthRenderer::serviceEffectCallbacks(std::vector<EffectState>& states)
{
    for (EffectState& state : states)
        if (state.clapEffect != nullptr)
            state.clapEffect->serviceMainThread();
}

bool SynthRenderer::hasPendingVst3LatencyRestart() const
{
    for (const std::unique_ptr<Vst3AudioInstrumentInstance>& instrument : vst3Instruments)
        if (instrument != nullptr && instrument->hasPendingLatencyRestart())
            return true;
    const auto hasPendingInEffects = [](const std::vector<EffectState>& states)
    {
        return std::any_of(states.cbegin(), states.cend(), [](const EffectState& state)
        {
            return state.vst3Effect != nullptr &&
                   state.vst3Effect->hasPendingLatencyRestart();
        });
    };
    for (const std::vector<EffectState>& states : trackEffectStates)
        if (hasPendingInEffects(states))
            return true;
    for (const std::vector<EffectState>& states : busEffectStates)
        if (hasPendingInEffects(states))
            return true;
    return hasPendingInEffects(masterEffectStates);
}

bool SynthRenderer::hasPendingLv2LatencyChange() const
{
    const auto hasPendingInEffects = [](const std::vector<EffectState>& states)
    {
        return std::any_of(states.cbegin(), states.cend(), [](const EffectState& state)
        {
            return state.lv2Effect != nullptr &&
                   state.lv2Effect->hasPendingLatencyChange();
        });
    };
    for (const std::vector<EffectState>& states : trackEffectStates)
        if (hasPendingInEffects(states))
            return true;
    for (const std::vector<EffectState>& states : busEffectStates)
        if (hasPendingInEffects(states))
            return true;
    return hasPendingInEffects(masterEffectStates);
}

bool SynthRenderer::hasPendingClapRestart() const
{
    for (const std::unique_ptr<ClapAudioPluginInstance>& instrument : clapInstruments)
        if (instrument != nullptr && instrument->hasPendingRestart())
            return true;
    const auto hasPendingInEffects = [](const std::vector<EffectState>& states)
    {
        return std::any_of(states.cbegin(), states.cend(), [](const EffectState& state)
        {
            return state.clapEffect != nullptr && state.clapEffect->hasPendingRestart();
        });
    };
    for (const std::vector<EffectState>& states : trackEffectStates)
        if (hasPendingInEffects(states))
            return true;
    for (const std::vector<EffectState>& states : busEffectStates)
        if (hasPendingInEffects(states))
            return true;
    return hasPendingInEffects(masterEffectStates);
}

void SynthRenderer::servicePluginCallbacks()
{
    for (const std::unique_ptr<ClapAudioPluginInstance>& instrument : clapInstruments)
        if (instrument != nullptr)
            instrument->serviceMainThread();
    for (std::vector<EffectState>& states : trackEffectStates)
        serviceEffectCallbacks(states);
    for (std::vector<EffectState>& states : busEffectStates)
        serviceEffectCallbacks(states);
    serviceEffectCallbacks(masterEffectStates);
}

void SynthRenderer::serviceVst3LatencyRestarts()
{
    bool rebuildCompensation = false;
    for (const std::unique_ptr<Vst3AudioInstrumentInstance>& instrument : vst3Instruments)
        if (instrument != nullptr)
            rebuildCompensation = instrument->serviceLatencyRestart() || rebuildCompensation;
    const auto serviceInEffects = [&rebuildCompensation](std::vector<EffectState>& states)
    {
        for (EffectState& state : states)
            if (state.vst3Effect != nullptr)
                rebuildCompensation = state.vst3Effect->serviceLatencyRestart() ||
                    rebuildCompensation;
    };
    for (std::vector<EffectState>& states : trackEffectStates)
        serviceInEffects(states);
    for (std::vector<EffectState>& states : busEffectStates)
        serviceInEffects(states);
    serviceInEffects(masterEffectStates);
    if (rebuildCompensation)
        recomputeLatencyCompensation();
}

bool SynthRenderer::serviceLv2LatencyChanges()
{
    bool rebuildCompensation = false;
    const auto serviceInEffects = [&rebuildCompensation](std::vector<EffectState>& states)
    {
        for (EffectState& state : states)
            if (state.lv2Effect != nullptr)
                rebuildCompensation = state.lv2Effect->serviceLatencyChange() ||
                    rebuildCompensation;
    };
    for (std::vector<EffectState>& states : trackEffectStates)
        serviceInEffects(states);
    for (std::vector<EffectState>& states : busEffectStates)
        serviceInEffects(states);
    serviceInEffects(masterEffectStates);
    if (rebuildCompensation)
        recomputeLatencyCompensation();
    return rebuildCompensation;
}

bool SynthRenderer::serviceClapRestarts()
{
    bool rebuildCompensation = false;
    for (const std::unique_ptr<ClapAudioPluginInstance>& instrument : clapInstruments)
        if (instrument != nullptr)
            rebuildCompensation = instrument->serviceRestart() || rebuildCompensation;
    const auto serviceInEffects = [&rebuildCompensation](std::vector<EffectState>& states)
    {
        for (EffectState& state : states)
            if (state.clapEffect != nullptr)
                rebuildCompensation = state.clapEffect->serviceRestart() || rebuildCompensation;
    };
    for (std::vector<EffectState>& states : trackEffectStates)
        serviceInEffects(states);
    for (std::vector<EffectState>& states : busEffectStates)
        serviceInEffects(states);
    serviceInEffects(masterEffectStates);
    if (rebuildCompensation)
        recomputeLatencyCompensation();
    return rebuildCompensation;
}

void SynthRenderer::stopPluginProcessing()
{
    for (const std::unique_ptr<ClapAudioPluginInstance>& instrument : clapInstruments)
        if (instrument != nullptr)
            instrument->stopProcessing();
    for (const std::unique_ptr<Vst3AudioInstrumentInstance>& instrument : vst3Instruments)
        if (instrument != nullptr)
            instrument->stopProcessing();
    const auto stopEffectProcessing = [](std::vector<EffectState>& states)
    {
        for (EffectState& state : states)
        {
            if (state.clapEffect != nullptr)
                state.clapEffect->stopProcessing();
            if (state.vst3Effect != nullptr)
                state.vst3Effect->stopProcessing();
        }
    };
    for (std::vector<EffectState>& states : trackEffectStates)
        stopEffectProcessing(states);
    for (std::vector<EffectState>& states : busEffectStates)
        stopEffectProcessing(states);
    stopEffectProcessing(masterEffectStates);
}

void SynthRenderer::resetTrackPeakLevels()
{
    std::fill(trackPeakLevels.begin(), trackPeakLevels.end(), 0.0f);
}

float SynthRenderer::trackPeakLevel(size_t trackIndex) const
{
    return trackIndex < trackPeakLevels.size() ? trackPeakLevels[trackIndex] : 0.0f;
}

void SynthRenderer::resetBusPeakLevels()
{
    std::fill(busPeakLevels.begin(), busPeakLevels.end(), 0.0f);
}

float SynthRenderer::busPeakLevel(size_t busIndex) const
{
    return busIndex < busPeakLevels.size() ? busPeakLevels[busIndex] : 0.0f;
}

void SynthRenderer::addTrackSample(size_t trackIndex, float left, float right,
                                   bool pluginInstrumentOutput)
{
    if (trackIndex >= trackFrameSamples.size())
        return;
    if (trackFrameTouched[trackIndex] == 0)
    {
        trackFrameTouched[trackIndex] = 1;
        touchedTrackIndices.push_back(trackIndex);
    }
    StereoSample& sample = pluginInstrumentOutput
        ? trackFramePluginInstrumentSamples[trackIndex] : trackFrameSamples[trackIndex];
    sample.left += left;
    sample.right += right;
}

void SynthRenderer::addBusFrameSample(size_t busIndex, StereoSample input)
{
    if (busIndex >= busFrameSamples.size())
        return;
    if (busFrameTouched[busIndex] == 0)
    {
        busFrameTouched[busIndex] = 1;
        touchedBusIndices.push_back(busIndex);
    }
    busFrameSamples[busIndex].left += input.left;
    busFrameSamples[busIndex].right += input.right;
}

StereoSample SynthRenderer::renderSample(qint64 projectFrame)
{
    if (!project)
        return {};

    // A backwards frame means the transport restarted or wrapped at the loop boundary.
    if (lastFrame != -1 && projectFrame < lastFrame)
    {
        for (const std::unique_ptr<ClapAudioPluginInstance>& instrument : clapInstruments)
        {
            if (instrument == nullptr)
                continue;
            instrument->resetProcessingState();
            instrument->allNotesOff();
        }
        for (const std::unique_ptr<Vst3AudioInstrumentInstance>& instrument : vst3Instruments)
        {
            if (instrument == nullptr)
                continue;
            instrument->resetProcessingState();
            instrument->allNotesOff();
        }
        ++playbackCycle;
        activeNotes.clear();
        activeAudioRegions.clear();
        resetTrackEqualizers();
        resetLatencyCompensation();
        for (size_t trackIndex = 0; trackIndex < trackInstrumentFilterStates.size(); ++trackIndex)
        {
            std::vector<EffectState>& effects = trackInstrumentFilterStates[trackIndex];
            resetEffectChain(effects);
            if (!effects.empty() && trackIndex < static_cast<size_t>(project->tracks.size()))
            {
                const InstrumentTrack& track = project->tracks[static_cast<qsizetype>(trackIndex)];
                effects.front().settings.cutoffHz = track.synthFilterCutoffHz;
                configureFilter(effects.front(), track.synthFilterType);
            }
        }
        std::fill(trackInstrumentFilterHasTail.begin(), trackInstrumentFilterHasTail.end(), 0);
        std::fill(synthFilterEnvelopeAppliedOffsets.begin(),
                  synthFilterEnvelopeAppliedOffsets.end(), 0.0f);
        nextStartIndex = 0;
        nextPitchBendIndex = 0;
        nextPluginInstrumentEventIndex = 0;
        nextAudioStartIndex = 0;
        for (std::array<float, 16>& bends : trackPitchBends)
            bends.fill(0.0f);
    }
    lastFrame = projectFrame;
    setTransportContext(projectFrame, transportPlaying, transportCycleActive);
    updateBusAudibility();
    const double projectBeat = transportProjectBeat;

    StereoSample sample;
    while (nextPitchBendIndex < pitchBendEvents.size() &&
           pitchBendEvents[nextPitchBendIndex].frame <= projectFrame)
    {
        const PitchBendEvent& bend = pitchBendEvents[nextPitchBendIndex++];
        if (bend.trackIndex >= trackPitchBends.size())
            continue;
        trackPitchBends[bend.trackIndex][static_cast<size_t>(bend.channel)] = bend.semitones;
        if (bend.trackIndex < clapInstruments.size())
            pluginInstrumentPitchBend(bend.trackIndex, bend.channel, bend.value);
        const double bendRatio = std::pow(2.0, static_cast<double>(bend.semitones) / 12.0);
        for (const size_t index : activeNotes)
        {
            VoiceNote& voice = notes[index];
            if (!voice.percussion && voice.trackIndex == bend.trackIndex &&
                voice.midiChannel == bend.channel)
                voice.frequency = voice.baseFrequency * bendRatio;
        }
    }

    while (nextPluginInstrumentEventIndex < pluginInstrumentEvents.size() &&
           pluginInstrumentEvents[nextPluginInstrumentEventIndex].frame <= projectFrame)
    {
        const PluginInstrumentEvent& event = pluginInstrumentEvents[nextPluginInstrumentEventIndex++];
        if ((event.type == PluginInstrumentEvent::Type::NoteOn &&
             event.endFrame <= projectFrame) ||
            (event.type == PluginInstrumentEvent::Type::NoteOff &&
             event.frame < projectFrame) ||
            !probabilityPasses(event.probabilityPercent, event.probabilitySeed, playbackCycle))
            continue;
        if (event.type == PluginInstrumentEvent::Type::NoteOn)
            pluginInstrumentNoteOn(event.trackIndex, event.noteId, event.channel,
                                 event.key, event.velocity);
        else
            pluginInstrumentNoteOff(event.trackIndex, event.noteId, event.channel,
                                  event.key, event.velocity);
    }

    while (nextStartIndex < notes.size() && notes[nextStartIndex].startFrame <= projectFrame)
    {
        const size_t index = nextStartIndex++;
        VoiceNote& voice = notes[index];
        if (!probabilityPasses(voice.probabilityPercent, voice.probabilitySeed, playbackCycle))
            continue;
        if (voice.trackIndex < trackPitchBends.size())
        {
            const float semitones = trackPitchBends[voice.trackIndex][
                static_cast<size_t>(voice.midiChannel)];
            voice.frequency = voice.baseFrequency * std::pow(2.0,
                static_cast<double>(semitones) / 12.0);
        }
        activeNotes.push_back(index);
    }

    activeNotes.erase(std::remove_if(activeNotes.begin(), activeNotes.end(), [&](size_t index)
    {
        const VoiceNote& voice = notes[index];
        const qint64 releaseFrames = voice.filterEnvelopeEnabled
            ? std::max(voice.releaseFrames, voice.filterEnvelopeReleaseFrames)
            : voice.releaseFrames;
        return projectFrame >= voice.endFrame + releaseFrames;
    }), activeNotes.end());

    while (nextAudioStartIndex < audioRegions.size() &&
           audioRegions[nextAudioStartIndex].startFrame <= projectFrame)
    {
        const size_t index = nextAudioStartIndex++;
        const AudioRegion& region = audioRegions[index];
        if (probabilityPasses(region.probabilityPercent, region.probabilitySeed, playbackCycle))
            activeAudioRegions.push_back(index);
    }
    activeAudioRegions.erase(std::remove_if(activeAudioRegions.begin(), activeAudioRegions.end(),
        [&](size_t index) { return projectFrame >= audioRegions[index].endFrame; }), activeAudioRegions.end());

    for (const size_t index : activeNotes)
    {
        const VoiceNote& note = notes[index];
        const qint64 voiceReleaseFrames = note.filterEnvelopeEnabled
            ? std::max(note.releaseFrames, note.filterEnvelopeReleaseFrames)
            : note.releaseFrames;
        if (projectFrame < note.startFrame || projectFrame >= note.endFrame + voiceReleaseFrames)
            continue;

        const qint64 age = projectFrame - note.startFrame;
        if (note.filterEnvelopeEnabled)
        {
            const qint64 gateDuration = note.endFrame - note.startFrame;
            const float filterEnvelope = age < gateDuration
                ? envelopeLevel(age, note.filterEnvelopeAttackFrames,
                                note.filterEnvelopeDecayFrames,
                                note.filterEnvelopeSustainLevel)
                : note.filterEnvelopeReleaseFrames > 0
                    ? envelopeLevel(gateDuration, note.filterEnvelopeAttackFrames,
                                    note.filterEnvelopeDecayFrames,
                                    note.filterEnvelopeSustainLevel) * std::max(0.0f, 1.0f -
                        static_cast<float>(age - gateDuration) /
                        static_cast<float>(note.filterEnvelopeReleaseFrames))
                    : 0.0f;
            setSynthFilterEnvelope(note.trackIndex,
                note.filterEnvelopeAmountSemitones * filterEnvelope);
        }
        if (note.percussion)
        {
            const TrackMixValue& mix = trackMixAt(note.trackIndex, projectFrame, projectBeat);
            const qint64 gateDuration = note.endFrame - note.startFrame;
            const float envelope = age < gateDuration
                ? envelopeLevel(age, note.attackFrames, note.decayFrames, note.sustainLevel)
                : note.releaseFrames > 0
                    ? envelopeLevel(gateDuration, note.attackFrames, note.decayFrames,
                                    note.sustainLevel) * std::max(0.0f, 1.0f -
                        static_cast<float>(age - gateDuration) / static_cast<float>(note.releaseFrames))
                    : 0.0f;
            const float voice = renderPercussionSample(note.midiPitch, age, note.startFrame, sampleRate) *
                                note.amplitude * envelope;
            addTrackSample(note.trackIndex, voice * mix.leftGain, voice * mix.rightGain);
            continue;
        }

        const auto envelopeBeforeRelease = [&note](qint64 envelopeAge)
        {
            if (envelopeAge < note.attackFrames)
                return static_cast<float>(envelopeAge) / static_cast<float>(note.attackFrames);
            const qint64 decayAge = envelopeAge - note.attackFrames;
            if (note.decayFrames > 0 && decayAge < note.decayFrames)
            {
                const float decayProgress = static_cast<float>(decayAge) /
                                            static_cast<float>(note.decayFrames);
                return 1.0f - decayProgress * (1.0f - note.sustainLevel);
            }
            return note.sustainLevel;
        };
        const qint64 ageAtRelease = note.endFrame - note.startFrame;
        float envelope = envelopeBeforeRelease(age);
        if (projectFrame >= note.endFrame)
        {
            const qint64 releaseAge = projectFrame - note.endFrame;
            const float releaseStartLevel = envelopeBeforeRelease(ageAtRelease);
            envelope = releaseStartLevel * std::max(0.0f, 1.0f - static_cast<float>(releaseAge) /
                                                                  static_cast<float>(note.releaseFrames));
        }

        const double phase = static_cast<double>(age) * note.frequency / sampleRate;
        float oscillator = oscillatorSample(note.waveform, phase);
        if (note.secondOscillatorEnabled)
        {
            const double detuneRatio = std::pow(2.0,
                static_cast<double>(note.secondOscillatorDetuneCents) / 1200.0);
            const double secondPhase = static_cast<double>(age) * note.frequency * detuneRatio / sampleRate;
            const float secondOscillator = oscillatorSample(note.secondOscillatorWaveform, secondPhase);
            const float mix = std::clamp(note.secondOscillatorMix, 0.0f, 1.0f);
            oscillator = oscillator * (1.0f - mix) + secondOscillator * mix;
        }
        const float voice = oscillator * note.amplitude * envelope;
        const TrackMixValue& mix = trackMixAt(note.trackIndex, projectFrame, projectBeat);
        addTrackSample(note.trackIndex, voice * mix.leftGain, voice * mix.rightGain);
    }

    for (const size_t index : activeAudioRegions)
    {
        const AudioRegion& region = audioRegions[index];
        const qint64 age = projectFrame - region.startFrame;
        double sourcePosition = region.sourceStartFrame +
            static_cast<double>(age) * region.source->sampleRate * region.playbackRate / sampleRate;
        const qint64 loopStartFrame = std::clamp<qint64>(region.sourceStartFrame, 0,
                                                          region.source->frameCount() - 1);
        const qint64 sourceEndFrame = std::clamp<qint64>(
            region.sourceEndFrame < 0 ? region.source->frameCount() - 1 : region.sourceEndFrame,
            loopStartFrame, region.source->frameCount() - 1);
        if (region.drumSampleLoop)
        {
            const double loopLengthFrames = std::max<qint64>(1,
                sourceEndFrame - loopStartFrame + 1);
            sourcePosition = loopStartFrame + std::fmod(
                std::max(0.0, sourcePosition - loopStartFrame), loopLengthFrames);
        }
        const qint64 firstFrame = static_cast<qint64>(sourcePosition);
        if (firstFrame > sourceEndFrame)
            continue;
        const qint64 secondFrame = firstFrame + 1 <= sourceEndFrame
            ? firstFrame + 1
            : region.drumSampleLoop ? loopStartFrame : sourceEndFrame;
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
        float fadeEnvelope = 1.0f;
        if (region.fadeInFrames > 1)
            fadeEnvelope *= audioFadeEnvelope(region.fadeInCurve, true,
                static_cast<double>(age) / (region.fadeInFrames - 1), region.fadeInShape,
                &region.fadeInCurvePoints);
        if (region.fadeOutFrames > 0)
        {
            const qint64 remainingFadeFrames = std::max<qint64>(0, region.endFrame - projectFrame - 1);
            const qint64 fadeOutLength = std::max<qint64>(1, region.fadeOutFrames - 1);
            const double fadeOutProgress = 1.0 - std::clamp(
                static_cast<double>(remainingFadeFrames) / fadeOutLength, 0.0, 1.0);
            fadeEnvelope *= audioFadeEnvelope(region.fadeOutCurve, false, fadeOutProgress,
                                              region.fadeOutShape, &region.fadeOutCurvePoints);
        }
        if (region.drumSamplePlayback)
        {
            const qint64 gateEndAge = std::max<qint64>(0,
                region.drumGateEndFrame - region.startFrame);
            float drumEnvelope = envelopeLevel(age, region.drumAttackFrames,
                region.drumDecayFrames, region.drumSustainLevel);
            if (region.drumReleaseFrames > 0 && age >= gateEndAge)
            {
                const qint64 releaseAge = age - gateEndAge;
                const float releaseStartLevel = envelopeLevel(gateEndAge,
                    region.drumAttackFrames, region.drumDecayFrames, region.drumSustainLevel);
                drumEnvelope = releaseStartLevel * std::max(0.0f, 1.0f -
                    static_cast<float>(releaseAge) / static_cast<float>(region.drumReleaseFrames));
            }
            fadeEnvelope *= drumEnvelope;
        }
        const TrackMixValue& mix = trackMixAt(region.trackIndex, projectFrame, projectBeat);
        addTrackSample(region.trackIndex,
                       left * region.sourceGain * mix.leftGain * fadeEnvelope,
                       right * region.sourceGain * mix.rightGain * fadeEnvelope);
    }

    const qint64 countInTicks = static_cast<qint64>(project->countInBars) * project->beatsPerBar;
    const double countInBeatOffset = static_cast<double>(countInTicks) * 4.0 / project->beatUnit;
    const double metronomeBeat = projectBeat + countInBeatOffset;
    const qint64 tick = static_cast<qint64>(std::floor(
        metronomeBeat * project->beatUnit / 4.0 + 1.0e-9));
    const double clickBeat = static_cast<double>(tick) * 4.0 / project->beatUnit - countInBeatOffset;
    const qint64 clickStartFrame = project->frameAtBeat(clickBeat, static_cast<int>(sampleRate));
    const qint64 age = projectFrame - clickStartFrame;
    if (age >= 0 && (project->metronomeEnabled || projectFrame < 0))
    {
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
