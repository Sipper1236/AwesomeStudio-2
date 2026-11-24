#include "Project.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
constexpr std::array<int, 7> supportedStepCounts = {8, 12, 16, 24, 32, 48, 64};

double tempoRampValue(double startBpm, double endBpm, double progress)
{
    return startBpm + (endBpm - startBpm) * std::clamp(progress, 0.0, 1.0);
}

double tempoRampSeconds(double beats, double startBpm, double endBpm, double rampBeats)
{
    if (beats <= 0.0)
        return 0.0;
    if (rampBeats <= 0.0)
        return beats * 60.0 / startBpm;
    const double slope = (endBpm - startBpm) / rampBeats;
    if (std::abs(slope) < 1.0e-12)
        return beats * 60.0 / startBpm;
    const double endTempo = tempoRampValue(startBpm, endBpm, beats / rampBeats);
    return 60.0 / slope * std::log(endTempo / startBpm);
}

double tempoRampBeats(double seconds, double startBpm, double endBpm, double rampBeats)
{
    if (seconds <= 0.0)
        return 0.0;
    if (rampBeats <= 0.0)
        return seconds * startBpm / 60.0;
    const double slope = (endBpm - startBpm) / rampBeats;
    if (std::abs(slope) < 1.0e-12)
        return seconds * startBpm / 60.0;
    const double tempo = startBpm * std::exp(slope * seconds / 60.0);
    return std::clamp((tempo - startBpm) / slope, 0.0, rampBeats);
}

int normalizedStepsPerBar(int requested)
{
    return *std::min_element(supportedStepCounts.cbegin(), supportedStepCounts.cend(),
                             [requested](int left, int right)
    {
        return std::abs(static_cast<long long>(left) - requested) <
               std::abs(static_cast<long long>(right) - requested);
    });
}

void normalizeAutomation(QVector<AutomationPoint>& points, float minimum, float maximum)
{
    if (points.size() > 10000)
        points.resize(10000);

    QVector<AutomationPoint> validPoints;
    validPoints.reserve(points.size());
    for (AutomationPoint point : points)
    {
        if (!std::isfinite(point.beat) || !std::isfinite(point.value))
            continue;
        point.beat = std::clamp(point.beat, 0.0, 16384.0);
        point.value = std::clamp(point.value, minimum, maximum);
        point.curveToNext = static_cast<AutomationPoint::Curve>(
            std::clamp(static_cast<int>(point.curveToNext), 0, 4));
        validPoints.push_back(point);
    }
    std::stable_sort(validPoints.begin(), validPoints.end(), [](const AutomationPoint& left,
                                                                 const AutomationPoint& right)
    {
        return left.beat < right.beat;
    });

    QVector<AutomationPoint> uniquePoints;
    uniquePoints.reserve(validPoints.size());
    for (const AutomationPoint& point : validPoints)
    {
        if (!uniquePoints.isEmpty() && std::abs(uniquePoints.last().beat - point.beat) < 1.0e-9)
            uniquePoints.last() = point;
        else
            uniquePoints.push_back(point);
    }
    points = std::move(uniquePoints);
}

void normalizeEffectChain(QVector<MixerEffect>& effects, qsizetype& remainingBudget)
{
    const qsizetype acceptedCount = std::min<qsizetype>({effects.size(), 8, remainingBudget});
    effects.resize(acceptedCount);
    remainingBudget -= acceptedCount;
    for (MixerEffect& effect : effects)
    {
        effect.type = static_cast<MixerEffectType>(
            std::clamp(static_cast<int>(effect.type), 0, 5));
        if (!std::isfinite(effect.wet)) effect.wet = 0.25f;
        if (!std::isfinite(effect.delayMs)) effect.delayMs = 250.0f;
        if (!std::isfinite(effect.feedback)) effect.feedback = 0.3f;
        if (!std::isfinite(effect.roomSize)) effect.roomSize = 0.6f;
        if (!std::isfinite(effect.damping)) effect.damping = 0.5f;
        if (!std::isfinite(effect.thresholdDb)) effect.thresholdDb = -18.0f;
        if (!std::isfinite(effect.ratio)) effect.ratio = 4.0f;
        if (!std::isfinite(effect.attackMs)) effect.attackMs = 10.0f;
        if (!std::isfinite(effect.releaseMs)) effect.releaseMs = 100.0f;
        if (!std::isfinite(effect.makeupDb)) effect.makeupDb = 0.0f;
        if (!std::isfinite(effect.cutoffHz)) effect.cutoffHz = 20000.0f;
        if (!std::isfinite(effect.resonance)) effect.resonance = 0.707f;
        effect.wet = std::clamp(effect.wet, 0.0f, 1.0f);
        effect.delayMs = std::clamp(effect.delayMs, 1.0f, 2000.0f);
        effect.feedback = std::clamp(effect.feedback, 0.0f, 0.95f);
        effect.roomSize = std::clamp(effect.roomSize, 0.0f, 1.0f);
        effect.damping = std::clamp(effect.damping, 0.0f, 1.0f);
        effect.thresholdDb = std::clamp(effect.thresholdDb, -60.0f, 0.0f);
        effect.ratio = std::clamp(effect.ratio, 1.0f, 20.0f);
        effect.attackMs = std::clamp(effect.attackMs, 0.1f, 200.0f);
        effect.releaseMs = std::clamp(effect.releaseMs, 1.0f, 2000.0f);
        effect.makeupDb = std::clamp(effect.makeupDb, -12.0f, 24.0f);
        effect.cutoffHz = std::clamp(effect.cutoffHz, 20.0f, 20000.0f);
        effect.resonance = std::clamp(effect.resonance, 0.1f, 10.0f);
        if (effect.pluginParameterValues.size() > 256)
            effect.pluginParameterValues.resize(256);
        for (float& value : effect.pluginParameterValues)
            if (!std::isfinite(value)) value = 0.0f;
        if (effect.pluginParameterAutomation.size() > 256)
            effect.pluginParameterAutomation.resize(256);
        for (QVector<AutomationPoint>& points : effect.pluginParameterAutomation)
            normalizeAutomation(points, -1.0e9f, 1.0e9f);
    }
}
}

MixerEffect defaultMixerEffect(MixerEffectType type)
{
    MixerEffect effect;
    effect.type = type;
    if (type == MixerEffectType::Compressor || type == MixerEffectType::LowPass ||
        isPluginEffect(type))
        effect.wet = 1.0f;
    return effect;
}

QVector<float> defaultAudioFadeCurvePoints()
{
    QVector<float> points;
    points.reserve(audioFadeCurvePointCount);
    for (int index = 0; index < audioFadeCurvePointCount; ++index)
        points.push_back(static_cast<float>(index) / (audioFadeCurvePointCount - 1));
    return points;
}

void normalizeAudioFadeCurvePoints(QVector<float>& points)
{
    if (points.size() != audioFadeCurvePointCount)
        points = defaultAudioFadeCurvePoints();
    for (int index = 0; index < points.size(); ++index)
    {
        if (!std::isfinite(points[index]))
            points[index] = static_cast<float>(index) / (audioFadeCurvePointCount - 1);
        points[index] = std::clamp(points[index], 0.0f, 1.0f);
    }
    points.front() = 0.0f;
    points.back() = 1.0f;
}

QVector<QPointF> defaultAudioFadeBezierHandles()
{
    return {{1.0 / 3.0, 1.0 / 3.0}, {2.0 / 3.0, 2.0 / 3.0}};
}

void normalizeAudioFadeBezierHandles(QVector<QPointF>& handles)
{
    if (handles.size() != audioFadeBezierHandleCount)
        handles = defaultAudioFadeBezierHandles();
    for (QPointF& handle : handles)
    {
        const double x = std::isfinite(handle.x()) ? handle.x() : 0.5;
        const double y = std::isfinite(handle.y()) ? handle.y() : 0.5;
        handle.setX(std::clamp(x, 0.0, 1.0));
        handle.setY(std::clamp(y, 0.0, 1.0));
    }
    if (handles[0].x() > handles[1].x())
        std::swap(handles[0], handles[1]);
}

QVector<float> audioFadeBezierCurvePoints(const QVector<QPointF>& sourceHandles)
{
    QVector<QPointF> handles = sourceHandles;
    normalizeAudioFadeBezierHandles(handles);
    const QPointF first = handles[0];
    const QPointF second = handles[1];
    const auto cubic = [](double a, double b, double t)
    {
        const double inverse = 1.0 - t;
        return 3.0 * inverse * inverse * t * a +
               3.0 * inverse * t * t * b + t * t * t;
    };

    QVector<float> points;
    points.reserve(audioFadeBezierCurvePointCount);
    for (int index = 0; index < audioFadeBezierCurvePointCount; ++index)
    {
        const double targetX = static_cast<double>(index) / (audioFadeBezierCurvePointCount - 1);
        double low = 0.0;
        double high = 1.0;
        for (int iteration = 0; iteration < 20; ++iteration)
        {
            const double parameter = (low + high) * 0.5;
            if (cubic(first.x(), second.x(), parameter) < targetX)
                low = parameter;
            else
                high = parameter;
        }
        const double parameter = (low + high) * 0.5;
        const double value = index == 0 ? 0.0 : index == audioFadeBezierCurvePointCount - 1
            ? 1.0 : cubic(first.y(), second.y(), parameter);
        points.push_back(static_cast<float>(std::clamp(value, 0.0, 1.0)));
    }
    return points;
}

float audioFadeEnvelope(AudioFadeCurve curve, bool fadeIn, double progress, double shape,
                        const QVector<float>* drawnPoints)
{
    const double position = std::clamp(progress, 0.0, 1.0);
    switch (curve)
    {
        case AudioFadeCurve::Smooth:
        {
            const double smooth = position * position * (3.0 - 2.0 * position);
            return static_cast<float>(fadeIn ? smooth : 1.0 - smooth);
        }
        case AudioFadeCurve::EqualPower:
            return static_cast<float>(fadeIn
                ? std::sin(position * 1.5707963267948966)
                : std::cos(position * 1.5707963267948966));
        case AudioFadeCurve::Custom:
        {
            const double shaped = std::pow(position, std::clamp(shape, 0.25, 4.0));
            return static_cast<float>(fadeIn ? shaped : 1.0 - shaped);
        }
        case AudioFadeCurve::Drawn:
        case AudioFadeCurve::Bezier:
        {
            double shaped = position;
            if (drawnPoints != nullptr && drawnPoints->size() > 1)
            {
                const int pointCount = static_cast<int>(drawnPoints->size());
                const double pointPosition = position * (pointCount - 1);
                const int first = static_cast<int>(pointPosition);
                const int second = std::min(pointCount - 1, first + 1);
                const double fraction = pointPosition - first;
                shaped = (*drawnPoints)[first] * (1.0 - fraction) +
                         (*drawnPoints)[second] * fraction;
            }
            return static_cast<float>(fadeIn ? shaped : 1.0 - shaped);
        }
        case AudioFadeCurve::Linear:
        default:
            return static_cast<float>(fadeIn ? position : 1.0 - position);
    }
}

bool MidiNote::operator==(const MidiNote& other) const
{
    return id == other.id && pitch == other.pitch && velocity == other.velocity &&
           probabilityPercent == other.probabilityPercent && ratchetCount == other.ratchetCount &&
           std::abs(startBeat - other.startBeat) < 1.0e-9 &&
           std::abs(lengthBeats - other.lengthBeats) < 1.0e-9 && channel == other.channel;
}

bool MidiPitchBend::operator==(const MidiPitchBend& other) const
{
    return channel == other.channel && value == other.value &&
           std::abs(beat - other.beat) < 1.0e-9;
}

bool TimelineMarker::operator==(const TimelineMarker& other) const
{
    return std::abs(beat - other.beat) < 1.0e-9 && name == other.name;
}

bool TempoChange::operator==(const TempoChange& other) const
{
    return std::abs(beat - other.beat) < 1.0e-9 && std::abs(bpm - other.bpm) < 1.0e-9 &&
           rampFromPrevious == other.rampFromPrevious;
}

bool TimeSignatureChange::operator==(const TimeSignatureChange& other) const
{
    return std::abs(beat - other.beat) < 1.0e-9 && beatsPerBar == other.beatsPerBar &&
           beatUnit == other.beatUnit;
}

double ProjectData::patternLengthBeats() const
{
    return patternLengthBeatsAt(0.0, bars);
}

double ProjectData::patternLengthBeatsAt(double startBeat, int barCount) const
{
    double beat = std::max(0.0, startBeat);
    const double start = beat;
    for (int bar = 0; bar < std::max(0, barCount); ++bar)
    {
        double next = beat + quarterNotesPerBarAtBeat(beat);
        for (const TimeSignatureChange& change : timeSignatureChanges)
            if (change.beat > beat + 1.0e-9 && change.beat < next)
            {
                next = change.beat;
                break;
            }
        if (next <= beat + 1.0e-9)
            break;
        beat = next;
    }
    return beat - start;
}

double ProjectData::barBoundaryAtOrAfter(double beat) const
{
    beat = std::max(0.0, beat);
    double segmentStart = 0.0;
    double barLength = quarterNotesPerBarAtBeat(0.0);
    for (const TimeSignatureChange& change : timeSignatureChanges)
    {
        if (change.beat <= 1.0e-9)
            continue;
        if (change.beat > beat)
            break;
        segmentStart = change.beat;
        barLength = static_cast<double>(change.beatsPerBar) * 4.0 / change.beatUnit;
    }
    if (!std::isfinite(barLength) || barLength <= 0.0)
        return beat;
    double boundary = segmentStart +
        std::ceil((beat - segmentStart) / barLength - 1.0e-9) * barLength;
    for (const TimeSignatureChange& change : timeSignatureChanges)
    {
        if (change.beat > beat + 1.0e-9 && change.beat < boundary - 1.0e-9)
            return change.beat;
    }
    return std::max(beat, boundary);
}

double ProjectData::barBoundaryAtOrBefore(double beat) const
{
    beat = std::max(0.0, beat);
    double segmentStart = 0.0;
    double barLength = quarterNotesPerBarAtBeat(0.0);
    for (const TimeSignatureChange& change : timeSignatureChanges)
    {
        if (change.beat <= 1.0e-9)
            continue;
        if (change.beat > beat)
            break;
        segmentStart = change.beat;
        barLength = static_cast<double>(change.beatsPerBar) * 4.0 / change.beatUnit;
    }
    if (!std::isfinite(barLength) || barLength <= 0.0)
        return beat;
    const double boundary = segmentStart +
        std::floor((beat - segmentStart) / barLength + 1.0e-9) * barLength;
    return std::clamp(boundary, segmentStart, beat);
}

double ProjectData::snapBeatToBarDivision(double beat, int divisions) const
{
    beat = std::max(0.0, beat);
    divisions = std::clamp(divisions, 1, 64);
    const double barStart = barBoundaryAtOrBefore(beat);
    const double barLength = quarterNotesPerBarAtBeat(beat);
    const double step = barLength / divisions;
    if (!std::isfinite(step) || step <= 0.0)
        return beat;
    double snapped = barStart + std::round((beat - barStart) / step) * step;
    for (const TimeSignatureChange& change : timeSignatureChanges)
    {
        if (change.beat > beat && change.beat < snapped - 1.0e-9)
            snapped = change.beat;
    }
    return std::clamp(snapped, 0.0, 16384.0);
}

int ProjectData::barNumberAtBeat(double beat) const
{
    beat = std::max(0.0, beat);
    int bar = 1;
    double barStart = 0.0;
    while (barStart < beat - 1.0e-9 && bar < 65536)
    {
        double next = barStart + quarterNotesPerBarAtBeat(barStart);
        for (const TimeSignatureChange& change : timeSignatureChanges)
        {
            if (change.beat <= barStart + 1.0e-9)
                continue;
            if (change.beat >= next)
                break;
            next = change.beat;
            break;
        }
        if (next <= barStart + 1.0e-9)
            break;
        if (next > beat + 1.0e-9)
            break;
        barStart = next;
        ++bar;
    }
    return bar;
}

double ProjectData::quarterNotesPerBar() const
{
    return static_cast<double>(beatsPerBar) * 4.0 / beatUnit;
}

double ProjectData::quarterNotesPerBarAtBeat(double beat) const
{
    const TimeSignatureChange meter = timeSignatureAtBeat(beat);
    return static_cast<double>(meter.beatsPerBar) * 4.0 / meter.beatUnit;
}

TimeSignatureChange ProjectData::timeSignatureAtBeat(double beat) const
{
    const auto next = std::upper_bound(timeSignatureChanges.cbegin(), timeSignatureChanges.cend(), beat,
        [](double value, const TimeSignatureChange& change) { return value < change.beat; });
    if (next == timeSignatureChanges.cbegin())
        return {0.0, beatsPerBar, beatUnit};
    return *(next - 1);
}

ProjectData::ProjectData()
{
    InstrumentTrack track;
    track.id = 1;
    track.name = QStringLiteral("Synth 1");

    MidiClip clip;
    clip.id = 1;
    clip.name = QStringLiteral("Pattern 1");
    track.clips.push_back(clip);
    tracks.push_back(track);
}

double ProjectData::songLengthBeats() const
{
    double length = patternLengthBeats();
    for (const InstrumentTrack& track : tracks)
    {
        for (const MidiClip& clip : track.clips)
            length = std::max(length, clip.startBeat + clip.lengthBeats);
    }
    return length;
}

double ProjectData::tempoAtBeat(double beat) const
{
    if (beat <= 0.0)
        return tempoBpm;
    const auto next = std::upper_bound(tempoChanges.cbegin(), tempoChanges.cend(), beat,
        [](double value, const TempoChange& change) { return value < change.beat; });
    const double startBeat = next == tempoChanges.cbegin() ? 0.0 : (next - 1)->beat;
    const double startBpm = next == tempoChanges.cbegin() ? tempoBpm : (next - 1)->bpm;
    if (next != tempoChanges.cend() && next->rampFromPrevious && next->beat > startBeat)
        return tempoRampValue(startBpm, next->bpm, (beat - startBeat) / (next->beat - startBeat));
    return startBpm;
}

double ProjectData::secondsAtBeat(double beat) const
{
    if (!std::isfinite(beat))
        return 0.0;
    if (beat <= 0.0)
        return beat * 60.0 / tempoBpm;

    const auto next = std::lower_bound(tempoChanges.cbegin(), tempoChanges.cend(), beat,
        [](const TempoChange& change, double value) { return change.beat < value; });
    if (next == tempoChanges.cend())
    {
        if (tempoChanges.isEmpty())
            return beat * 60.0 / tempoBpm;
        const qsizetype last = tempoChanges.size() - 1;
        return tempoChangeElapsedSeconds[last] +
               (beat - tempoChanges[last].beat) * 60.0 / tempoChanges[last].bpm;
    }
    const qsizetype index = next - tempoChanges.cbegin();
    const double startBeat = index == 0 ? 0.0 : tempoChanges[index - 1].beat;
    const double startBpm = index == 0 ? tempoBpm : tempoChanges[index - 1].bpm;
    const double startSeconds = index == 0 ? 0.0 : tempoChangeElapsedSeconds[index - 1];
    if (next->rampFromPrevious)
        return startSeconds + tempoRampSeconds(beat - startBeat, startBpm, next->bpm,
                                               next->beat - startBeat);
    return startSeconds + (beat - startBeat) * 60.0 / startBpm;
}

double ProjectData::beatAtSeconds(double seconds) const
{
    if (!std::isfinite(seconds))
        return 0.0;
    if (seconds <= 0.0)
        return seconds * tempoBpm / 60.0;

    const auto next = std::upper_bound(tempoChangeElapsedSeconds.cbegin(),
                                       tempoChangeElapsedSeconds.cend(), seconds);
    if (next == tempoChangeElapsedSeconds.cend())
    {
        if (tempoChanges.isEmpty())
            return seconds * tempoBpm / 60.0;
        const qsizetype last = tempoChanges.size() - 1;
        return tempoChanges[last].beat + (seconds - tempoChangeElapsedSeconds[last]) *
               tempoChanges[last].bpm / 60.0;
    }
    const qsizetype index = next - tempoChangeElapsedSeconds.cbegin();
    const double startBeat = index == 0 ? 0.0 : tempoChanges[index - 1].beat;
    const double startBpm = index == 0 ? tempoBpm : tempoChanges[index - 1].bpm;
    const double startSeconds = index == 0 ? 0.0 : tempoChangeElapsedSeconds[index - 1];
    if (tempoChanges[index].rampFromPrevious)
        return startBeat + tempoRampBeats(seconds - startSeconds, startBpm,
                                          tempoChanges[index].bpm,
                                          tempoChanges[index].beat - startBeat);
    return startBeat + (seconds - startSeconds) * startBpm / 60.0;
}

qint64 ProjectData::frameAtBeat(double beat, int sampleRate) const
{
    const double frames = secondsAtBeat(beat) * std::max(1, sampleRate);
    if (!std::isfinite(frames) || frames >= static_cast<double>(std::numeric_limits<qint64>::max()))
        return std::numeric_limits<qint64>::max();
    if (frames <= static_cast<double>(std::numeric_limits<qint64>::min()))
        return std::numeric_limits<qint64>::min();
    return static_cast<qint64>(std::llround(frames));
}

double ProjectData::beatAtFrame(qint64 frame, int sampleRate) const
{
    return beatAtSeconds(static_cast<double>(frame) / std::max(1, sampleRate));
}

qint64 ProjectData::framesBetweenBeats(double startBeat, double endBeat, int sampleRate) const
{
    const qint64 startFrame = frameAtBeat(startBeat, sampleRate);
    const qint64 endFrame = frameAtBeat(endBeat, sampleRate);
    return endFrame >= startFrame ? endFrame - startFrame : 0;
}

void ProjectData::rebuildTempoCache()
{
    tempoChangeElapsedSeconds.resize(tempoChanges.size());
    double elapsedSeconds = 0.0;
    double cursorBeat = 0.0;
    double activeTempo = tempoBpm;
    for (qsizetype index = 0; index < tempoChanges.size(); ++index)
    {
        const TempoChange& change = tempoChanges[index];
        const double segmentBeats = change.beat - cursorBeat;
        elapsedSeconds += change.rampFromPrevious
            ? tempoRampSeconds(segmentBeats, activeTempo, change.bpm, segmentBeats)
            : segmentBeats * 60.0 / activeTempo;
        tempoChangeElapsedSeconds[index] = elapsedSeconds;
        cursorBeat = change.beat;
        activeTempo = change.bpm;
    }
}

InstrumentTrack* ProjectData::findTrack(quint64 trackId)
{
    for (InstrumentTrack& track : tracks)
        if (track.id == trackId) return &track;
    return nullptr;
}

const InstrumentTrack* ProjectData::findTrack(quint64 trackId) const
{
    for (const InstrumentTrack& track : tracks)
        if (track.id == trackId) return &track;
    return nullptr;
}

MidiClip* ProjectData::findClip(quint64 trackId, quint64 clipId)
{
    InstrumentTrack* track = findTrack(trackId);
    if (track == nullptr) return nullptr;
    for (MidiClip& clip : track->clips)
        if (clip.id == clipId) return &clip;
    return nullptr;
}

const MidiClip* ProjectData::findClip(quint64 trackId, quint64 clipId) const
{
    const InstrumentTrack* track = findTrack(trackId);
    if (track == nullptr) return nullptr;
    for (const MidiClip& clip : track->clips)
        if (clip.id == clipId) return &clip;
    return nullptr;
}

quint64 ProjectData::nextTrackId() const
{
    quint64 nextId = 1;
    for (const InstrumentTrack& track : tracks)
        nextId = std::max(nextId, track.id + 1);
    return nextId;
}

quint64 ProjectData::nextClipId() const
{
    quint64 nextId = 1;
    for (const InstrumentTrack& track : tracks)
        for (const MidiClip& clip : track.clips)
            nextId = std::max({nextId, clip.id + 1, clip.audioTakeGroupId + 1});
    return nextId;
}

quint64 ProjectData::nextBusId() const
{
    quint64 nextId = 1;
    for (const MixerBus& bus : buses)
        nextId = std::max(nextId, bus.id + 1);
    return nextId;
}

quint64 ProjectData::nextNoteId() const
{
    quint64 nextId = 1;
    for (const InstrumentTrack& track : tracks)
        for (const MidiClip& clip : track.clips)
            for (const MidiNote& note : clip.notes)
                nextId = std::max(nextId, note.id + 1);
    return nextId;
}

void ProjectData::normalize()
{
    tempoBpm = std::clamp(tempoBpm, 20.0, 300.0);
    beatsPerBar = std::clamp(beatsPerBar, 1, 12);
    if (beatUnit != 1 && beatUnit != 2 && beatUnit != 4 && beatUnit != 8 && beatUnit != 16)
        beatUnit = 4;
    if (timeSignatureChanges.size() > 1000)
        timeSignatureChanges.resize(1000);
    QVector<TimeSignatureChange> validTimeSignatures;
    validTimeSignatures.reserve(timeSignatureChanges.size());
    for (TimeSignatureChange change : timeSignatureChanges)
    {
        if (!std::isfinite(change.beat))
            continue;
        change.beat = std::clamp(change.beat, 0.0, 16384.0);
        change.beatsPerBar = std::clamp(change.beatsPerBar, 1, 12);
        if (change.beatUnit != 1 && change.beatUnit != 2 && change.beatUnit != 4 &&
            change.beatUnit != 8 && change.beatUnit != 16)
            change.beatUnit = 4;
        validTimeSignatures.push_back(change);
    }
    std::stable_sort(validTimeSignatures.begin(), validTimeSignatures.end(),
        [](const TimeSignatureChange& left, const TimeSignatureChange& right) { return left.beat < right.beat; });
    timeSignatureChanges.clear();
    for (const TimeSignatureChange& change : validTimeSignatures)
    {
        if (change.beat < 1.0e-9)
        {
            beatsPerBar = change.beatsPerBar;
            beatUnit = change.beatUnit;
        }
        else if (!timeSignatureChanges.isEmpty() &&
                 std::abs(timeSignatureChanges.back().beat - change.beat) < 1.0e-9)
            timeSignatureChanges.back() = change;
        else
            timeSignatureChanges.push_back(change);
    }
    bars = std::clamp(bars, 1, 64);
    countInBars = std::clamp(countInBars, 0, 4);
    if (tempoChanges.size() > 1000)
        tempoChanges.resize(1000);
    QVector<TempoChange> validTempoChanges;
    validTempoChanges.reserve(tempoChanges.size());
    for (TempoChange change : tempoChanges)
    {
        if (!std::isfinite(change.beat) || !std::isfinite(change.bpm))
            continue;
        change.beat = std::clamp(change.beat, 0.0, 16384.0);
        change.bpm = std::clamp(change.bpm, 20.0, 300.0);
        validTempoChanges.push_back(change);
    }
    std::stable_sort(validTempoChanges.begin(), validTempoChanges.end(),
        [](const TempoChange& left, const TempoChange& right) { return left.beat < right.beat; });
    tempoChanges.clear();
    for (const TempoChange& change : validTempoChanges)
    {
        if (change.beat < 1.0e-9)
        {
            tempoBpm = change.bpm;
            continue;
        }
        if (!tempoChanges.isEmpty() && std::abs(tempoChanges.back().beat - change.beat) < 1.0e-9)
            tempoChanges.back() = change;
        else
            tempoChanges.push_back(change);
    }
    rebuildTempoCache();
    masterGain = std::clamp(masterGain, 0.0f, 1.0f);
    if (!std::isfinite(pitchBendRangeSemitones))
        pitchBendRangeSemitones = 2.0;
    pitchBendRangeSemitones = std::clamp(pitchBendRangeSemitones, 0.01, 48.0);
    pitchBendRangeSemitones = std::round(pitchBendRangeSemitones * 100.0) / 100.0;
    qsizetype effectBudget = 48;
    normalizeEffectChain(masterEffects, effectBudget);
    if (markers.size() > 1000)
        markers.resize(1000);
    QVector<TimelineMarker> validMarkers;
    validMarkers.reserve(markers.size());
    for (TimelineMarker marker : markers)
    {
        if (!std::isfinite(marker.beat))
            continue;
        marker.beat = std::clamp(marker.beat, 0.0, 16384.0);
        marker.name = marker.name.trimmed().left(80);
        if (marker.name.isEmpty())
            marker.name = QStringLiteral("Marker");
        validMarkers.push_back(std::move(marker));
    }
    std::stable_sort(validMarkers.begin(), validMarkers.end(), [](const TimelineMarker& left,
                                                                   const TimelineMarker& right)
    {
        return left.beat < right.beat;
    });
    QVector<TimelineMarker> uniqueMarkers;
    uniqueMarkers.reserve(validMarkers.size());
    for (const TimelineMarker& marker : validMarkers)
    {
        if (!uniqueMarkers.isEmpty() && std::abs(uniqueMarkers.last().beat - marker.beat) < 1.0e-6)
            uniqueMarkers.last() = marker;
        else
            uniqueMarkers.push_back(marker);
    }
    markers = std::move(uniqueMarkers);
    if (buses.size() > 64)
        buses.resize(64);
    std::unordered_set<quint64> usedBusIds;
    quint64 busCandidate = nextBusId();
    for (MixerBus& bus : buses)
    {
        if (bus.id == 0 || usedBusIds.find(bus.id) != usedBusIds.end())
        {
            while (busCandidate == 0 || usedBusIds.find(busCandidate) != usedBusIds.end())
                ++busCandidate;
            bus.id = busCandidate++;
        }
        usedBusIds.insert(bus.id);
        bus.name = bus.name.trimmed().left(80);
        if (bus.name.isEmpty())
            bus.name = QStringLiteral("Bus %1").arg(bus.id);
        normalizeEffectChain(bus.effects, effectBudget);
        if (!std::isfinite(bus.gain)) bus.gain = 1.0f;
        if (!std::isfinite(bus.pan)) bus.pan = 0.0f;
        bus.gain = std::clamp(bus.gain, 0.0f, 2.0f);
        bus.pan = std::clamp(bus.pan, -1.0f, 1.0f);
        normalizeAutomation(bus.gainAutomation, 0.0f, 2.0f);
        normalizeAutomation(bus.panAutomation, -1.0f, 1.0f);
        normalizeAutomation(bus.muteAutomation, 0.0f, 1.0f);
        normalizeAutomation(bus.soloAutomation, 0.0f, 1.0f);
    }
    std::unordered_map<quint64, MixerBus*> busById;
    for (MixerBus& bus : buses)
        busById.emplace(bus.id, &bus);
    for (MixerBus& bus : buses)
    {
        if (bus.outputBusId != 0 && busById.find(bus.outputBusId) == busById.end())
            bus.outputBusId = 0;
    }
    for (MixerBus& bus : buses)
    {
        std::unordered_set<quint64> route;
        route.insert(bus.id);
        quint64 nextBusId = bus.outputBusId;
        while (nextBusId != 0)
        {
            if (route.find(nextBusId) != route.end())
            {
                bus.outputBusId = 0;
                break;
            }
            route.insert(nextBusId);
            const auto nextBus = busById.find(nextBusId);
            if (nextBus == busById.end())
                break;
            nextBusId = nextBus->second->outputBusId;
        }
    }
    std::unordered_map<quint64, std::vector<quint64>> busRouteGraph;
    for (const MixerBus& bus : buses)
    {
        if (bus.outputBusId != 0)
            busRouteGraph[bus.id].push_back(bus.outputBusId);
    }
    for (MixerBus& bus : buses)
    {
        if (bus.sends.size() > 64)
            bus.sends.resize(64);
        QVector<MixerSend> validSends;
        validSends.reserve(bus.sends.size());
        std::unordered_set<quint64> usedSendBuses;
        for (MixerSend send : bus.sends)
        {
            if (send.busId == 0 || send.busId == bus.id || busById.find(send.busId) == busById.end() ||
                !usedSendBuses.insert(send.busId).second)
                continue;
            if (!std::isfinite(send.gain)) send.gain = 0.0f;
            send.gain = std::clamp(send.gain, 0.0f, 1.0f);
            normalizeAutomation(send.automation, 0.0f, 1.0f);

            bool createsCycle = false;
            std::vector<quint64> pending{send.busId};
            std::unordered_set<quint64> visited;
            while (!pending.empty())
            {
                const quint64 routeBusId = pending.back();
                pending.pop_back();
                if (routeBusId == bus.id)
                {
                    createsCycle = true;
                    break;
                }
                if (!visited.insert(routeBusId).second)
                    continue;
                const auto outgoing = busRouteGraph.find(routeBusId);
                if (outgoing != busRouteGraph.end())
                    pending.insert(pending.end(), outgoing->second.begin(), outgoing->second.end());
            }
            if (createsCycle)
                continue;

            validSends.push_back(send);
            busRouteGraph[bus.id].push_back(send.busId);
        }
        bus.sends = std::move(validSends);
    }
    if (tracks.size() > 256)
        tracks.resize(256);

    std::unordered_set<quint64> usedTrackIds;
    std::unordered_set<quint64> usedClipIds;
    quint64 trackCandidate = nextTrackId();
    quint64 clipCandidate = nextClipId();
    quint64 noteCandidate = nextNoteId();
    for (InstrumentTrack& track : tracks)
    {
        if (track.id == 0 || usedTrackIds.find(track.id) != usedTrackIds.end())
        {
            while (usedTrackIds.find(trackCandidate) != usedTrackIds.end() || trackCandidate == 0)
                ++trackCandidate;
            track.id = trackCandidate++;
        }
        usedTrackIds.insert(track.id);
        if (track.outputBusId != 0 && usedBusIds.find(track.outputBusId) == usedBusIds.end())
            track.outputBusId = 0;
        if (track.sends.size() > 64)
            track.sends.resize(64);
        QVector<MixerSend> validSends;
        validSends.reserve(track.sends.size());
        std::unordered_set<quint64> usedSendBuses;
        for (MixerSend send : track.sends)
        {
            if (send.busId == 0 || usedBusIds.find(send.busId) == usedBusIds.end() ||
                !usedSendBuses.insert(send.busId).second)
                continue;
            if (!std::isfinite(send.gain)) send.gain = 0.0f;
            send.gain = std::clamp(send.gain, 0.0f, 1.0f);
            normalizeAutomation(send.automation, 0.0f, 1.0f);
            validSends.push_back(send);
        }
        track.sends = std::move(validSends);
        normalizeEffectChain(track.effects, effectBudget);
        track.instrumentType = static_cast<InstrumentType>(
            std::clamp(static_cast<int>(track.instrumentType), 0, 4));
        if (track.clapParameterValues.size() > 256)
            track.clapParameterValues.resize(256);
        for (float& value : track.clapParameterValues)
            if (!std::isfinite(value))
                value = 0.0f;
        if (track.clapParameterAutomation.size() > 256)
            track.clapParameterAutomation.resize(256);
        for (QVector<AutomationPoint>& points : track.clapParameterAutomation)
            normalizeAutomation(points, -1.0e9f, 1.0e9f);
        if (track.vst3ParameterValues.size() > 256)
            track.vst3ParameterValues.resize(256);
        for (float& value : track.vst3ParameterValues)
        {
            if (!std::isfinite(value))
                value = 0.0f;
            value = std::clamp(value, -1.0e9f, 1.0e9f);
        }
        if (track.vst3ParameterAutomation.size() > 256)
            track.vst3ParameterAutomation.resize(256);
        for (QVector<AutomationPoint>& points : track.vst3ParameterAutomation)
            normalizeAutomation(points, -1.0e9f, 1.0e9f);
        track.gain = std::clamp(track.gain, 0.0f, 1.0f);
        track.pan = std::clamp(track.pan, -1.0f, 1.0f);
        if (!std::isfinite(track.eqLowDb)) track.eqLowDb = 0.0f;
        if (!std::isfinite(track.eqMidDb)) track.eqMidDb = 0.0f;
        if (!std::isfinite(track.eqHighDb)) track.eqHighDb = 0.0f;
        track.eqLowDb = std::clamp(track.eqLowDb, -18.0f, 18.0f);
        track.eqMidDb = std::clamp(track.eqMidDb, -18.0f, 18.0f);
        track.eqHighDb = std::clamp(track.eqHighDb, -18.0f, 18.0f);
        if (!std::isfinite(track.eqLowCrossoverHz)) track.eqLowCrossoverHz = 120.0f;
        if (!std::isfinite(track.eqHighCrossoverHz)) track.eqHighCrossoverHz = 6000.0f;
        track.eqLowCrossoverHz = std::clamp(track.eqLowCrossoverHz, 20.0f, 2000.0f);
        track.eqHighCrossoverHz = std::clamp(track.eqHighCrossoverHz,
            std::max(500.0f, track.eqLowCrossoverHz + 1.0f), 20000.0f);
        for (size_t bandIndex = 0; bandIndex < track.eqParametricBands.size(); ++bandIndex)
        {
            ParametricEqBand& band = track.eqParametricBands[bandIndex];
            if (!std::isfinite(band.frequencyHz)) band.frequencyHz = 1000.0f;
            if (!std::isfinite(band.gainDb)) band.gainDb = 0.0f;
            if (!std::isfinite(band.q)) band.q = 0.707f;
            band.frequencyHz = std::clamp(band.frequencyHz, 20.0f, 20000.0f);
            band.gainDb = std::clamp(band.gainDb, -18.0f, 18.0f);
            band.q = std::clamp(band.q, 0.1f, 10.0f);
            auto& automation = track.eqParametricAutomation[bandIndex];
            normalizeAutomation(automation[0], 20.0f, 20000.0f);
            normalizeAutomation(automation[1], -18.0f, 18.0f);
            normalizeAutomation(automation[2], 0.1f, 10.0f);
        }
        normalizeAutomation(track.volumeAutomation, 0.0f, 1.0f);
        normalizeAutomation(track.panAutomation, -1.0f, 1.0f);
        normalizeAutomation(track.eqLowGainAutomation, -18.0f, 18.0f);
        normalizeAutomation(track.eqMidGainAutomation, -18.0f, 18.0f);
        normalizeAutomation(track.eqHighGainAutomation, -18.0f, 18.0f);
        normalizeAutomation(track.eqLowCrossoverAutomation, 20.0f, 2000.0f);
        normalizeAutomation(track.eqHighCrossoverAutomation, 500.0f, 20000.0f);
        normalizeAutomation(track.synthFilterCutoffAutomation, 20.0f, 20000.0f);
        normalizeAutomation(track.synthFilterResonanceAutomation, 0.1f, 10.0f);
        track.waveform = static_cast<SynthWaveform>(std::clamp(static_cast<int>(track.waveform), 0, 3));
        track.secondOscillatorWaveform = static_cast<SynthWaveform>(
            std::clamp(static_cast<int>(track.secondOscillatorWaveform), 0, 3));
        if (!std::isfinite(track.secondOscillatorDetuneCents))
            track.secondOscillatorDetuneCents = 0.0f;
        if (!std::isfinite(track.secondOscillatorMix))
            track.secondOscillatorMix = 0.5f;
        track.secondOscillatorDetuneCents = std::clamp(track.secondOscillatorDetuneCents, -100.0f, 100.0f);
        track.secondOscillatorMix = std::clamp(track.secondOscillatorMix, 0.0f, 1.0f);
        track.synthFilterType = static_cast<SynthFilterType>(
            std::clamp(static_cast<int>(track.synthFilterType), 0, 3));
        if (!std::isfinite(track.synthFilterCutoffHz))
            track.synthFilterCutoffHz = 20000.0f;
        if (!std::isfinite(track.synthFilterResonance))
            track.synthFilterResonance = 0.707f;
        track.synthFilterCutoffHz = std::clamp(track.synthFilterCutoffHz, 20.0f, 20000.0f);
        track.synthFilterResonance = std::clamp(track.synthFilterResonance, 0.1f, 10.0f);
        if (!std::isfinite(track.synthFilterEnvelopeAmountSemitones))
            track.synthFilterEnvelopeAmountSemitones = 24.0f;
        if (!std::isfinite(track.synthFilterEnvelopeAttackSeconds))
            track.synthFilterEnvelopeAttackSeconds = 0.008f;
        if (!std::isfinite(track.synthFilterEnvelopeDecaySeconds))
            track.synthFilterEnvelopeDecaySeconds = 0.2f;
        if (!std::isfinite(track.synthFilterEnvelopeSustainLevel))
            track.synthFilterEnvelopeSustainLevel = 0.0f;
        if (!std::isfinite(track.synthFilterEnvelopeReleaseSeconds))
            track.synthFilterEnvelopeReleaseSeconds = 0.2f;
        track.synthFilterEnvelopeAmountSemitones = std::clamp(
            track.synthFilterEnvelopeAmountSemitones, -48.0f, 48.0f);
        track.synthFilterEnvelopeAttackSeconds = std::clamp(
            track.synthFilterEnvelopeAttackSeconds, 0.0f, 5.0f);
        track.synthFilterEnvelopeDecaySeconds = std::clamp(
            track.synthFilterEnvelopeDecaySeconds, 0.0f, 5.0f);
        track.synthFilterEnvelopeSustainLevel = std::clamp(
            track.synthFilterEnvelopeSustainLevel, 0.0f, 1.0f);
        track.synthFilterEnvelopeReleaseSeconds = std::clamp(
            track.synthFilterEnvelopeReleaseSeconds, 0.0f, 5.0f);
        if (!std::isfinite(track.attackSeconds)) track.attackSeconds = 0.008f;
        if (!std::isfinite(track.decaySeconds)) track.decaySeconds = 0.0f;
        if (!std::isfinite(track.sustainLevel)) track.sustainLevel = 1.0f;
        if (!std::isfinite(track.releaseSeconds)) track.releaseSeconds = 0.06f;
        track.attackSeconds = std::clamp(track.attackSeconds, 0.0f, 5.0f);
        track.decaySeconds = std::clamp(track.decaySeconds, 0.0f, 5.0f);
        track.sustainLevel = std::clamp(track.sustainLevel, 0.0f, 1.0f);
        track.releaseSeconds = std::clamp(track.releaseSeconds, 0.0f, 10.0f);
        for (DrumPadSample& drumSample : track.drumSamples)
        {
            if (drumSample.source != nullptr && drumSample.path.isEmpty())
                drumSample.path = drumSample.source->path;
            if (drumSample.veryLowVelocitySource != nullptr && drumSample.veryLowVelocityPath.isEmpty())
                drumSample.veryLowVelocityPath = drumSample.veryLowVelocitySource->path;
            if (drumSample.lowVelocitySource != nullptr && drumSample.lowVelocityPath.isEmpty())
                drumSample.lowVelocityPath = drumSample.lowVelocitySource->path;
            if (drumSample.highVelocitySource != nullptr && drumSample.highVelocityPath.isEmpty())
                drumSample.highVelocityPath = drumSample.highVelocitySource->path;
            if (drumSample.veryHighVelocitySource != nullptr && drumSample.veryHighVelocityPath.isEmpty())
                drumSample.veryHighVelocityPath = drumSample.veryHighVelocitySource->path;
            drumSample.lowVelocityThreshold = std::clamp(drumSample.lowVelocityThreshold, 1, 126);
            drumSample.highVelocityThreshold = std::clamp(
                drumSample.highVelocityThreshold, drumSample.lowVelocityThreshold + 1, 127);
            drumSample.veryLowVelocityThreshold = std::clamp(
                drumSample.veryLowVelocityThreshold, 1, drumSample.lowVelocityThreshold);
            drumSample.veryHighVelocityThreshold = std::clamp(
                drumSample.veryHighVelocityThreshold, drumSample.highVelocityThreshold, 127);
            if (!std::isfinite(drumSample.tuningSemitones))
                drumSample.tuningSemitones = 0.0f;
            if (!std::isfinite(drumSample.startPositionPercent))
                drumSample.startPositionPercent = 0.0f;
            if (!std::isfinite(drumSample.endPositionPercent))
                drumSample.endPositionPercent = 100.0f;
            drumSample.tuningSemitones = std::clamp(drumSample.tuningSemitones, -24.0f, 24.0f);
            drumSample.startPositionPercent = std::clamp(drumSample.startPositionPercent, 0.0f, 99.0f);
            drumSample.endPositionPercent = std::clamp(drumSample.endPositionPercent,
                drumSample.startPositionPercent + 0.1f, 100.0f);
            if (!std::isfinite(drumSample.attackSeconds)) drumSample.attackSeconds = 0.008f;
            if (!std::isfinite(drumSample.decaySeconds)) drumSample.decaySeconds = 0.0f;
            if (!std::isfinite(drumSample.sustainLevel)) drumSample.sustainLevel = 1.0f;
            if (!std::isfinite(drumSample.releaseSeconds)) drumSample.releaseSeconds = 0.06f;
            drumSample.attackSeconds = std::clamp(drumSample.attackSeconds, 0.0f, 5.0f);
            drumSample.decaySeconds = std::clamp(drumSample.decaySeconds, 0.0f, 5.0f);
            drumSample.sustainLevel = std::clamp(drumSample.sustainLevel, 0.0f, 1.0f);
            drumSample.releaseSeconds = std::clamp(drumSample.releaseSeconds, 0.0f, 5.0f);
            for (DrumLayerSettings& settings : drumSample.velocityLayerSettings)
            {
                if (!std::isfinite(settings.tuningSemitones)) settings.tuningSemitones = 0.0f;
                if (!std::isfinite(settings.startPositionPercent)) settings.startPositionPercent = 0.0f;
                if (!std::isfinite(settings.endPositionPercent)) settings.endPositionPercent = 100.0f;
                if (!std::isfinite(settings.attackSeconds)) settings.attackSeconds = 0.008f;
                if (!std::isfinite(settings.decaySeconds)) settings.decaySeconds = 0.0f;
                if (!std::isfinite(settings.sustainLevel)) settings.sustainLevel = 1.0f;
                if (!std::isfinite(settings.releaseSeconds)) settings.releaseSeconds = 0.06f;
                settings.tuningSemitones = std::clamp(settings.tuningSemitones, -24.0f, 24.0f);
                settings.startPositionPercent = std::clamp(settings.startPositionPercent, 0.0f, 99.0f);
                settings.endPositionPercent = std::clamp(settings.endPositionPercent,
                    settings.startPositionPercent + 0.1f, 100.0f);
                settings.attackSeconds = std::clamp(settings.attackSeconds, 0.0f, 5.0f);
                settings.decaySeconds = std::clamp(settings.decaySeconds, 0.0f, 5.0f);
                settings.sustainLevel = std::clamp(settings.sustainLevel, 0.0f, 1.0f);
                settings.releaseSeconds = std::clamp(settings.releaseSeconds, 0.0f, 5.0f);
            }
        }
        if (track.clips.size() > 10000)
            track.clips.resize(10000);

        std::unordered_set<quint64> usedNotes;
        QVector<MidiClip> validClips;
        validClips.reserve(track.clips.size());
        for (MidiClip clip : track.clips)
        {
            if (!std::isfinite(clip.startBeat) || !std::isfinite(clip.lengthBeats))
                continue;
            if (clip.id == 0 || usedClipIds.find(clip.id) != usedClipIds.end())
            {
                while (usedClipIds.find(clipCandidate) != usedClipIds.end() || clipCandidate == 0)
                    ++clipCandidate;
                clip.id = clipCandidate++;
            }
            usedClipIds.insert(clip.id);
            clip.startBeat = std::clamp(clip.startBeat, 0.0, 16384.0);
            clip.lengthBeats = std::clamp(clip.lengthBeats, 0.25, 16384.0);
            clip.stepsPerBar = normalizedStepsPerBar(clip.stepsPerBar);
            clip.audioStartFrame = std::max<qint64>(0, clip.audioStartFrame);
            if (!std::isfinite(clip.audioFadeInBeats))
                clip.audioFadeInBeats = 0.0;
            if (!std::isfinite(clip.audioFadeOutBeats))
                clip.audioFadeOutBeats = 0.0;
            clip.audioFadeInBeats = std::clamp(clip.audioFadeInBeats, 0.0, clip.lengthBeats);
            clip.audioFadeOutBeats = std::clamp(clip.audioFadeOutBeats, 0.0, clip.lengthBeats);
            clip.audioFadeInCurve = static_cast<AudioFadeCurve>(std::clamp(
                static_cast<int>(clip.audioFadeInCurve), 0, 5));
            clip.audioFadeOutCurve = static_cast<AudioFadeCurve>(std::clamp(
                static_cast<int>(clip.audioFadeOutCurve), 0, 5));
            if (!std::isfinite(clip.audioFadeInShape))
                clip.audioFadeInShape = 1.0;
            if (!std::isfinite(clip.audioFadeOutShape))
                clip.audioFadeOutShape = 1.0;
            clip.audioFadeInShape = std::clamp(clip.audioFadeInShape, 0.25, 4.0);
            clip.audioFadeOutShape = std::clamp(clip.audioFadeOutShape, 0.25, 4.0);
            if (clip.audioFadeInCurve == AudioFadeCurve::Drawn &&
                !clip.audioFadeInCurvePoints.isEmpty())
                normalizeAudioFadeCurvePoints(clip.audioFadeInCurvePoints);
            if (clip.audioFadeOutCurve == AudioFadeCurve::Drawn &&
                !clip.audioFadeOutCurvePoints.isEmpty())
                normalizeAudioFadeCurvePoints(clip.audioFadeOutCurvePoints);
            if (clip.audioFadeInCurve == AudioFadeCurve::Drawn &&
                clip.audioFadeInCurvePoints.isEmpty())
                clip.audioFadeInCurvePoints = defaultAudioFadeCurvePoints();
            if (clip.audioFadeOutCurve == AudioFadeCurve::Drawn &&
                clip.audioFadeOutCurvePoints.isEmpty())
                clip.audioFadeOutCurvePoints = defaultAudioFadeCurvePoints();
            if (clip.audioFadeInCurve == AudioFadeCurve::Bezier)
            {
                normalizeAudioFadeBezierHandles(clip.audioFadeInBezierHandles);
                clip.audioFadeInCurvePoints = audioFadeBezierCurvePoints(clip.audioFadeInBezierHandles);
            }
            if (clip.audioFadeOutCurve == AudioFadeCurve::Bezier)
            {
                normalizeAudioFadeBezierHandles(clip.audioFadeOutBezierHandles);
                clip.audioFadeOutCurvePoints = audioFadeBezierCurvePoints(clip.audioFadeOutBezierHandles);
            }
            if (clip.audioSource != nullptr)
            {
                if (clip.audioSourcePath.isEmpty())
                    clip.audioSourcePath = clip.audioSource->path;
                clip.audioStartFrame = std::min(clip.audioStartFrame,
                                                std::max<qint64>(0, clip.audioSource->frameCount() - 1));
            }
            if (clip.notes.size() > 100000)
                clip.notes.resize(100000);

            QVector<MidiNote> validNotes;
            validNotes.reserve(clip.notes.size());
            for (MidiNote note : clip.notes)
            {
                if (!std::isfinite(note.startBeat) || !std::isfinite(note.lengthBeats))
                    continue;
                note.pitch = std::clamp(note.pitch, 0, 127);
                note.velocity = std::clamp(note.velocity, 1, 127);
                note.probabilityPercent = std::clamp(note.probabilityPercent, 0, 100);
                note.ratchetCount = std::clamp(note.ratchetCount, 1, 8);
                note.channel = std::clamp(note.channel, -1, 15);
                const double minimumLength = 1.0 / 64.0;
                note.startBeat = std::clamp(note.startBeat, 0.0,
                                            std::max(0.0, clip.lengthBeats - minimumLength));
                note.lengthBeats = std::clamp(note.lengthBeats, minimumLength,
                                              std::max(minimumLength, clip.lengthBeats - note.startBeat));
                if (note.id == 0 || usedNotes.find(note.id) != usedNotes.end())
                {
                    while (usedNotes.find(noteCandidate) != usedNotes.end() || noteCandidate == 0)
                        ++noteCandidate;
                    note.id = noteCandidate++;
                }
                usedNotes.insert(note.id);
                validNotes.push_back(note);
            }
            std::sort(validNotes.begin(), validNotes.end(), [](const MidiNote& left, const MidiNote& right)
            {
                if (left.startBeat != right.startBeat) return left.startBeat < right.startBeat;
                return left.pitch < right.pitch;
            });
            clip.notes = std::move(validNotes);

            if (clip.pitchBends.size() > 100000)
                clip.pitchBends.resize(100000);
            QVector<MidiPitchBend> validPitchBends;
            validPitchBends.reserve(clip.pitchBends.size());
            for (MidiPitchBend bend : clip.pitchBends)
            {
                if (!std::isfinite(bend.beat))
                    continue;
                bend.channel = std::clamp(bend.channel, 0, 15);
                bend.value = std::clamp(bend.value, 0, 16383);
                bend.beat = std::clamp(bend.beat, 0.0, clip.lengthBeats);
                validPitchBends.push_back(bend);
            }
            std::stable_sort(validPitchBends.begin(), validPitchBends.end(),
                [](const MidiPitchBend& left, const MidiPitchBend& right)
                {
                    return left.beat < right.beat;
                });
            clip.pitchBends = std::move(validPitchBends);
            validClips.push_back(std::move(clip));
        }

        std::sort(validClips.begin(), validClips.end(), [](const MidiClip& left, const MidiClip& right)
        {
            return left.startBeat < right.startBeat;
        });
        track.clips = std::move(validClips);
    }
}
