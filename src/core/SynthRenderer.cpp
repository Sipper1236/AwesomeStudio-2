#include "SynthRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
    notes.clear();
    pitchBendEvents.clear();
    trackPitchBends.clear();
    activeNotes.clear();
    audioRegions.clear();
    activeAudioRegions.clear();
    trackMixAutomations.clear();
    trackMixValues.clear();
    liveTrackMixOverrides.clear();
    trackEqualizerGains.clear();
    trackEqualizerStates.clear();
    trackInstrumentFilterStates.clear();
    trackInstrumentFilterHasTail.clear();
    trackEffectStates.clear();
    trackEffectHasTail.clear();
    activeEqualizerTailTracks.clear();
    trackBusIndices.clear();
    trackSends.clear();
    trackAudible.clear();
    busMixStates.clear();
    busEffectStates.clear();
    busEffectHasTail.clear();
    busSends.clear();
    busOutputIndices.clear();
    busInputAudible.clear();
    busOutputAudible.clear();
    busSoloDownstreamAudible.clear();
    busRouteOrder.clear();
    busFrameSamples.clear();
    lastBusOutputSamples.clear();
    busFrameTouched.clear();
    touchedBusIndices.clear();
    busPeakLevels.clear();
    trackFrameSamples.clear();
    trackFrameTouched.clear();
    touchedTrackIndices.clear();
    trackPeakLevels.clear();
    hasBusSolo = false;
    nextStartIndex = 0;
    nextPitchBendIndex = 0;
    nextAudioStartIndex = 0;
    playbackCycle = 0;
    lastFrame = -1;
    if (!project)
        return;

    trackMixAutomations.reserve(static_cast<size_t>(project->tracks.size()));
    trackMixValues.resize(static_cast<size_t>(project->tracks.size()));
    liveTrackMixOverrides.resize(static_cast<size_t>(project->tracks.size()));
    trackPitchBends.assign(static_cast<size_t>(project->tracks.size()), {});
    trackEqualizerGains.reserve(static_cast<size_t>(project->tracks.size()));
    trackEqualizerStates.resize(static_cast<size_t>(project->tracks.size()));
    trackInstrumentFilterStates.resize(static_cast<size_t>(project->tracks.size()));
    trackInstrumentFilterHasTail.assign(static_cast<size_t>(project->tracks.size()), 0);
    trackEffectStates.resize(static_cast<size_t>(project->tracks.size()));
    trackEffectHasTail.assign(static_cast<size_t>(project->tracks.size()), 0);
    trackBusIndices.resize(static_cast<size_t>(project->tracks.size()), -1);
    trackSends.resize(static_cast<size_t>(project->tracks.size()));
    trackAudible.assign(static_cast<size_t>(project->tracks.size()), 0);
    busMixStates.reserve(static_cast<size_t>(project->buses.size()));
    busEffectStates.resize(static_cast<size_t>(project->buses.size()));
    busEffectHasTail.assign(static_cast<size_t>(project->buses.size()), 0);
    busSends.resize(static_cast<size_t>(project->buses.size()));
    busOutputIndices.assign(static_cast<size_t>(project->buses.size()), -1);
    busInputAudible.assign(static_cast<size_t>(project->buses.size()), 0);
    busOutputAudible.assign(static_cast<size_t>(project->buses.size()), 0);
    busSoloDownstreamAudible.assign(static_cast<size_t>(project->buses.size()), 0);
    busRouteOrder.resize(static_cast<size_t>(project->buses.size()));
    busFrameSamples.resize(static_cast<size_t>(project->buses.size()));
    lastBusOutputSamples.resize(static_cast<size_t>(project->buses.size()));
    busFrameTouched.assign(static_cast<size_t>(project->buses.size()), 0);
    touchedBusIndices.reserve(static_cast<size_t>(project->buses.size()));
    busPeakLevels.assign(static_cast<size_t>(project->buses.size()), 0.0f);
    trackFrameSamples.resize(static_cast<size_t>(project->tracks.size()));
    trackFrameTouched.assign(static_cast<size_t>(project->tracks.size()), 0);
    touchedTrackIndices.reserve(static_cast<size_t>(project->tracks.size()));
    trackPeakLevels.assign(static_cast<size_t>(project->tracks.size()), 0.0f);
    prepareEffectChain(project->masterEffects, masterEffectStates);
    lowBandCoefficient = static_cast<float>(1.0 - std::exp(-twoPi * 120.0 / sampleRate));
    highBandCoefficient = static_cast<float>(1.0 - std::exp(-twoPi * 6000.0 / sampleRate));
    for (size_t trackIndex = 0; trackIndex < static_cast<size_t>(project->tracks.size()); ++trackIndex)
    {
        const InstrumentTrack& track = project->tracks[static_cast<qsizetype>(trackIndex)];
        trackMixAutomations.push_back({track.gain, track.pan,
                                       track.volumeAutomation, track.panAutomation});
        trackEqualizerGains.push_back({
            static_cast<float>(std::pow(10.0, track.eqLowDb / 20.0)),
            static_cast<float>(std::pow(10.0, track.eqMidDb / 20.0)),
            static_cast<float>(std::pow(10.0, track.eqHighDb / 20.0))
        });
        if (track.instrumentType == InstrumentType::Synth && track.synthFilterEnabled)
        {
            MixerEffect instrumentFilter = defaultMixerEffect(MixerEffectType::LowPass);
            instrumentFilter.cutoffHz = track.synthFilterCutoffHz;
            instrumentFilter.resonance = track.synthFilterResonance;
            QVector<MixerEffect> instrumentFilterChain{instrumentFilter};
            prepareEffectChain(instrumentFilterChain, trackInstrumentFilterStates[trackIndex]);
        }
        prepareEffectChain(track.effects, trackEffectStates[trackIndex]);
    }

    std::unordered_map<quint64, int> busIndexById;
    hasBusSolo = false;
    for (size_t busIndex = 0; busIndex < static_cast<size_t>(project->buses.size()); ++busIndex)
    {
        const MixerBus& bus = project->buses[static_cast<qsizetype>(busIndex)];
        busIndexById.emplace(bus.id, static_cast<int>(busIndex));
        hasBusSolo = hasBusSolo || bus.solo;
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
                busSends[busIndex].push_back({sendBus->second, send.gain});
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

    for (size_t busIndex = 0; busIndex < static_cast<size_t>(project->buses.size()); ++busIndex)
    {
        const MixerBus& bus = project->buses[static_cast<qsizetype>(busIndex)];
        if (!hasBusSolo)
        {
            busInputAudible[busIndex] = bus.muted ? 0 : 1;
            busSoloDownstreamAudible[busIndex] = busInputAudible[busIndex];
            continue;
        }

        if (bus.muted)
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
            const MixerBus& routeBus = project->buses[static_cast<qsizetype>(routeIndex)];
            if (routeBus.muted)
                continue;
            if (routeBus.solo)
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
            const MixerBus& bus = project->buses[static_cast<qsizetype>(busIndex)];
            if (!bus.solo || bus.muted)
                continue;
            std::vector<size_t> pending{busIndex};
            while (!pending.empty())
            {
                const size_t routeIndex = pending.back();
                pending.pop_back();
                if (routeIndex >= busSoloDownstreamAudible.size() ||
                    busSoloDownstreamAudible[routeIndex] != 0)
                    continue;
                const MixerBus& routeBus = project->buses[static_cast<qsizetype>(routeIndex)];
                if (routeBus.muted)
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

    bool hasTrackSolo = false;
    for (const InstrumentTrack& track : project->tracks)
        hasTrackSolo = hasTrackSolo || track.solo;

    int synthChannel = 0;
    for (size_t trackIndex = 0; trackIndex < static_cast<size_t>(project->tracks.size()); ++trackIndex)
    {
        const InstrumentTrack& track = project->tracks[static_cast<qsizetype>(trackIndex)];
        int trackMidiChannel = 9;
        if (track.instrumentType == InstrumentType::Synth)
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
                trackSends[trackIndex].push_back({sendBusPosition->second, send.gain});
        }
        const bool audible = !track.muted && (!hasTrackSolo || track.solo);
        trackAudible[trackIndex] = audible ? 1 : 0;
        if (!audible)
            continue;

        for (const MidiClip& clip : track.clips)
        {
            if (clip.audioSource != nullptr && clip.audioSource->sampleRate > 0 &&
                clip.audioSource->channelCount > 0 && clip.audioSource->frameCount() > clip.audioStartFrame)
            {
                AudioRegion region;
                region.startFrame = static_cast<qint64>(std::llround(clip.startBeat * sampleRate *
                                                                    60.0 / project->tempoBpm));
                region.sourceStartFrame = clip.audioStartFrame;
                region.fadeInFrames = static_cast<qint64>(std::llround(
                    clip.audioFadeInBeats * sampleRate * 60.0 / project->tempoBpm));
                region.fadeOutFrames = static_cast<qint64>(std::llround(
                    clip.audioFadeOutBeats * sampleRate * 60.0 / project->tempoBpm));
                region.fadeInCurve = clip.audioFadeInCurve;
                region.fadeOutCurve = clip.audioFadeOutCurve;
                region.fadeInShape = clip.audioFadeInShape;
                region.fadeOutShape = clip.audioFadeOutShape;
                region.source = clip.audioSource;
                region.trackIndex = trackIndex;
                const qint64 clipDuration = static_cast<qint64>(std::llround(
                    clip.lengthBeats * sampleRate * 60.0 / project->tempoBpm));
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
                event.frame = static_cast<qint64>(std::llround(
                    (clip.startBeat + bend.beat) * sampleRate * 60.0 / project->tempoBpm));
                event.trackIndex = trackIndex;
                event.channel = std::clamp(bend.channel, 0, 15);
                event.semitones = (static_cast<float>(std::clamp(bend.value, 0, 16383)) - 8192.0f) /
                                  8192.0f * static_cast<float>(project->pitchBendRangeSemitones);
                pitchBendEvents.push_back(event);
            }
            for (const MidiNote& note : clip.notes)
            {
                const quint64 probabilitySeed = track.id * 0x9e3779b97f4a7c15ULL ^
                    clip.id * 0xbf58476d1ce4e5b9ULL ^ note.id * 0x94d049bb133111ebULL;
                if (track.instrumentType == InstrumentType::DrumRack)
                {
                    const int pad = drumPadForPitch(note.pitch);
                    if (pad >= 0 && track.drumSamples[static_cast<size_t>(pad)].hasSampleAssignment())
                    {
                        const DrumPadSample& padSample = track.drumSamples[static_cast<size_t>(pad)];
                        const std::shared_ptr<const AudioAsset>& source =
                            padSample.sourceForVelocity(note.velocity);
                        if (source != nullptr && source->sampleRate > 0 &&
                            source->channelCount > 0 && source->frameCount() > 0)
                        {
                            AudioRegion region;
                            const qint64 noteStartFrame = static_cast<qint64>(std::llround(
                                (clip.startBeat + note.startBeat) * sampleRate * 60.0 / project->tempoBpm));
                            region.source = source;
                            region.trackIndex = trackIndex;
                            region.drumSamplePlayback = true;
                            region.drumSampleLoop = padSample.loopEnabled;
                            region.drumAttackFrames = std::max<qint64>(0,
                                static_cast<qint64>(std::llround(padSample.attackSeconds * sampleRate)));
                            region.drumDecayFrames = std::max<qint64>(0,
                                static_cast<qint64>(std::llround(padSample.decaySeconds * sampleRate)));
                            region.drumReleaseFrames = std::max<qint64>(0,
                                static_cast<qint64>(std::llround(padSample.releaseSeconds * sampleRate)));
                            region.drumSustainLevel = padSample.sustainLevel;
                            region.probabilityPercent = note.probabilityPercent;
                            region.probabilitySeed = probabilitySeed;
                            region.sourceStartFrame = static_cast<qint64>(std::llround(
                                static_cast<double>(source->frameCount() - 1) *
                                padSample.startPositionPercent / 100.0));
                            region.playbackRate = std::pow(2.0,
                                static_cast<double>(padSample.tuningSemitones) / 12.0);
                            const float velocityGain = 0.48f * static_cast<float>(note.velocity) / 127.0f;
                            region.sourceGain = velocityGain;
                            const qint64 noteDurationFrames = std::max<qint64>(1,
                                static_cast<qint64>(std::llround(
                                    note.lengthBeats * sampleRate * 60.0 / project->tempoBpm)));
                            const int ratchetCount = std::clamp(note.ratchetCount, 1, 8);
                            const qint64 remainingSourceFrames = source->frameCount() - region.sourceStartFrame;
                            const qint64 oneShotDurationFrames = static_cast<qint64>(std::ceil(
                                static_cast<double>(remainingSourceFrames) * sampleRate /
                                (source->sampleRate * region.playbackRate)));
                            for (int hit = 0; hit < ratchetCount; ++hit)
                            {
                                AudioRegion hitRegion = region;
                                const qint64 hitStartOffset = noteDurationFrames * hit / ratchetCount;
                                const qint64 hitEndOffset = noteDurationFrames * (hit + 1) / ratchetCount;
                                hitRegion.startFrame = noteStartFrame + hitStartOffset;
                                const qint64 duration = region.drumSampleLoop
                                    ? std::max<qint64>(1, hitEndOffset - hitStartOffset)
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
                const qint64 noteStartFrame = static_cast<qint64>(std::llround(
                    startBeat * sampleRate * 60.0 / project->tempoBpm));
                const qint64 noteDurationFrames = std::max<qint64>(1,
                    static_cast<qint64>(std::llround(
                        note.lengthBeats * sampleRate * 60.0 / project->tempoBpm)));
                const int ratchetCount = track.instrumentType == InstrumentType::DrumRack
                    ? std::clamp(note.ratchetCount, 1, 8) : 1;
                for (int hit = 0; hit < ratchetCount; ++hit)
                {
                    VoiceNote voice;
                    voice.startFrame = noteStartFrame + noteDurationFrames * hit / ratchetCount;
                    const qint64 hitEndOffset = noteDurationFrames * (hit + 1) / ratchetCount;
                    voice.endFrame = voice.startFrame + std::max<qint64>(
                        1, hitEndOffset - noteDurationFrames * hit / ratchetCount);
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
    std::sort(audioRegions.begin(), audioRegions.end(), [](const AudioRegion& left, const AudioRegion& right)
    {
        return left.startFrame < right.startFrame;
    });
}

void SynthRenderer::resetTransport()
{
    activeNotes.clear();
    activeAudioRegions.clear();
    resetTrackEqualizers();
    for (std::vector<EffectState>& effects : trackEffectStates)
        resetEffectChain(effects);
    for (std::vector<EffectState>& effects : trackInstrumentFilterStates)
        resetEffectChain(effects);
    for (std::vector<EffectState>& effects : busEffectStates)
        resetEffectChain(effects);
    resetEffectChain(masterEffectStates);
    std::fill(trackEffectHasTail.begin(), trackEffectHasTail.end(), 0);
    std::fill(trackInstrumentFilterHasTail.begin(), trackInstrumentFilterHasTail.end(), 0);
    std::fill(busEffectHasTail.begin(), busEffectHasTail.end(), 0);
    std::fill(trackFrameSamples.begin(), trackFrameSamples.end(), StereoSample{});
    std::fill(trackFrameTouched.begin(), trackFrameTouched.end(), 0);
    touchedTrackIndices.clear();
    std::fill(busFrameSamples.begin(), busFrameSamples.end(), StereoSample{});
    std::fill(lastBusOutputSamples.begin(), lastBusOutputSamples.end(), StereoSample{});
    std::fill(busFrameTouched.begin(), busFrameTouched.end(), 0);
    touchedBusIndices.clear();
    nextStartIndex = 0;
    nextPitchBendIndex = 0;
    nextAudioStartIndex = 0;
    playbackCycle = 0;
    for (std::array<float, 16>& bends : trackPitchBends)
        bends.fill(0.0f);
    lastFrame = -1;
}

void SynthRenderer::beginMixerFrame()
{
    std::fill(lastBusOutputSamples.begin(), lastBusOutputSamples.end(), StereoSample{});
}

void SynthRenderer::setCurrentBeat(double beat)
{
    currentBeat = std::isfinite(beat) ? std::clamp(beat, 0.0, 16384.0) : 0.0;
}

StereoSample SynthRenderer::addLiveTrackSample(size_t trackIndex, StereoSample input)
{
    if (trackIndex >= trackAudible.size() || trackAudible[trackIndex] == 0)
        return {};
    addTrackSample(trackIndex, input.left, input.right);
    return {};
}

StereoSample SynthRenderer::finishMixerFrame()
{
    StereoSample masterSample;

    for (size_t trackIndex = 0; trackIndex < trackEffectHasTail.size(); ++trackIndex)
    {
        if ((trackEffectHasTail[trackIndex] != 0 || trackInstrumentFilterHasTail[trackIndex] != 0) &&
            trackIndex < trackFrameTouched.size() && trackFrameTouched[trackIndex] == 0)
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
        if (!trackInstrumentFilterStates[trackIndex].empty())
        {
            trackSample = processEffectChain(trackInstrumentFilterStates[trackIndex], trackSample);
            trackInstrumentFilterHasTail[trackIndex] =
                effectChainHasTail(trackInstrumentFilterStates[trackIndex]) ? 1 : 0;
        }
        trackSample = applyTrackEqualizer(trackIndex, trackSample);
        TrackEqualizerState& equalizerState = trackEqualizerStates[trackIndex];
        if (!equalizerState.tailActive &&
            std::max({std::abs(equalizerState.lowLeft), std::abs(equalizerState.lowRight),
                      std::abs(equalizerState.upperLeft), std::abs(equalizerState.upperRight)}) > 1.0e-5f)
        {
            equalizerState.tailActive = true;
            activeEqualizerTailTracks.push_back(trackIndex);
        }
        trackSample = processEffectChain(trackEffectStates[trackIndex], trackSample);
        trackEffectHasTail[trackIndex] = effectChainHasTail(trackEffectStates[trackIndex]) ? 1 : 0;
        trackPeakLevels[trackIndex] = std::max(trackPeakLevels[trackIndex],
            std::max(std::abs(trackSample.left), std::abs(trackSample.right)));

        const int busIndex = trackBusIndices[trackIndex];
        if (busIndex >= 0 && static_cast<size_t>(busIndex) < busFrameSamples.size())
        {
            if (busInputAudible[static_cast<size_t>(busIndex)] != 0)
                addBusFrameSample(static_cast<size_t>(busIndex), trackSample);
        }
        else if (!hasBusSolo)
        {
            masterSample.left += trackSample.left;
            masterSample.right += trackSample.right;
        }
        for (const TrackSendMix& send : trackSends[trackIndex])
        {
            if (send.gain > 0.0f && send.busIndex >= 0 &&
                static_cast<size_t>(send.busIndex) < busFrameSamples.size() &&
                busInputAudible[static_cast<size_t>(send.busIndex)] != 0)
            {
                addBusFrameSample(static_cast<size_t>(send.busIndex),
                                  {trackSample.left * send.gain, trackSample.right * send.gain});
            }
        }
        trackFrameSamples[trackIndex] = {};
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
        if (busOutputAudible[busIndex] == 0 ||
            (!hasInput && busEffectHasTail[busIndex] == 0))
        {
            busFrameSamples[busIndex] = {};
            busFrameTouched[busIndex] = 0;
            continue;
        }
        const BusMixState& mix = busMixStates[busIndex];
        const StereoSample input{busFrameSamples[busIndex].left * mix.leftGain,
                                 busFrameSamples[busIndex].right * mix.rightGain};
        StereoSample output = processEffectChain(busEffectStates[busIndex], input);
        busEffectHasTail[busIndex] = effectChainHasTail(busEffectStates[busIndex]) ? 1 : 0;
        lastBusOutputSamples[busIndex] = output;
        busPeakLevels[busIndex] = std::max(busPeakLevels[busIndex],
            std::max(std::abs(output.left), std::abs(output.right)));

        const int parentIndex = busOutputIndices[busIndex];
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
                addBusFrameSample(destination, output);
            }
        }
        else if (busSoloDownstreamAudible[busIndex] != 0)
        {
            masterSample.left += output.left;
            masterSample.right += output.right;
        }
        for (const TrackSendMix& send : busSends[busIndex])
        {
            if (send.busIndex < 0 || static_cast<size_t>(send.busIndex) >= busSoloDownstreamAudible.size() ||
                send.gain <= 0.0f)
                continue;
            const size_t destination = static_cast<size_t>(send.busIndex);
            const bool routeAudible = !hasBusSolo
                ? busInputAudible[destination] != 0
                : ((busInputAudible[busIndex] != 0 && busInputAudible[destination] != 0) ||
                   (busSoloDownstreamAudible[busIndex] != 0 &&
                    busSoloDownstreamAudible[destination] != 0));
            if (!routeAudible)
                continue;
            addBusFrameSample(destination, {output.left * send.gain, output.right * send.gain});
        }
        busFrameSamples[busIndex] = {};
        busFrameTouched[busIndex] = 0;
    }
    touchedBusIndices.clear();
    return masterSample;
}

StereoSample SynthRenderer::applyTrackEqualizer(size_t trackIndex, StereoSample input)
{
    if (trackIndex >= trackEqualizerGains.size() || trackIndex >= trackEqualizerStates.size())
        return input;
    const std::array<float, 3>& gains = trackEqualizerGains[trackIndex];
    if (std::abs(gains[0] - 1.0f) < 1.0e-6f && std::abs(gains[1] - 1.0f) < 1.0e-6f &&
        std::abs(gains[2] - 1.0f) < 1.0e-6f)
    {
        trackEqualizerStates[trackIndex] = {};
        return input;
    }

    TrackEqualizerState& state = trackEqualizerStates[trackIndex];
    state.lowLeft += lowBandCoefficient * (input.left - state.lowLeft);
    state.lowRight += lowBandCoefficient * (input.right - state.lowRight);
    state.upperLeft += highBandCoefficient * (input.left - state.upperLeft);
    state.upperRight += highBandCoefficient * (input.right - state.upperRight);

    const float midLeft = state.upperLeft - state.lowLeft;
    const float midRight = state.upperRight - state.lowRight;
    const float highLeft = input.left - state.upperLeft;
    const float highRight = input.right - state.upperRight;
    return {
        state.lowLeft * gains[0] + midLeft * gains[1] + highLeft * gains[2],
        state.lowRight * gains[0] + midRight * gains[1] + highRight * gains[2]
    };
}

void SynthRenderer::resetTrackEqualizers()
{
    for (TrackEqualizerState& state : trackEqualizerStates)
        state = {};
    activeEqualizerTailTracks.clear();
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
        {
            const double cutoff = std::clamp(static_cast<double>(effect.cutoffHz), 20.0,
                                              std::max(20.0, sampleRate * 0.49));
            const double omega = twoPi * cutoff / sampleRate;
            const double cosine = std::cos(omega);
            const double alpha = std::sin(omega) / (2.0 * std::max(0.1f, effect.resonance));
            const double a0 = 1.0 + alpha;
            state.filterB0 = static_cast<float>(((1.0 - cosine) * 0.5) / a0);
            state.filterB1 = static_cast<float>((1.0 - cosine) / a0);
            state.filterB2 = state.filterB0;
            state.filterA1 = static_cast<float>((-2.0 * cosine) / a0);
            state.filterA2 = static_cast<float>((1.0 - alpha) / a0);
        }
        else if (effect.enabled && effect.wet > 0.0f &&
                 effect.type == MixerEffectType::Lv2Plugin && !effect.pluginUri.isEmpty())
        {
            state.lv2Effect = std::make_unique<Lv2AudioEffectInstance>(
                effect.pluginUri, sampleRate, effect.pluginParameterValues);
        }
        states.push_back(std::move(state));
    }
}

void SynthRenderer::resetEffectChain(std::vector<EffectState>& states)
{
    for (EffectState& state : states)
    {
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
    for (EffectState& state : states)
    {
        const MixerEffect& effect = state.settings;
        if (!effect.enabled || effect.wet <= 0.0f)
            continue;

        const StereoSample dry = output;
        StereoSample wet = dry;
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
                const float value = automationValue(
                    effect.pluginParameterAutomation[parameterIndex], currentBeat, defaultValue);
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
        output = {dry.left * (1.0f - effect.wet) + wet.left * effect.wet,
                  dry.right * (1.0f - effect.wet) + wet.right * effect.wet};
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
            else if (effect.type == MixerEffectType::Lv2Plugin &&
                     static_cast<size_t>(index) < states.size() &&
                     states[static_cast<size_t>(index)].lv2Effect != nullptr &&
                     states[static_cast<size_t>(index)].lv2Effect->isValid())
                seconds += 5.0;
        }
        return seconds;
    };
    double longestTrackTail = 0.0;
    double renderOverrunSeconds = 0.0;
    const double songEndFrame = project->songLengthBeats() * sampleRate * 60.0 / project->tempoBpm;
    for (const AudioRegion& region : audioRegions)
    {
        if (region.drumSamplePlayback)
            renderOverrunSeconds = std::max(renderOverrunSeconds,
                std::max(0.0, region.endFrame - songEndFrame) / sampleRate);
    }
    for (const VoiceNote& note : notes)
        renderOverrunSeconds = std::max(renderOverrunSeconds,
            std::max(0.0, note.endFrame + note.releaseFrames - songEndFrame) / sampleRate);
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
        longestTrackTail = std::max(longestTrackTail,
            instrumentFilterTail + chainTail(track.effects,
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

void SynthRenderer::addTrackSample(size_t trackIndex, float left, float right)
{
    if (trackIndex >= trackFrameSamples.size())
        return;
    if (trackFrameTouched[trackIndex] == 0)
    {
        trackFrameTouched[trackIndex] = 1;
        touchedTrackIndices.push_back(trackIndex);
    }
    StereoSample& sample = trackFrameSamples[trackIndex];
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
        ++playbackCycle;
        activeNotes.clear();
        activeAudioRegions.clear();
        resetTrackEqualizers();
        for (std::vector<EffectState>& effects : trackInstrumentFilterStates)
            resetEffectChain(effects);
        std::fill(trackInstrumentFilterHasTail.begin(), trackInstrumentFilterHasTail.end(), 0);
        nextStartIndex = 0;
        nextPitchBendIndex = 0;
        nextAudioStartIndex = 0;
        for (std::array<float, 16>& bends : trackPitchBends)
            bends.fill(0.0f);
    }
    lastFrame = projectFrame;
    const double projectBeat = static_cast<double>(projectFrame) * project->tempoBpm /
                               (60.0 * sampleRate);
    setCurrentBeat(projectBeat);

    StereoSample sample;
    while (nextPitchBendIndex < pitchBendEvents.size() &&
           pitchBendEvents[nextPitchBendIndex].frame <= projectFrame)
    {
        const PitchBendEvent& bend = pitchBendEvents[nextPitchBendIndex++];
        if (bend.trackIndex >= trackPitchBends.size())
            continue;
        trackPitchBends[bend.trackIndex][static_cast<size_t>(bend.channel)] = bend.semitones;
        const double bendRatio = std::pow(2.0, static_cast<double>(bend.semitones) / 12.0);
        for (const size_t index : activeNotes)
        {
            VoiceNote& voice = notes[index];
            if (!voice.percussion && voice.trackIndex == bend.trackIndex &&
                voice.midiChannel == bend.channel)
                voice.frequency = voice.baseFrequency * bendRatio;
        }
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
        return projectFrame >= notes[index].endFrame + notes[index].releaseFrames;
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
        if (projectFrame < note.startFrame || projectFrame >= note.endFrame + note.releaseFrames)
            continue;

        const qint64 age = projectFrame - note.startFrame;
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
        if (region.drumSampleLoop)
        {
            const double loopLengthFrames = std::max<qint64>(1,
                region.source->frameCount() - loopStartFrame);
            sourcePosition = loopStartFrame + std::fmod(
                std::max(0.0, sourcePosition - loopStartFrame), loopLengthFrames);
        }
        const qint64 firstFrame = static_cast<qint64>(sourcePosition);
        const qint64 secondFrame = firstFrame + 1 < region.source->frameCount()
            ? firstFrame + 1
            : region.drumSampleLoop ? loopStartFrame : region.source->frameCount() - 1;
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
                static_cast<double>(age) / (region.fadeInFrames - 1), region.fadeInShape);
        if (region.fadeOutFrames > 0)
        {
            const qint64 remainingFadeFrames = std::max<qint64>(0, region.endFrame - projectFrame - 1);
            const qint64 fadeOutLength = std::max<qint64>(1, region.fadeOutFrames - 1);
            const double fadeOutProgress = 1.0 - std::clamp(
                static_cast<double>(remainingFadeFrames) / fadeOutLength, 0.0, 1.0);
            fadeEnvelope *= audioFadeEnvelope(region.fadeOutCurve, false, fadeOutProgress,
                                              region.fadeOutShape);
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
